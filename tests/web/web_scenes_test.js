const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const { apiContext } = require('./support/api-context.js');

class Element {
  constructor() { this.children = []; this.events = {}; this.dataset = {}; this.value = ''; }
  append(...items) { this.children.push(...items); }
  replaceChildren(...items) { this.children = items; }
  addEventListener(name, callback) { this.events[name] = callback; }
  setAttribute(name, value) { this[name] = value; }
  checkValidity() { return this.value !== '' && Number(this.value) >= Number(this.min) && Number(this.value) <= Number(this.max); }
  async click() { if (!this.disabled) this.events.click?.(); await tick(); }
}
const tick = () => new Promise((resolve) => setImmediate(resolve));
async function fixture() {
  const controls = Object.fromEntries(['scene-status', 'scene-list', 'scene-name', 'scene-capture', 'scene-refresh']
    .map((id) => [id, new Element()]));
  const host = new Element();
  host.querySelector = (selector) => controls[selector.slice(1)];
  host.querySelectorAll = () => [controls['scene-capture'], controls['scene-refresh'],
    ...controls['scene-list'].children.flatMap((row) => row.children.filter((child) => child.type === 'button'))];
  const data = { scenes: [{ id: 'clean', name: '<script>voice</script>', effects: 1, routes: 2, volumes: 1,
    channel: -1, note: -1 }], sequence: 0, active: '' };
  const calls = [];
  const events = [];
  let poll;
  let failure = false;
  let dirty = false;
  let deviceBusy = false;
  const notifications = [];
  const confirmations = [];
  let tasks = 0;
  const window = {
    confirm: () => true,
    crypto: { getRandomValues: (array) => { array[0] = 123; } },
    dispatchEvent: (event) => events.push(event.type),
    setInterval: (callback) => { poll = callback; },
  };
  const document = { readyState: 'complete', hidden: false, createElement: () => new Element(),
    getElementById: (id) => id === 'scenes-panel' ? host : null };
  const AudioxUI = {
    hasUnsavedChanges: (key) => key === 'routing' && dirty,
    isBusy: () => deviceBusy,
    notify: (...args) => notifications.push(args),
    confirm: async (...args) => { confirmations.push(args); return window.confirm(); },
    runTask: async (title, action) => {
      if (deviceBusy) throw new Error('Another device action is already running.');
      tasks++;
      deviceBusy = true;
      try { return await action(); } finally { deviceBusy = false; }
    },
  };
  const context = { window, document, AudioxUI, TextEncoder, URLSearchParams, Uint32Array, Date,
    CustomEvent: class { constructor(type) { this.type = type; } },
    fetch: async (url, options) => {
      calls.push({ url, options });
      if (failure) return { ok: false, json: async () => ({ error: 'Disk unavailable' }) };
      if (url.startsWith('/api/scenes/recall?')) { data.sequence++; data.active = 'clean'; }
      return { ok: true, json: async () => structuredClone(url === '/api/scenes' ? data : { ok: true }) };
    },
  };
  vm.runInContext(fs.readFileSync('web/embedded/scenes.js', 'utf8'), apiContext(context));
  await tick();
  return { controls, data, calls, events, poll: () => poll(), fail: () => { failure = true; }, window,
    dirty: (value) => { dirty = value; }, deviceBusy: (value) => { deviceBusy = value; },
    notifications, confirmations, tasks: () => tasks };
}
const button = (row, text) => row.children.find((child) => child.textContent === text);

