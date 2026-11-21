const assert = require('node:assert/strict');
const { test } = require('node:test');
const { create } = require('../../web/embedded/desktop.js');

function element() {
  return {
    value: '', textContent: '', children: [], listeners: {},
    addEventListener(name, fn) { this.listeners[name] = fn; },
    removeEventListener(name) { delete this.listeners[name]; },
    replaceChildren(...children) { this.children = children; this.value = children[0]?.value || ''; },
    append(...children) { this.children.push(...children); },
  };
}

function documentForTest(t) {
  const previous = globalThis.document;
  globalThis.document = { createElement: element };
  t.after(() => { globalThis.document = previous; });
}

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

test('default sink shows discovered devices, sends stable names, and reports failures', async (t) => {
  documentForTest(t);
  let event, failed = false;
  const defaultSection = {}, defaultForm = element(), defaultSink = element(), defaultSubmit = {};
  const defaultCurrent = {}, defaultStatus = {}, inventory = {}, inventoryRefresh = element();
  const requests = [];
  const api = {
    subscribe(topic, listener) { event = listener; },
    async request(url, options) {
      requests.push({ url, options });
      if (url === '/api/desktop/inventory')
        return { ok: true, json: async () => ({ id: 1 }) };
      if (url.startsWith('/api/desktop/inventory?'))
        return { ok: true, json: async () => ({ id: 1, status: 'succeeded', nodes: [
          { id: 7, 'node.name': 'sink.audiox', 'node.description': 'AudioX', 'media.class': 'Audio/Sink' },
        ] }) };
      if (url.includes('?session='))
        return { ok: true, json: async () => ({ name: 'sink.audiox' }) };
      if (url === '/api/desktop/default')
        return { ok: true, json: async () => ({ id: 4 }) };
      if (url === '/api/desktop/default?id=4')
        return { ok: true, json: async () => ({ id: 4, status: failed ? 'failed' : 'succeeded' }) };
      throw new Error(`Unexpected request: ${url}`);
    },
  };
  const dispose = create({ api, tab: {}, host: {}, os: {}, capabilities: {}, media: {},
    inventory, inventoryRefresh, inventoryStatus: {}, inventoryList: element(),
    defaultSection, defaultForm, defaultSink, defaultSubmit, defaultCurrent, defaultStatus,
    onDisconnect() {} });
  event({ connected: true, session: 9, hello: { hostname: 'pc', os: 'linux',
    capabilities: ['pipewire_nodes', 'default_sink'] } });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(defaultSection.hidden, false);
  assert.equal(defaultSink.value, 'sink.audiox');
  await defaultForm.listeners.submit({ preventDefault() {} });
  assert.ok(requests.some(request => request.url === '/api/desktop/default' &&
    request.options.body === `session=9&name_hex=${Buffer.from('sink.audiox').toString('hex')}`));
  assert.equal(defaultStatus.textContent, 'Default device changed.');
  failed = true;
  await defaultForm.listeners.submit({ preventDefault() {} });
  assert.match(defaultStatus.textContent, /failed/);
  event({ connected: false });
  assert.equal(defaultSection.hidden, true);
  dispose();
});

test('discovery displays names as text and discards results from an old desktop', async (t) => {
  documentForTest(t);
  let event, resolveResult, refresh;
  const pending = new Promise((resolve) => { resolveResult = resolve; });
  const inventory = {}, inventoryRefresh = {
    addEventListener(name, listener) { refresh = listener; },
    removeEventListener() {},
  }, inventoryStatus = {}, inventoryList = element();
  const api = {
    subscribe(topic, listener) { event = listener; },
    request(url) {
      if (url === '/api/desktop/inventory')
        return Promise.resolve({ ok: true, json: async () => ({ id: 1 }) });
      return pending;
    },
  };
  const dispose = create({ api, tab: {}, host: {}, os: {}, capabilities: {}, media: {},
    inventory, inventoryRefresh, inventoryStatus, inventoryList, onDisconnect() {} });
  const hello = { hostname: 'pc', os: 'linux', capabilities: ['pipewire_nodes'] };
  event({ connected: true, session: 1, hello });
  assert.equal(inventory.hidden, false);
  assert.equal(inventoryRefresh.disabled, true);
  await Promise.resolve();
  await Promise.resolve();
  event({ connected: false });
  resolveResult({ ok: true, json: async () => ({ id: 1, status: 'succeeded',
    nodes: [{ id: 7, 'node.name': '<node>', 'application.name': '<app>' }] }) });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(inventory.hidden, true);
  assert.equal(inventoryList.children[0].textContent, 'No nodes found');
  assert.equal(inventoryStatus.textContent, '');
  assert.equal(typeof refresh, 'function');
  dispose();
});

