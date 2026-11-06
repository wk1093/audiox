const assert = require('node:assert/strict');
const { test } = require('node:test');
const updates = require('../../web/embedded/updates.js');

const bytes = Buffer.from('AXUPDATEtestdata!');
const asset = { id: 123, name: 'audiox-update-pi4.axu', size: bytes.length, state: 'uploaded' };
const release = { tag_name: 'v1.9.2', draft: false, prerelease: false, assets: [asset] };
const response = (data, status = 200) => new Response(JSON.stringify(data), { status });
const mirrorUrl = 'https://wklab.net/audiox/updates/v1.9.2/audiox-update-pi4.axu';

test('board detection uses hardware field and supports older Pi Model B responses', () => {
  assert.equal(updates.deviceBoard({ board: 'pi4' }), 'pi4');
  assert.equal(updates.deviceBoard({ board: 'pi5' }), 'pi5');
  assert.equal(updates.deviceBoard({ model: 'Raspberry Pi 4 Model B Rev 1.5' }), 'pi4');
  assert.equal(updates.deviceBoard({ model: 'Raspberry Pi 5 Model B Rev 1.0' }), 'pi5');
  assert.throws(() => updates.deviceBoard({ model: 'Raspberry Pi 3 Model B' }), /unknown Pi/);
  assert.throws(() => updates.deviceBoard({ model: 'Raspberry Pi 500' }), /unknown Pi/);
});

test('archive selection never substitutes another board and rejects unavailable/oversized assets', () => {
  assert.equal(updates.selectAsset(release, 'pi4'), asset);
  assert.throws(() => updates.selectAsset(release, 'pi5'), /no audiox-update-pi5/);
  assert.throws(() => updates.selectAsset({ ...release, draft: true }, 'pi4'), /unpublished/);
  assert.throws(() => updates.selectAsset({ ...release, assets: [{ ...asset, state: 'new' }] }, 'pi4'), /no audiox/);
  assert.throws(() => updates.selectAsset({ ...release, assets: [{ ...asset, size: 257 * 1048576 }] }, 'pi4'), /invalid size/);
});

test('stable check uses latest stable endpoint and reports GitHub API errors', async () => {
  assert.equal((await updates.latestRelease('stable', async (url, options) => {
    assert.equal(url, 'https://api.github.com/repos/wk1093/audiox/releases/latest');
    assert.equal(options.credentials, 'omit');
    return response(release);
  })).tag_name, 'v1.9.2');
  await assert.rejects(updates.latestRelease('stable', async () => response({}, 403)), /rate limit/);
  await assert.rejects(updates.latestRelease('stable', async () => response({}, 404)), /no published release/);
  await assert.rejects(updates.latestRelease('stable', async () => response({ ...release, prerelease: true })), /not an AudioX stable/);
});

test('nightly lookup resolves the rolling tag to version-specific release metadata', async () => {
  const paths = [];
  const result = await updates.latestRelease('nightly', async (url) => {
    paths.push(url);
    if (url.endsWith('/releases/latest')) return response(release);
    return response(url.endsWith('/releases/tags/nightly')
      ? { body: '<!-- audiox-release-tag: v1.10.0-beta.3.nightly+42 -->' }
      : { ...release, tag_name: 'v1.10.0-beta.3.nightly+42', prerelease: true });
  });
  assert.equal(result.tag_name, 'v1.10.0-beta.3.nightly+42');
  assert.ok(paths[2].endsWith('/releases/tags/v1.10.0-beta.3.nightly%2B42'));
  await assert.rejects(updates.latestRelease('nightly', async (url) =>
    response(url.endsWith('/releases/latest') ? release : {})), /No versioned nightly/);
});

test('nightly lookup uses stable unless a higher SemVer nightly is published', async () => {
  for (const tag of ['v1.8.9-beta.1.nightly+42', 'v1.9.2-beta.1.nightly+42', 'v1.9.3-beta.1.nightly+42']) {
    const result = await updates.latestRelease('nightly', async (url) => {
      if (url.endsWith('/releases/latest')) return response(release);
      if (url.endsWith('/releases/tags/nightly')) return response({ body: `<!-- audiox-release-tag: ${tag} -->` });
      return response({ ...release, tag_name: tag, prerelease: true });
    });
    assert.equal(result.tag_name, tag.startsWith('v1.9.3') ? tag : release.tag_name);
  }
});