test('scene UI captures names as UTF-8 hex and never renders user names as HTML', async () => {
  const f = await fixture();
  const row = f.controls['scene-list'].children[0];
  assert.equal(row.children[0].children[0].textContent, '<script>voice</script>');
  f.controls['scene-name'].value = 'Café';
  await f.controls['scene-capture'].click();
  const call = f.calls.find((entry) => entry.url.startsWith('/api/scenes/save'));
  const query = new URL(call.url, 'http://audiox').searchParams;
  assert.equal(query.get('name_hex'), Buffer.from('Café').toString('hex'));
  assert.equal(query.get('channel'), '-1');
  assert.equal(query.get('note'), '-1');
  assert.equal(call.options.method, 'POST');
  assert.equal(f.tasks(), 1);
});
test('recall and MIDI-triggered sequence updates refresh the live desk once', async () => {
  const f = await fixture();
  await button(f.controls['scene-list'].children[0], 'Recall').click();
  assert.deepEqual(f.events, ['audiox-scene-recalled']);
  f.data.sequence++;
  f.poll();
  await tick();
  assert.deepEqual(f.events, ['audiox-scene-recalled', 'audiox-scene-recalled']);
});
test('unchanged polls preserve in-progress MIDI input and binding validates channel range', async () => {
  const f = await fixture();
  const row = f.controls['scene-list'].children[0];
  const channel = row.children.find((child) => child.textContent === 'MIDI channel ').children[0];
  const note = row.children.find((child) => child.textContent === 'Note ').children[0];
  note.value = '64';
  f.poll();
  await tick();
  assert.equal(f.controls['scene-list'].children[0], row);
  assert.equal(note.value, '64');
  channel.value = '16';
  await button(row, 'Bind MIDI').click();
  const call = f.calls.find((entry) => entry.url.startsWith('/api/scenes/bind'));
  const query = new URL(call.url, 'http://audiox').searchParams;
  assert.equal(query.get('channel'), '15');
  assert.equal(query.get('note'), '64');
  channel.value = '17';
  await button(row, 'Bind MIDI').click();
  assert.match(f.controls['scene-status'].textContent, /channel 1–16/);
});
test('capture/update/delete/recall failures are visible and destructive actions honor cancellation', async () => {
  const f = await fixture();
  const row = f.controls['scene-list'].children[0];
  f.window.confirm = () => false;
  await button(row, 'Delete').click();
  assert.equal(f.calls.length, 1);
  f.window.confirm = () => true;
  await button(row, 'Delete').click();
  assert.equal(f.calls.find((call) => call.url.startsWith('/api/scenes/delete')).options.method, 'DELETE');
  f.fail();
  await button(row, 'Recall').click();
  assert.equal(f.controls['scene-status'].textContent, 'Disk unavailable');
  assert.equal(button(row, 'Recall').disabled, false);
  assert.equal(f.notifications.at(-1)[0], 'Disk unavailable');
  assert.equal(f.confirmations.at(-1)[1].title, 'Recall setup scene');
});

test('capture, update and local recall reject routing drafts before confirmation or mutation', async () => {
  const f = await fixture();
  f.dirty(true);
  f.controls['scene-name'].value = 'Pending';
  const row = f.controls['scene-list'].children[0];
  for (const control of [f.controls['scene-capture'], button(row, 'Update capture'), button(row, 'Recall')]) {
    await control.click();
    assert.match(f.controls['scene-status'].textContent, /Save or undo pending routing edits/);
  }
  assert.equal(f.calls.length, 1);
  assert.equal(f.confirmations.length, 0);
  assert.equal(f.tasks(), 0);
  assert.equal(f.notifications.length, 3);
});

test('external recalls preserve routing drafts and list/binding changes never refresh history', async () => {
  const f = await fixture();
  f.dirty(true);
  f.data.sequence++;
  f.poll();
  await tick();
  assert.deepEqual(f.events, ['audiox-scene-conflict']);
  assert.match(f.controls['scene-status'].textContent, /draft was preserved/);
  assert.equal(f.notifications.at(-1)[1], 'warn');
  f.dirty(false);
  const row = f.controls['scene-list'].children[0];
  await button(row, 'Bind MIDI').click();
  f.data.scenes[0].name = 'Renamed';
  f.poll();
  await tick();
  assert.deepEqual(f.events, ['audiox-scene-conflict']);
  f.data.sequence++;
  f.poll();
  await tick();
  assert.deepEqual(f.events, ['audiox-scene-conflict', 'audiox-scene-recalled']);
});

test('device actions block scene mutations and scene polling without starting another task', async () => {
  const f = await fixture();
  f.deviceBusy(true);
  f.poll();
  await tick();
  await button(f.controls['scene-list'].children[0], 'Recall').click();
  assert.equal(f.calls.length, 1);
  assert.equal(f.tasks(), 0);
  assert.match(f.notifications.at(-1)[0], /Another device action/);
});
