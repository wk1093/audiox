const assert = require('node:assert/strict');
const { test } = require('node:test');
const {
  formatMessage, notify, resolve: resolveNotice, confirm: confirmAction,
  probeDevice, waitForRestart, runTask, taskProgress, isBusy,
  startConnectionMonitor,
} = require('../web/ui.js');

test('friendly errors retain useful API messages without exposing JSON or stack traces', () => {
  assert.equal(formatMessage('save failed: 400 {"error":"Choose a valid MIDI note."}'),
    'Save failed: Choose a valid MIDI note.');
  assert.equal(formatMessage('Error: effects load failed: 500 {"error":"Device is busy."}'),
    'Effects load failed: Device is busy.');
  assert.equal(formatMessage('{"error":{"message":"Not enough storage."},"trace":"private"}'),
    'Not enough storage.');
  assert.equal(formatMessage('{"message":"Try another file."}'), 'Try another file.');
  assert.equal(formatMessage('{"ok":false,"debug":{"internal":"data"}}'), 'The request could not be completed.');
  assert.equal(formatMessage('upload failed: {"broken":'), 'Upload failed.');
  assert.equal(formatMessage(new Error('Invalid channel.\n    at save (app.js:10:2)')), 'Invalid channel.');
  assert.equal(formatMessage('TypeError: Invalid channel.\n    at save (app.js:10:2)'), 'Invalid channel.');
});

test('HTML responses and invalid JSON become actionable errors, not markup or parser errors', () => {
  for (const body of ['<!doctype html><html><body>Internal error</body></html>', '<h1>Gateway error</h1>']) {
    const message = formatMessage(`config save failed: 502 ${body}`);
    assert.match(message, /^Config save failed\./);
    assert.match(message, /Try again/);
    assert.doesNotMatch(message, /[<>]|Internal error|Gateway error/);
  }
  assert.match(formatMessage('upload failed: 413 <h1>Too large</h1>'), /file is too large/);
  assert.match(formatMessage('load failed: 404 <html>Missing</html>'), /newer device firmware/);
  assert.match(formatMessage('save failed: 503 <html>Busy</html>'), /busy or restarting/);
  assert.match(formatMessage('SyntaxError: Unexpected token \'<\', "<!doctype "... is not valid JSON'), /unexpected response/);
  assert.match(formatMessage('save error: {"error":"<html>private</html>"}'), /unexpected response/);
});

test('network failures, plain validation messages and very long errors are readable', () => {
  assert.match(formatMessage('routing load error: TypeError: Failed to fetch'), /Check your connection/);
  assert.match(formatMessage(new TypeError('NetworkError when attempting to fetch resource.')), /Cannot reach AudioX/);
  assert.equal(formatMessage('pick a file first'), 'Pick a file first');
  assert.equal(formatMessage('Backup checksum mismatch'), 'Backup checksum mismatch');
  assert.equal(formatMessage('upload failed'), 'Upload failed');
  assert.equal(formatMessage('uploaded kick[1].wav'), 'Uploaded kick[1].wav');
  assert.equal(formatMessage('x'.repeat(1000)).length, 300);
  assert.equal(formatMessage(''), 'The request could not be completed.');
});

function dialogUI() {
  class Control extends EventTarget {
    focus() { this.focused = true; }
    click() { this.dispatchEvent(new Event('click')); }
  }
  const elements = new Map(['confirm-dialog', 'confirm-accept', 'confirm-cancel',
    'confirm-title', 'confirm-message'].map((id) => [id, new Control()]));
  const dialog = elements.get('confirm-dialog');
  dialog.showModal = () => { dialog.open = true; };
  dialog.close = () => { dialog.open = false; };
  const previous = new Control();
  global.document = { getElementById: (id) => elements.get(id), activeElement: previous };
  return { elements, dialog, previous };
}