test('nightly lookup handles absent channels without hiding API failures', async () => {
  assert.equal((await updates.latestRelease('nightly', async (url) =>
    url.endsWith('/releases/latest') ? response(release) : response({}, 404))).tag_name, release.tag_name);
  const nightly = { ...release, tag_name: 'v1.10.0-nightly+42', prerelease: true };
  assert.equal((await updates.latestRelease('nightly', async (url) => {
    if (url.endsWith('/releases/latest')) return response({}, 404);
    return response(url.endsWith('/releases/tags/nightly')
      ? { body: '<!-- audiox-release-tag: v1.10.0-nightly+42 -->' } : nightly);
  })).tag_name, nightly.tag_name);
  await assert.rejects(updates.latestRelease('nightly', async () => response({}, 404)), /No published stable or nightly/);
  for (const failingPath of ['/releases/latest', '/releases/tags/nightly']) {
    await assert.rejects(updates.latestRelease('nightly', async (url) =>
      url.endsWith(failingPath) ? response({}, 403) : response(release)), /rate limit/);
  }
});

test('release version is read from the published release notes when present', () => {
  assert.equal(updates.releaseVersion({ body: 'Version: `1.10.0-beta.3.nightly+0123456789ab`\n\nBuild commit: `x`' }),
    '1.10.0-beta.3.nightly+0123456789ab');
  assert.equal(updates.releaseVersion({ body: 'Version: `1.10.1-nightly+0123456789ab`' }), '1.10.1-nightly+0123456789ab');
  assert.equal(updates.releaseVersion({ body: 'Build commit: `x`' }), '');
  assert.equal(updates.releaseVersion({}), '');
});

test('mirror lookup requires the exact GitHub version, board, size and path', async () => {
  const data = { stable: { tag: 'v1.9.2', assets: {
    pi4: { size: asset.size, url: 'updates/v1.9.2/audiox-update-pi4.axu' },
  } } };
  assert.equal(await updates.mirrorArchive('stable', release, 'pi4', asset, async () => response(data)), mirrorUrl);
  assert.equal(await updates.mirrorArchive('nightly', release, 'pi4', asset, async () => response(data)), mirrorUrl);
  await assert.rejects(updates.mirrorArchive('stable', { ...release, tag_name: 'v1.9.3' }, 'pi4', asset, async () => response(data)), /not deployed/);
  await assert.rejects(updates.mirrorArchive('stable', release, 'pi5', asset, async () => response(data)), /not deployed/);
  await assert.rejects(updates.mirrorArchive('stable', release, 'pi4', asset, async () => response({}, 404)), /mirror unavailable/);
});

test('browser downloads mirror bytes, checks length and magic, and reports progress', async () => {
  const progress = [];
  const blob = await updates.downloadArchive(asset, mirrorUrl, (p) => progress.push(p), async (url, options) => {
    assert.equal(url, mirrorUrl);
    assert.equal(options.credentials, 'omit');
    return new Response(bytes);
  });
  assert.deepEqual(Buffer.from(await blob.arrayBuffer()), bytes);
  assert.equal(progress.at(-1), 100);
  await assert.rejects(updates.downloadArchive(asset, mirrorUrl, () => {}, async () => new Response(bytes.subarray(0, 10))), /size does not match/);
  await assert.rejects(updates.downloadArchive(asset, mirrorUrl, () => {}, async () => new Response(Buffer.alloc(asset.size + 1))), /exceeds/);
  await assert.rejects(updates.downloadArchive(asset, mirrorUrl, () => {}, async () => new Response(Buffer.alloc(asset.size))), /invalid AXU/);
  await assert.rejects(updates.downloadArchive(asset, mirrorUrl, () => {}, async () => new Response('', { status: 404 })), /download failed/);
});

