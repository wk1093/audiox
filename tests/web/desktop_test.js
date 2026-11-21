const assert = require('node:assert/strict');
const { test } = require('node:test');
const { create } = require('../../web/embedded/desktop.js');

test('desktop page follows connection and displays detected capabilities as text', () => {
  const tab = { hidden: true };
  const host = { textContent: '' };
  const os = { textContent: '' };
  const capabilities = { textContent: '' };
  const media = { textContent: '' };
  let event, connection, disconnected = 0, unsubscribed = 0;
  const api = {
    subscribe(topic, listener) {
      assert.equal(topic, 'desktop.status');
      event = listener;
      return () => { unsubscribed++; };
    },
    onConnectionChange(listener) {
      connection = listener;
      return () => { unsubscribed++; };
    },
  };
  const dispose = create({ api, tab, host, os, capabilities, media, onDisconnect: () => { disconnected++; } });
  event({ connected: true, hello: { hostname: '<host>', os: 'linux', capabilities: ['pipewire', 'default_sink'], media: 'playing' } });
  assert.equal(tab.hidden, false);
  assert.equal(host.textContent, '<host>');
  assert.equal(os.textContent, 'linux');
  assert.equal(capabilities.textContent, 'pipewire, default_sink');
  assert.equal(media.textContent, 'Playing');
  event({ connected: true, hello: { hostname: 'other', os: 'windows', capabilities: [] } });
  assert.equal(capabilities.textContent, 'None detected');
  assert.equal(media.textContent, 'Unavailable');
  connection(false);
  assert.equal(tab.hidden, true);
  assert.equal(host.textContent, '');
  assert.equal(media.textContent, '');
  assert.equal(disconnected, 1);
  event({ connected: true, hello: { hostname: 'bad', os: 'linux', capabilities: [null] } });
  assert.equal(tab.hidden, true);
  dispose();
  assert.equal(unsubscribed, 2);
});

test('desktop routing reports acknowledged outcomes and rejects stale sessions', async () => {
  const tab = {}, host = {}, os = {}, capabilities = {}, media = {};
  const routing = {}, stream = { value: '11' }, sink = { value: '2' };
  const routeSubmit = {}, routeStatus = {};
  let submit, event;
  const routeForm = {
    addEventListener(name, listener) { assert.equal(name, 'submit'); submit = listener; },
    removeEventListener() {},
  };
  const requests = [];
  let result = 'succeeded';
  const api = {
    subscribe(topic, listener) { event = listener; },
    async request(url, options) {
      requests.push({ url, options });
      return { ok: true, async json() { return url.includes('?') ? { id: 8, status: result } : { id: 8 }; } };
    },
  };
  create({ api, tab, host, os, capabilities, media, routing, routeForm,
    stream, sink, routeSubmit, routeStatus, onDisconnect() {} });
  event({ connected: true, session: 17, hello: { hostname: 'pc', os: 'linux', capabilities: ['route_stream'] } });
  assert.equal(routing.hidden, false);
  const run = () => submit({ preventDefault() {} });
  await run();
  assert.equal(requests[0].options.body, 'stream=11&sink=2&session=17');
  assert.equal(routeStatus.textContent, 'Stream moved successfully.');
  assert.equal(routeSubmit.disabled, false);
  result = 'failed';
  await run();
  assert.match(routeStatus.textContent, /could not move/);
  stream.value = '-1';
  await run();
  assert.match(routeStatus.textContent, /valid stream/);
  assert.equal(requests.length, 4);
  event({ connected: true, session: 18, hello: { hostname: 'pc', os: 'windows', capabilities: [] } });
  assert.equal(routing.hidden, true);
  assert.equal(routeStatus.textContent, '');
});

test('a late route response cannot update a replacement desktop', async () => {
  let submit, event, resolveRequest;
  const pending = new Promise((resolve) => { resolveRequest = resolve; });
  const routeStatus = {}, routing = {}, routeSubmit = {};
  const stream = { value: '1' }, sink = { value: '2' };
  const api = {
    subscribe(topic, listener) { event = listener; },
    request() { return pending; },
  };
  create({ api, tab: {}, host: {}, os: {}, capabilities: {}, media: {}, routing,
    stream, sink, routeSubmit, routeStatus,
    routeForm: { addEventListener(name, listener) { submit = listener; } },
    onDisconnect() {} });
  const hello = { hostname: 'pc', os: 'linux', capabilities: ['route_stream'] };
  event({ connected: true, session: 1, hello });
  const action = submit({ preventDefault() {} });
  assert.equal(routeSubmit.disabled, true);
  event({ connected: true, session: 2, hello });
  resolveRequest({ ok: true, async json() { return { id: 1 }; } });
  await action;
  assert.equal(routeStatus.textContent, '');
  assert.equal(routeSubmit.disabled, false);
  assert.equal(stream.value, '');
  assert.equal(sink.value, '');
});