test('custom confirmations default to cancel, restore focus and do not accumulate listeners', async () => {
  const { elements, dialog, previous } = dialogUI();
  let result = confirmAction('Replace all settings?', { title: 'Restore?', label: 'Restore and reboot' });
  assert.equal(dialog.open, true);
  assert.equal(elements.get('confirm-title').textContent, 'Restore?');
  assert.equal(elements.get('confirm-accept').textContent, 'Restore and reboot');
  assert.equal(elements.get('confirm-cancel').focused, true);
  assert.equal(await confirmAction('Second request'), false);
  elements.get('confirm-cancel').click();
  assert.equal(await result, false);
  assert.equal(dialog.open, false);
  assert.equal(previous.focused, true);

  result = confirmAction('Install?');
  elements.get('confirm-accept').click();
  assert.equal(await result, true);
  result = confirmAction('Shutdown?');
  const escape = new Event('cancel', { cancelable: true });
  dialog.dispatchEvent(escape);
  assert.equal(await result, false);
  assert.equal(escape.defaultPrevented, true);

  result = confirmAction('Restart?');
  const key = new Event('keydown', { cancelable: true });
  Object.defineProperty(key, 'key', { value: 'Escape' });
  dialog.dispatchEvent(key);
  assert.equal(await result, false);
  assert.equal(key.defaultPrevented, true);
});

test('beforeunload warns only while tracked changes remain unsaved', () => {
  const event = { prevented: false, preventDefault() { this.prevented = true; } };
  const { setUnsaved, clearUnsaved, hasUnsavedChanges, handleBeforeUnload } = require('../web/ui.js');
  clearUnsaved();
  assert.equal(hasUnsavedChanges(), false);
  handleBeforeUnload(event);
  assert.equal(event.prevented, false);

  setUnsaved('routing');
  assert.equal(hasUnsavedChanges(), true);
  assert.equal(hasUnsavedChanges('routing'), true);
  handleBeforeUnload(event);
  assert.equal(event.prevented, true);
  assert.equal(event.returnValue, '');

  setUnsaved('routing', false);
  assert.equal(hasUnsavedChanges(), false);
  event.prevented = false;
  delete event.returnValue;
  handleBeforeUnload(event);
  assert.equal(event.prevented, false);

  setUnsaved('macro-editor');
  clearUnsaved();
  assert.equal(hasUnsavedChanges(), false);
});