function controller({ storage = memoryStorage(), device = { channel: 'stable' }, request = global.fetch } = {}) {
  global.fetch = async (url, options = {}) => {
    if (url !== '/api/updates/channel') return request(url, options);
    if (device.error) return new Response('Device configuration unavailable', { status: device.error });
    if (options.method === 'POST') device.channel = new URLSearchParams(options.body).get('channel');
    return response({ channel: device.channel });
  };
  const elements = new Map();
  for (const id of ['update-channel', 'btn-update-check', 'btn-update-online', 'btn-update-upload',
    'update_file', 'update-progress', 'update-status', 'update-release', 'update-download',
    'btn-config-backup', 'btn-config-restore', 'backup_file']) {
    elements.set(id, {
      value: id === 'update-channel' ? 'stable' : 0, files: [], disabled: false, listeners: {},
      addEventListener(event, callback) { this.listeners[event] = callback; },
      removeAttribute(name) { delete this[name]; },
    });
  }
  global.document = { getElementById: (id) => elements.get(id) };
  const popup = {
    ...require('../../web/embedded/ui.js'), confirm: async () => true,
    runTask: async (_title, action) => action(),
  };
  const uploads = [];
  global.XMLHttpRequest = class {
    constructor() {
      this.events = {};
      this.upload = { addEventListener: () => {} };
    }
    open(method, path) { assert.equal(method, 'PUT'); assert.equal(path, '/api/update'); }
    setRequestHeader() {}
    addEventListener(event, callback) { this.events[event] = callback; }
    send(blob) {
      uploads.push(blob);
      this.status = 202;
      this.responseText = 'Update installed; rebooting.';
      this.events.load();
    }
  };
  const ready = updates.init({ setStatus: () => {}, ui: popup, storage });
  return { elements, uploads, popup, ready, click: (id) => elements.get(id).listeners.click() };
}

test('device channel persists across browsers and overrides old browser preferences', async () => {
  for (const expected of ['stable', 'nightly']) {
    const device = { channel: expected };
    const storage = memoryStorage(expected === 'stable' ? 'nightly' : 'stable');
    const first = controller({ storage, device });
    await first.ready;
    assert.equal(first.elements.get('update-channel').value, expected);

    const reloaded = controller({ device, storage: null });
    await reloaded.ready;
    assert.equal(reloaded.elements.get('update-channel').value, expected);

    const changed = expected === 'nightly' ? 'stable' : 'nightly';
    reloaded.elements.get('update-channel').value = changed;
    await reloaded.elements.get('update-channel').listeners.change();
    assert.equal(device.channel, changed);
    const next = controller({ device, storage: memoryStorage() });
    await next.ready;
    assert.equal(next.elements.get('update-channel').value, changed);
  }
});

test('checking immediately waits for the device channel and stable fallback does not replace it', async () => {
  const urls = [];
  const request = async (url) => {
    urls.push(url);
    if (url === '/api/system/info') return response({ board: 'pi4', version: '1.15.0-nightly+0123456789ab' });
    if (url.endsWith('/releases/latest')) return response(release);
    if (url.endsWith('/releases/tags/nightly')) return response({}, 404);
    assert.ok(url.endsWith('/updates.json'));
    return response({ stable: { tag: release.tag_name, assets: {
      pi4: { size: asset.size, url: 'updates/v1.9.2/audiox-update-pi4.axu' },
    } } });
  };
  const device = { channel: 'nightly' };
  const { elements, click } = controller({ device, request });
  const checked = click('btn-update-check');
  assert.deepEqual(urls, []);
  await checked;
  assert.ok(urls.some((url) => url.endsWith('/releases/tags/nightly')));
  assert.match(elements.get('update-release').textContent, /Using stable/);
  assert.equal(elements.get('update-channel').value, 'nightly');
  assert.equal(device.channel, 'nightly');
});

