'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { demoHtml, demoCss } = require('../../web/embedded/build.js');

const embedded = path.resolve(__dirname, '../../web/embedded');
const read = (name) => fs.readFileSync(path.join(embedded, name), 'utf8');

function demoContext() {
  const timers = [];
  const context = vm.createContext({
    console, URL, URLSearchParams, Response, Blob, TextEncoder, TextDecoder, DOMException, ReadableStream,
    ArrayBuffer, Uint8Array, performance,
    location: { href: 'https://example.test/audiox/demo/' },
    fetch: async () => { throw new Error('demo must not reach the network for /api/'); },
    // Simulation loops are not needed; request latency resolves immediately.
    setTimeout: (fn, ms) => { if (ms < 1000) timers.push(setImmediate(fn)); return 0; },
    setInterval: () => 0,
    clearInterval: () => {},
  });
  context.globalThis = context;
  vm.runInContext(read('demo/mock-data.js'), context);
  vm.runInContext(read('demo/mock-api.js'), context);
  return context;
}

test('demo requires activity-hook consent and reports the new event mask', async () => {
  const context = demoContext();
  const body = new URLSearchParams({
    name_hex: Buffer.from('Ducking').toString('hex'),
    script_hex: Buffer.from("hook('device_active','mic',function(e) end)").toString('hex'),
  }).toString();
  const save = (payload) => context.apiFetch('/api/scripts', { method: 'POST', body: payload });
  assert.equal((await save(body)).status, 409);
  assert.equal((await save(body + '&activity_ack=1')).status, 200);
  const { scripts } = await (await context.apiFetch('/api/scripts')).json();
  assert.equal(scripts.find(script => script.name === 'Ducking').event_mask, 16384);
});

test('demo build swaps api.js for the simulated device and relativizes asset URLs', () => {
  const html = demoHtml(read('index.html'));
  assert.doesNotMatch(html, /src="\/api\.js"/);
  assert.ok(html.indexOf('./demo/mock-data.js') < html.indexOf('./demo/mock-api.js'));
  assert.ok(html.indexOf('./demo/mock-api.js') < html.indexOf('./app.js'));
  assert.doesNotMatch(html, /\s(?:src|href)="\/(?!\/)/);
  assert.match(html, /<title>AudioX Live Demo<\/title>/);
  assert.equal(demoCss("a{background:url('/logo.svg')} b{src:url(//cdn/x)}"),
    "a{background:url('./logo.svg')} b{src:url(//cdn/x)}");
});

test('demo gateway answers every API path the UI requests', async () => {
  const context = demoContext();
  assert.equal(context.AudioxApi.demo, true);
  const paths = new Set();
  for (const name of fs.readdirSync(embedded).filter((file) => file.endsWith('.js'))) {
    for (const [, literal] of read(name).matchAll(/'(\/api\/[^'?]+)'/g)) paths.add(literal);
  }
  // Templated paths built at runtime by the UI.
  ['/api/scenes/save', '/api/system/history', '/api/system/logs', '/api/config/restore/check',
    '/api/soundboard/trigger/airhorn.wav', '/api/soundboard/press/airhorn.wav', '/api/rootfs/sfx/airhorn.wav']
    .forEach((p) => paths.add(p));
  // Upload/download paths are handled by AudioxApi.upload/download rather than request().
  for (const p of ['/api/update', '/api/config/restore', '/api/config/backup']) paths.delete(p);
  assert.ok(paths.size > 60);
  for (const p of paths) {
    const text = await (await context.apiFetch(p)).text();
    assert.ok(!text.startsWith('not found:'), `${p} is not simulated`);
  }
});

test('demo device state is consistent across routing, effects and meters', async () => {
  const context = demoContext();
  const json = async (url, init) => (await context.apiFetch(url, init)).json();
  const { things } = await json('/api/routing/things');
  const ids = new Set(things.map((thing) => thing.id));
  const reload = await json('/api/routing/reload', { method: 'POST' });
  assert.equal(reload.skippedEdges, 0, 'every demo route should resolve');
  assert.ok(reload.edges >= 20 && reload.edges <= 40);

  const { effects } = await json('/api/effects');
  assert.ok(effects.length >= 5 && effects.length <= 10);
  for (const fx of effects) {
    assert.ok(ids.has(fx.id));
    assert.deepEqual(Object.keys(fx.params), fx.param_meta.map((meta) => meta.name));
  }

  const created = await json('/api/effect/create', { method: 'POST', body: 'type=reverb' });
  assert.equal(created.ok, true);
  assert.ok((await json('/api/routing/things')).things.some((thing) => thing.id === created.id));

  const { meters } = await json('/api/audio/meters');
  assert.equal(meters.length, things.length + 1);
  assert.ok(meters.every((meter) => meter.levels.every((level) => level >= 0 && level <= 1)));
  assert.ok(meters.find((meter) => meter.id === 'usb_gadget_out').levels.some((level) => level > 0));

  const info = await json('/api/system/info');
  const { storage } = info;
  assert.equal(storage.used_bytes + storage.free_bytes + storage.reserved_bytes, storage.total_bytes);
  assert.equal(typeof info.version, 'string');
  assert.ok(Number.isFinite(info.uptime_secs));

  const { scripts } = await json('/api/scripts');
  assert.ok(scripts.every((script) => /^[0-9a-f]+$/.test(script.script_hex)));
});

test('demo scenes capture and recall the setup within the page', async () => {
  const context = demoContext();
  const json = async (url, init) => (await context.apiFetch(url, init)).json();
  const routing = async () => (await context.apiFetch('/api/rootfs/config/routing.txt')).text();
  const hex = (text) => Buffer.from(text).toString('hex');

  const initial = await json('/api/scenes');
  assert.ok(initial.scenes.length >= 3);
  for (const scene of initial.scenes) {
    assert.equal(scene.snapshot, undefined);
    assert.ok(Number.isInteger(scene.routes) && Number.isInteger(scene.effects) && Number.isInteger(scene.volumes));
  }

  const liveRouting = await routing();
  const liveEffects = (await json('/api/effects')).effects.map((fx) => fx.id);
  const clean = initial.scenes.find((scene) => scene.effects === 0);
  assert.deepEqual(await json(`/api/scenes/recall?id=${clean.id}`, { method: 'POST' }), { ok: true });
  const recalled = await json('/api/scenes');
  assert.equal(recalled.active, clean.id);
  assert.equal(recalled.sequence, initial.sequence + 1);
  assert.equal((await json('/api/effects')).effects.length, 0);
  assert.notEqual(await routing(), liveRouting);
  assert.equal((await json('/api/routing/reload', { method: 'POST' })).skippedEdges, 0);

  // Capture a new scene from the clean setup, change things, then recall it.
  await json(`/api/scenes/save?id=mine&name_hex=${hex('Mine')}&channel=-1&note=-1`, { method: 'POST' });
  const cleanRouting = await routing();
  await json('/api/effect/create', { method: 'POST', body: 'type=reverb' });
  await json(`/api/scenes/recall?id=${initial.active}`, { method: 'POST' });
  assert.equal(await routing(), liveRouting);
  assert.deepEqual((await json('/api/effects')).effects.map((fx) => fx.id), liveEffects);
  await json('/api/scenes/recall?id=mine', { method: 'POST' });
  assert.equal(await routing(), cleanRouting);
  assert.equal((await json('/api/effects')).effects.length, 0);
  assert.equal((await context.apiFetch('/api/scenes/recall?id=nope', { method: 'POST' })).status, 404);
});