test('notifications deduplicate, stay bounded, preserve errors and coalesce progress', async (t) => {
  const timers = new Map();
  let nextTimer = 0;
  t.mock.method(global, 'setTimeout', (callback) => {
    timers.set(++nextTimer, callback);
    return nextTimer;
  });
  t.mock.method(global, 'clearTimeout', (timer) => timers.delete(timer));
  class Element extends EventTarget {
    children = [];
    dataset = {};
    append(...items) {
      for (const item of items) {
        item.parent = this;
        this.children.push(item);
      }
    }
    setAttribute() {}
    get firstElementChild() { return this.children[0]; }
    remove() { this.parent.children = this.parent.children.filter((item) => item !== this); }
    querySelector(selector) {
      const tone = selector.match(/data-tone="([^"]+)"/)?.[1];
      return this.children.find((item) => item.dataset.tone === tone) || null;
    }
  }
  const host = new Element();
  global.document = { getElementById: () => host, createElement: () => new Element() };
  notify('Same error', 'warn');
  notify('Same error', 'warn');
  assert.equal(host.children.length, 1);
  assert.equal(timers.size, 0);
  for (let i = 0; i < 8; i++) notify(`Error ${i}`, 'warn');
  assert.equal(host.children.length, 4);
  assert.ok(host.children.every((notice) => notice.children.length === 2));
  notify('Uploading 1%');
  notify('Uploading 2%');
  assert.equal(host.children.length, 4);
  assert.equal(host.children.filter((notice) => notice.dataset.tone === 'info').length, 1);
  notify('Saved', 'ok');
  notify('Saved again', 'ok');
  assert.equal(host.children.length, 4);
  assert.equal(host.children.filter((notice) => notice.dataset.tone === 'ok').length, 1);
  const success = host.children.at(-1);
  success.dispatchEvent(new Event('mouseenter'));
  const active = timers.size;
  success.dispatchEvent(new Event('mouseleave'));
  assert.equal(timers.size, active + 1);
  for (const expire of timers.values()) expire();
  assert.ok(host.children.every((notice) => notice.dataset.tone === 'warn'));
  host.children[0].children[1].dispatchEvent(new Event('click'));
  assert.equal(host.children.length, 2);
  notify('TypeError: Failed to fetch', 'warn');
  notify('Checksum mismatch', 'warn', 'upload');
  resolveNotice('connection');
  assert.ok(host.children.every((notice) => notice.dataset.key !== 'connection'));
  assert.ok(host.children.some((notice) => notice.dataset.key === 'upload'));
  notify('Upload completed', 'ok', 'upload');
  assert.equal(host.children.filter((notice) => notice.dataset.key === 'upload').length, 1);
  assert.equal(host.children.at(-1).dataset.tone, 'ok');
  let reachable = false, reconnects = 0;
  document.visibilityState = 'visible';
  t.mock.method(global, 'fetch', async () => {
    if (!reachable) throw new TypeError('Failed to fetch');
    return infoResponse(100);
  });
  notify('Restore checksum mismatch', 'warn', 'restore');
  const stop = startConnectionMonitor(() => { reconnects++; });
  await timers.get(nextTimer)();
  assert.ok(host.children.every((notice) => notice.dataset.key !== 'connection'));
  await timers.get(nextTimer)();
  assert.ok(host.children.some((notice) => notice.dataset.key === 'connection'));
  reachable = true;
  await timers.get(nextTimer)();
  assert.equal(reconnects, 1);
  assert.ok(host.children.every((notice) => notice.dataset.key !== 'connection'));
  assert.ok(host.children.some((notice) => notice.dataset.key === 'restore'));
  stop();
});

const infoResponse = (uptime_secs, version = '1.11.0') => ({
  ok: true, json: async () => ({ uptime_secs, version }),
});

test('device probes bypass caches, validate readiness and use a bounded request timeout', async () => {
  const info = await probeDevice(async (url, options) => {
    assert.equal(url, '/api/system/info');
    assert.equal(options.cache, 'no-store');
    assert.ok(options.signal instanceof AbortSignal);
    return infoResponse(100);
  });
  assert.equal(info.uptime_secs, 100);
  await assert.rejects(probeDevice(async () => ({ ok: false, status: 503 })), /503/);
  for (const uptime of [undefined, -1, '100', NaN]) {
    await assert.rejects(probeDevice(async () => infoResponse(uptime)), /incomplete device information/);
  }
});

test('restart waits through old responses and outages, then detects a new boot at one-second intervals', async () => {
  let time = 0;
  const progress = [], pauses = [];
  const replies = [infoResponse(100), infoResponse(101), new TypeError('Failed to fetch'), infoResponse(2)];
  let requests = 0;
  const result = await waitForRestart({ uptime_secs: 100 }, {
    startedAt: 0,
    now: () => time,
    pause: async (ms) => { pauses.push(ms); time += ms; },
    fetcher: async () => {
      const reply = replies[requests++];
      if (reply instanceof Error) throw reply;
      return reply;
    },
    onProgress: (text) => progress.push(text),
  });
  assert.equal(result.uptime_secs, 2);
  assert.equal(requests, 4);
  assert.deepEqual(pauses, [1000, 1000, 1000]);
  assert.match(progress[0], /previous session is still online/);
  assert.match(progress[2], /offline or starting up/);
});