test('channel initialization and persistence failures are reported', async () => {
  const device = { channel: 'nightly', error: 503 };
  const failed = controller({ device });
  await failed.ready;
  assert.match(failed.elements.get('update-status').textContent, /Could not load.*503/);
  await failed.click('btn-update-check');
  assert.match(failed.elements.get('update-status').textContent, /Select a release channel/);

  delete device.error;
  const unsaved = controller({ device });
  await unsaved.ready;
  assert.equal(unsaved.elements.get('update-channel').value, 'nightly');
  device.error = 500;
  unsaved.elements.get('update-channel').value = 'stable';
  await unsaved.elements.get('update-channel').listeners.change();
  assert.match(unsaved.elements.get('update-status').textContent, /Could not save.*500/);
  assert.equal(unsaved.elements.get('update-channel').value, 'nightly');
  assert.equal(device.channel, 'nightly');
  delete device.error;
  unsaved.elements.get('update-channel').value = 'stable';
  await unsaved.elements.get('update-channel').listeners.change();
  assert.equal(device.channel, 'stable');
});

test('controller checks, confirms, downloads and uploads without Pi internet; cancellation sends nothing', async () => {
  const { elements, uploads, popup, click } = controller();
  const urls = [];
  global.fetch = async (url) => {
    urls.push(url);
    if (url === '/api/system/info') return response({ board: 'pi4', version: '1.9.1' });
    if (url.endsWith('/releases/latest')) return response(release);
    if (url.endsWith('/updates.json')) return response({ stable: { tag: 'v1.9.2', assets: {
      pi4: { size: asset.size, url: 'updates/v1.9.2/audiox-update-pi4.axu' },
    } } });
    assert.equal(url, mirrorUrl);
    return new Response(bytes);
  };
  await click('btn-update-check');
  assert.equal(elements.get('btn-update-online').disabled, false);
  popup.confirm = async () => false;
  await click('btn-update-online');
  assert.equal(uploads.length, 0);
  popup.confirm = async () => true;
  await click('btn-update-online');
  assert.equal(uploads.length, 1);
  assert.deepEqual(Buffer.from(await uploads[0].arrayBuffer()), bytes);
  assert.equal(elements.get('btn-update-upload').disabled, true);
  assert.ok(urls.includes(mirrorUrl));
});

test('nightly controller downloads and installs the stable archive when nightly is older', async () => {
  const { elements, uploads, click } = controller({ device: { channel: 'nightly' } });
  elements.get('update-channel').value = 'nightly';
  const urls = [];
  global.fetch = async (url) => {
    urls.push(url);
    if (url === '/api/system/info') return response({ board: 'pi4', version: '1.9.1' });
    if (url.endsWith('/releases/latest')) return response(release);
    if (url.endsWith('/releases/tags/nightly')) {
      return response({ body: '<!-- audiox-release-tag: v1.8.0-nightly+42 -->' });
    }
    if (url.endsWith('/releases/tags/v1.8.0-nightly%2B42')) {
      return response({ ...release, tag_name: 'v1.8.0-nightly+42', prerelease: true });
    }
    if (url.endsWith('/updates.json')) return response({ stable: { tag: release.tag_name, assets: {
      pi4: { size: asset.size, url: 'updates/v1.9.2/audiox-update-pi4.axu' },
    } } });
    assert.equal(url, mirrorUrl);
    return new Response(bytes);
  };
  await click('btn-update-check');
  assert.equal(elements.get('btn-update-online').disabled, false);
  assert.match(elements.get('update-release').textContent, /v1.9.2/);
  assert.match(elements.get('update-release').textContent, /Using stable/);
  await click('btn-update-online');
  assert.equal(uploads.length, 1);
  assert.ok(urls.includes(mirrorUrl));
});

test('nightly fallback recognizes an already installed stable release', async () => {
  const { elements, uploads, click } = controller({ device: { channel: 'nightly' } });
  elements.get('update-channel').value = 'nightly';
  global.fetch = async (url) => {
    if (url === '/api/system/info') return response({ board: 'pi4', version: '1.9.2' });
    if (url.endsWith('/releases/latest')) return response(release);
    assert.ok(url.endsWith('/releases/tags/nightly'));
    return response({}, 404);
  };
  await click('btn-update-check');
  assert.match(elements.get('update-release').textContent, /Already installed/);
  assert.equal(elements.get('btn-update-online').disabled, true);
  assert.equal(uploads.length, 0);
});

