const assert = require('node:assert/strict');
const { test } = require('node:test');
const updates = require('../web/updates.js');

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

test('nightly lookup resolves the rolling index to version-specific release metadata', async () => {
  const paths = [];
  const result = await updates.latestRelease('nightly', async (url) => {
    paths.push(url);
    return response(paths.length === 1
      ? { body: '<!-- audiox-run-number: 42 -->' }
      : { ...release, tag_name: 'nightly-42', prerelease: true });
  });
  assert.equal(result.tag_name, 'nightly-42');
  assert.ok(paths[1].endsWith('/releases/tags/nightly-42'));
  await assert.rejects(updates.latestRelease('nightly', async () => response({})), /No versioned nightly/);
});

test('mirror lookup requires the exact GitHub version, board, size and path', async () => {
  const data = { stable: { tag: 'v1.9.2', assets: {
    pi4: { size: asset.size, url: 'updates/v1.9.2/audiox-update-pi4.axu' },
  } } };
  assert.equal(await updates.mirrorArchive('stable', release, 'pi4', asset, async () => response(data)), mirrorUrl);
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

function controller() {
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
  global.window = { confirm: () => true };
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
  updates.init({ setStatus: () => {} });
  return { elements, uploads, click: (id) => elements.get(id).listeners.click() };
}

test('controller checks, confirms, downloads and uploads without Pi internet; cancellation sends nothing', async () => {
  const { elements, uploads, click } = controller();
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
  global.window.confirm = () => false;
  await click('btn-update-online');
  assert.equal(uploads.length, 0);
  global.window.confirm = () => true;
  await click('btn-update-online');
  assert.equal(uploads.length, 1);
  assert.deepEqual(Buffer.from(await uploads[0].arrayBuffer()), bytes);
  assert.equal(elements.get('btn-update-upload').disabled, true);
  assert.ok(urls.includes(mirrorUrl));
});

test('controller rejects changed hardware, resets channel selection and retains manual upload', async () => {
  const { elements, uploads, click } = controller();
  let board = 'pi4';
  global.fetch = async (url) => {
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
  elements.get('update-channel').listeners.change();
  assert.equal(elements.get('btn-update-online').disabled, true);
  elements.get('update_file').files = [Object.assign(new Blob([bytes]), { name: 'manual.axu' })];
  await click('btn-update-upload');
  assert.equal(uploads.length, 1);
});