test('discovery renders available nodes without interpreting names as markup', async (t) => {
  documentForTest(t);
  let event;
  const inventoryList = element(), inventoryStatus = {};
  const api = {
    subscribe(topic, listener) { event = listener; },
    async request(url) {
      return { ok: true, async json() {
        return url.includes('?') ? { id: 1, status: 'succeeded', nodes: [
          { id: 7, 'node.name': '<node>', 'application.name': '<app>', 'media.class': 'Stream/Output/Audio' },
        ] } : { id: 1 };
      } };
    },
  };
  const dispose = create({ api, tab: {}, host: {}, os: {}, capabilities: {}, media: {},
    inventory: {}, inventoryRefresh: { addEventListener() {}, removeEventListener() {} },
    inventoryStatus, inventoryList, onDisconnect() {} });
  event({ connected: true, session: 1,
    hello: { hostname: 'pc', os: 'linux', capabilities: ['pipewire_nodes'] } });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(inventoryList.children[0].textContent, '<app> - <node> [Stream/Output/Audio]');
  assert.equal(inventoryStatus.textContent, '1 nodes found.');
  dispose();
});

test('automatic rules select discovered apps, devices and stereo pairs', async (t) => {
  documentForTest(t);
  let event, saved = '# audiox desktop routes v1\n';
  const requests = [];
  const api = {
    subscribe(topic, listener) { event = listener; },
    async request(url, options) {
      requests.push({ url, options });
      if (url.startsWith('/api/desktop/rules?')) saved = options.body;
      return { ok: true, async json() {
        if (url.startsWith('/api/desktop/rules')) return { status: 'succeeded', routing_ok: true, rules: saved };
        if (!url.includes('?')) return { id: 1 };
        return { id: 1, status: 'succeeded', nodes: [
          { id: 5, 'media.class': 'Stream/Output/Audio', 'application.name': 'Music', 'node.name': 'music' },
          { id: 6, 'media.class': 'Audio/Sink', 'node.name': 'usb.sink', 'node.description': 'USB',
            'audio.channels': 4 },
          { id: 7, 'media.class': 'Stream/Input/Audio', 'application.name': 'Recorder' },
          { id: 8, 'media.class': 'Audio/Source/Virtual', 'node.name': 'virtual.source',
            'audio.channels': 2 },
          { id: 9, 'media.class': 'Audio/Source/Virtual', 'node.name': 'audiox_auto_1_1',
            'audio.channels': 2 },
        ] };
      } };
    },
  };
  const autoDirection = element(), autoForm = element(), autoApp = element(), autoTarget = element();
  const autoPair = element(), autoRules = element(), autoStatus = element(), autoSubmit = element();
  const virtualChannels = element();
  autoDirection.value = 'p';
  const dispose = create({ api, tab: {}, host: {}, os: {}, capabilities: {}, media: {},
    inventory: {}, inventoryRefresh: element(), inventoryStatus: element(), inventoryList: element(),
    autoRouting: {}, autoForm, autoDirection, autoApp, autoTarget, autoPair,
    autoAppPattern: element(), autoTargetPattern: element(), autoSubmit, autoStatus, autoRules, virtualChannels,
    onDisconnect() {} });
  event({ connected: true, session: 8, hello: { hostname: 'pc', os: 'linux',
    capabilities: ['pipewire_nodes', 'auto_route'] } });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(virtualChannels.checked, true);
  virtualChannels.checked = false;
  virtualChannels.listeners.change();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(saved, '# audiox desktop routes v1\n# virtual-channels off\n');
  assert.equal(virtualChannels.checked, false);
  virtualChannels.checked = true;
  virtualChannels.listeners.change();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(saved, '# audiox desktop routes v1\n');
  assert.equal(autoApp.value, 'Music');
  assert.equal(autoTarget.value, 'usb.sink');
  assert.deepEqual(autoPair.children.map((option) => option.textContent), ['Channels 1/2', 'Channels 3/4']);
  autoPair.value = '1';
  autoForm.listeners.submit({ preventDefault() {} });
  await new Promise((resolve) => setImmediate(resolve));
  assert.match(saved, /\np\te\te\t1\t4d75736963\t7573622e73696e6b\n$/);
  assert.ok(requests.some((entry) => entry.url === '/api/desktop/rules?session=8' && entry.options.method === 'PUT'));
  assert.match(autoRules.children[0].children[0].textContent, /channels 3\/4/);
  autoRules.children[0].children[1].listeners.click();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(saved, '# audiox desktop routes v1\n');
  autoDirection.value = 'c';
  autoDirection.listeners.change();
  assert.equal(autoApp.value, 'Recorder');
  assert.equal(autoTarget.value, 'virtual.source');
  assert.equal(autoTarget.children.length, 1);
  assert.deepEqual(autoPair.children.map((option) => option.textContent), ['Channels 1/2']);
  dispose();
});