test('controller rejects changed hardware, resets channel selection and retains manual upload', async () => {
  const { elements, uploads, click } = controller();
  let board = 'pi4';
  global.fetch = async (url) => {
    if (url === '/api/updates/channel') return response({ channel: 'stable' });
    if (url === '/api/system/info') return response({ board, version: '1.9.1' });
    if (url.endsWith('/releases/latest')) return response(release);
    return response({ stable: { tag: 'v1.9.2', assets: {
      pi4: { size: asset.size, url: 'updates/v1.9.2/audiox-update-pi4.axu' },
    } } });
  };
  await click('btn-update-check');
  board = 'pi5';
  await click('btn-update-online');
  assert.equal(uploads.length, 0);
  assert.match(elements.get('update-status').textContent, /hardware changed/);
  assert.equal(elements.get('btn-update-upload').disabled, false);
  await elements.get('update-channel').listeners.change();
  assert.equal(elements.get('btn-update-online').disabled, true);
  elements.get('update_file').files = [Object.assign(new Blob([bytes]), { name: 'manual.axu' })];
  await click('btn-update-upload');
  assert.equal(uploads.length, 1);
});

function memoryStorage(channel) {
  const values = new Map();
  if (channel) values.set('audiox.updateChannel', channel);
  return {
    values,
    getItem: (key) => (values.has(key) ? values.get(key) : null),
    setItem: (key, value) => values.set(key, String(value)),
  };
}

const dated = { ...release, published_at: '2026-05-03T10:00:00Z' };
const githubRequest = (calls, history = [dated, { ...release, tag_name: 'v1.9.1', published_at: '2026-04-01T00:00:00Z' }]) =>
  async (url) => {
    calls.push(url);
    if (url.endsWith('/releases/latest')) return response(dated);
    if (url.endsWith('/releases?per_page=100')) return response(history);
    return response({}, 404);
  };

test('update check compares the selected channel with GitHub release history', async () => {
  const calls = [];
  assert.deepEqual(await updates.checkForUpdate('stable', '1.9.1', githubRequest(calls)),
    { available: true, tag: 'v1.9.2', version: '1.9.2', prerelease: false });
  assert.equal((await updates.checkForUpdate('stable', '1.9.2', githubRequest(calls))).available, false);
  assert.equal((await updates.checkForUpdate('stable', '0.1.0-local', githubRequest(calls))).available, true);
  await assert.rejects(updates.checkForUpdate('stable', '1.9.1', githubRequest(calls, {})), /invalid release list/);
});

test('update checks are cached per channel and device version, with shorter retry after errors', async () => {
  const storage = memoryStorage();
  const calls = [];
  const now = 1_000_000_000;
  const check = (extra) => updates.cachedUpdateCheck({
    channel: 'stable', version: '1.9.1', request: githubRequest(calls), storage, now, ...extra,
  });
  assert.equal((await check()).available, true);
  const fetched = calls.length;
  await check({ now: now + 5 * 3600 * 1000 });
  assert.equal(calls.length, fetched, 'cached for 6 hours');
  await check({ now: now + 6 * 3600 * 1000 });
  assert.equal(calls.length, fetched * 2);
  await check({ now: now + 6 * 3600 * 1000, version: '1.9.2' });
  assert.equal(calls.length, fetched * 3, 'device version change invalidates cache');

  const failing = memoryStorage();
  let attempts = 0;
  const fail = (at) => updates.cachedUpdateCheck({
    channel: 'stable', version: '1.9.1', storage: failing, now: at,
    request: async () => { attempts += 1; return response({}, 403); },
  });
  await assert.rejects(fail(now), /rate limit/);
  await assert.rejects(fail(now + 30 * 60 * 1000), /rate limit/);
  assert.equal(attempts, 2, 'releases and latest fetched once');
  await assert.rejects(fail(now + 60 * 60 * 1000), /rate limit/);
  assert.equal(attempts, 4, 'retried after an hour');

  const broken = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); } };
  assert.equal((await updates.cachedUpdateCheck({
    channel: 'stable', version: '1.9.1', request: githubRequest([]), storage: broken, now,
  })).available, true);
});