test('a brief connection loss is not mistaken for a restart; a fast boot need not be seen offline', async () => {
  let requests = 0, time = 0;
  await waitForRestart({ uptime_secs: 100 }, {
    startedAt: 0, now: () => time, pause: async (ms) => { time += ms; },
    fetcher: async () => {
      requests++;
      if (requests === 1) throw new TypeError('Failed to fetch');
      return infoResponse(requests === 2 ? 101 : 1);
    },
  });
  assert.equal(requests, 3);
  await waitForRestart({ uptime_secs: 100 }, {
    startedAt: 0, now: () => 0, fetcher: async () => infoResponse(1),
    pause: () => assert.fail('Fast reboot should be detected immediately'),
  });
});

test('restart readiness ignores HTTP errors and malformed data; stopping is explicit', async () => {
  let requests = 0, time = 0;
  await waitForRestart({ uptime_secs: 100 }, {
    startedAt: 0, now: () => time, pause: async (ms) => { time += ms; },
    fetcher: async () => {
      requests++;
      if (requests === 1) return { ok: false, status: 503 };
      return infoResponse(requests === 2 ? undefined : 1);
    },
  });
  assert.equal(requests, 3);
  const controller = new AbortController();
  await assert.rejects(waitForRestart({ uptime_secs: 100 }, {
    signal: controller.signal, startedAt: 0, now: () => 0,
    fetcher: async () => infoResponse(100),
    pause: async () => controller.abort(),
  }), /Stopped waiting.*may still be running/);
});

function loadingUI() {
  class Control extends EventTarget {
    children = [];
    focus() { global.document.activeElement = this; }
    showModal() { this.open = true; }
    close() { this.open = false; }
  }
  const elements = new Map(['loading-dialog', 'loading-stop', 'loading-title', 'loading-message', 'notifications']
    .map((id) => [id, new Control()]));
  const previous = new Control();
  global.document = { getElementById: (id) => elements.get(id), activeElement: previous };
  return { elements, previous };
}

test('loading modals remain open for the entire action, reject concurrent work and clean up failures', async () => {
  const { elements, previous } = loadingUI();
  let finish;
  const pending = runTask('Uploading sound', () => new Promise((resolve) => { finish = resolve; }));
  assert.equal(isBusy(), true);
  assert.equal(elements.get('loading-dialog').open, true);
  taskProgress('Uploading 40%');
  assert.equal(elements.get('loading-message').textContent, 'Uploading 40%');
  const escape = new Event('cancel', { cancelable: true });
  elements.get('loading-dialog').dispatchEvent(escape);
  assert.equal(escape.defaultPrevented, true);
  await assert.rejects(runTask('Second action', () => {}), /already running/);
  finish(42);
  assert.equal(await pending, 42);
  assert.equal(isBusy(), false);
  assert.equal(elements.get('loading-dialog').open, false);
  assert.equal(global.document.activeElement, previous);
  await assert.rejects(runTask('Failing action', () => { throw new Error('Rejected'); }), /Rejected/);
  assert.equal(isBusy(), false);
  assert.equal(elements.get('loading-dialog').open, false);
});

test('restart tasks reload only after the new boot is ready, not after request acceptance', async (t) => {
  const { elements } = loadingUI();
  let requests = 0, reloads = 0;
  t.mock.method(global, 'fetch', async () => infoResponse(++requests === 1 ? 100 : 1));
  const previousLocation = global.location;
  global.location = { reload: () => { reloads++; } };
  t.after(() => {
    if (previousLocation === undefined) delete global.location;
    else global.location = previousLocation;
  });
  let actionComplete = false;
  await runTask('Restarting AudioX', async () => {
    assert.equal(reloads, 0);
    assert.equal(elements.get('loading-stop').hidden, true);
    actionComplete = true;
  }, { restart: true });
  assert.equal(actionComplete, true);
  assert.equal(requests, 2);
  assert.equal(reloads, 1);
  assert.equal(elements.get('loading-dialog').open, false);
  assert.equal(isBusy(), false);
});