test('dismissing hides only the dismissed release', () => {
  const storage = memoryStorage();
  const result = { available: true, tag: 'v1.9.2', version: '1.9.2' };
  assert.equal(updates.noticeVisible(result, storage), true);
  updates.dismissNotice(result, storage);
  assert.equal(updates.noticeVisible(result, storage), false);
  assert.equal(updates.noticeVisible({ ...result, tag: 'v1.9.3' }, storage), true);
  assert.equal(updates.noticeVisible({ ...result, available: false }, memoryStorage()), false);
});

test('controller loads the device channel before checking notices and persists the opt-out', async () => {
  const elements = new Map();
  const make = (id, extra = {}) => elements.set(id, {
    id, value: 0, files: [], disabled: false, hidden: true, checked: false, listeners: {}, textContent: '',
    addEventListener(event, callback) { this.listeners[event] = callback; },
    removeAttribute(name) { delete this[name]; },
    click() { return this.listeners.click?.(); },
    ...extra,
  });
  for (const id of ['btn-update-check', 'btn-update-online', 'btn-update-upload', 'update_file',
    'update-progress', 'update-status', 'update-release', 'update-download', 'update-banner',
    'update-banner-text', 'btn-update-banner-view', 'btn-update-banner-dismiss', 'update-notifications']) make(id);
  make('update-channel', { value: 'stable' });
  global.document = { getElementById: (id) => elements.get(id) };
  const storage = memoryStorage();
  let enabled = true;
  const posts = [];
  const lookups = [];
  let deviceChannel = 'nightly';
  global.fetch = async (url, options = {}) => {
    if (url === '/api/updates/channel') {
      if (options.method === 'POST') deviceChannel = new URLSearchParams(options.body).get('channel');
      return response({ channel: deviceChannel });
    }
    if (url === '/api/system/info') return response({ board: 'pi4', version: '1.9.1-nightly+0123456789ab' });
    if (url === '/api/updates/notifications') {
      if (options.method === 'POST') {
        posts.push(options.body);
        enabled = options.body === 'enabled=1';
      }
      return response({ enabled });
    }
    return githubRequest(lookups)(url);
  };
  let shown = 0;
  const popup = { ...require('../../web/embedded/ui.js'), runTask: async (_title, action) => action() };
  const flush = () => new Promise((resolve) => setTimeout(resolve, 10));
  await updates.init({ setStatus: () => {}, ui: popup, storage, showUpdates: () => { shown += 1; } });
  assert.equal(elements.get('update-channel').value, 'nightly');
  assert.equal(deviceChannel, 'nightly');
  await flush();
  assert.ok(lookups.some((url) => url.endsWith('/releases/tags/nightly')));
  const banner = elements.get('update-banner');
  assert.equal(banner.hidden, false);
  assert.match(elements.get('update-banner-text').textContent, /1\.9\.2 is available/);

  await elements.get('btn-update-banner-view').click();
  assert.equal(shown, 1);
  await flush();

  elements.get('btn-update-banner-dismiss').click();
  assert.equal(banner.hidden, true);
  assert.equal(storage.getItem('audiox.updateDismissed'), 'v1.9.2');

  storage.values.delete('audiox.updateDismissed');
  const box = elements.get('update-notifications');
  box.checked = false;
  await box.listeners.change();
  assert.deepEqual(posts, ['enabled=0']);
  assert.equal(banner.hidden, true);
  box.checked = true;
  await box.listeners.change();
  assert.equal(banner.hidden, false);

  elements.get('update-channel').value = 'stable';
  await elements.get('update-channel').listeners.change();
  assert.equal(deviceChannel, 'stable');
  await flush();
});
