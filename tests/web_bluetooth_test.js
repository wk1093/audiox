const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

function fixture(devices) {
  class Element {
    children = [];
    dataset = {};
    textContent = '';
    listeners = {};
    checked = true;
    attributes = {};
    append(...children) {
      for (const child of children) child.parent = this;
      this.children.push(...children);
    }
    after(child) {
      child.parent = this.parent;
      this.parent.children.splice(this.parent.children.indexOf(this) + 1, 0, child);
    }
    replaceChildren() { this.children = []; }
    contains(child) { return this.children.includes(child); }
    setAttribute(name, value) { this.attributes[name] = value; }
    addEventListener(event, callback) { this.listeners[event] = callback; }
    querySelectorAll(tag) {
      return this.children.flatMap(child =>
        [...((child.tag === tag || (tag === '[data-device-action]' && child.dataset.deviceAction))
          ? [child] : []), ...child.querySelectorAll(tag)]);
    }
    get text() { return this.textContent + this.children.map(child => child.text).join(' '); }
  }
  const elements = new Map(['bluetooth-hide-unnamed', 'bluetooth-devices', 'bluetooth-status',
    'btn-bluetooth-scan', 'bluetooth-scan-hint'].map(id => [id, new Element()]));
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  let fetches = 0;
  const requests = [];
  const notices = [];
  const context = vm.createContext({
    document: {
      getElementById: id => elements.get(id),
      createElement: tag => Object.assign(new Element(), { tag }),
      activeElement: null,
    },
    bluetoothDevicesEl: elements.get('bluetooth-devices'),
    bluetoothStatusEl: elements.get('bluetooth-status'),
    AudioxUI: { isBusy: () => false, resolve() {}, formatMessage: String },
    disconnectBluetoothDevice() {},
    state: { activeTab: 'routing' },
    AbortSignal,
    setStatus: (...args) => notices.push(args),
    fetch: async (path, options = {}) => {
      fetches++;
      if (options.method === 'POST') requests.push([path, options.body]);
      return { ok: true, json: async () => ({ ok: true, ready: true, devices }) };
    },
    clearTimeout,
    setTimeout,
  });
  vm.runInContext(source.slice(source.indexOf('const bluetoothHideUnnamed ='),
    source.indexOf('function setRoutingZoo')), context);
  return {
    root: elements.get('bluetooth-devices'),
    filter: elements.get('bluetooth-hide-unnamed'),
    load: () => context.loadBluetoothDevices(),
    fetches: () => fetches,
    requests,
    notices,
  };
}

const device = (name, suffix, options = {}) => ({
  name, address: `00:11:22:33:44:${suffix}`, classic: true,
  paired: false, connected: false, ...options,
});

test('default filter hides blank, whitespace, and address-only nearby names but keeps saved devices', async () => {
  const f = fixture([
    device('Speaker', '01'),
    device('', '02'),
    device('   ', '03'),
    device('00:11:22:33:44:04', '04'),
    device('00-11-22-33-44-0A', '0a'),
    device('', '05', { paired: true }),
    device('', '06', { paired: true, connected: true }),
    device('LE beacon', '07', { classic: false }),
  ]);
  await f.load();
  assert.match(f.root.text, /Speaker/);
  assert.match(f.root.text, /4 unnamed device\(s\) hidden/);
  for (const suffix of ['02', '03', '04', '0a', '07'])
    assert.ok(!f.root.text.includes(`00:11:22:33:44:${suffix}`));
  assert.match(f.root.children[0].text, /00:11:22:33:44:05/);
  assert.match(f.root.children[0].text, /00:11:22:33:44:06/);
  assert.doesNotMatch(f.root.text, /Ready to pair|LE beacon/);
  assert.match(f.root.text, /put device in pairing mode/);
  assert.equal(f.notices.length, 0);
});

test('switch reveals cached unnamed devices immediately without rescanning; LE stays excluded', async () => {
  const f = fixture([device('', '01'), device('Beacon', '02', { classic: false })]);
  await f.load();
  assert.match(f.root.text, /1 unnamed device\(s\) hidden/);
  assert.equal(f.root.querySelectorAll('button').length, 0);
  f.filter.checked = false;
  f.filter.listeners.change();
  assert.match(f.root.text, /00:11:22:33:44:01/);
  assert.doesNotMatch(f.root.text, /Beacon|hidden/);
  assert.equal(f.root.querySelectorAll('button')[0].textContent, 'Pair');
  assert.equal(f.fetches(), 1);
  await f.load();
  assert.match(f.root.text, /00:11:22:33:44:01/);
  assert.equal(f.filter.checked, false);
  f.filter.checked = true;
  f.filter.listeners.change();
  assert.match(f.root.text, /Turn off Hide unnamed devices/);
});

test('names learned by subsequent polls appear with the filter enabled', async () => {
  const devices = [device('', '01')];
  const f = fixture(devices);
  await f.load();
  assert.doesNotMatch(f.root.text, /Headphones/);
  devices[0].name = 'Headphones';
  await f.load();
  assert.match(f.root.text, /Headphones/);
  assert.doesNotMatch(f.root.text, /hidden/);
});

test('saved devices expose an Auto-connect switch that persists changes', async () => {
  const f = fixture([
    device('Speaker', '01', { paired: true }),
    device('Headphones', '02', { paired: true, auto_connect: false }),
  ]);
  await f.load();
  const switches = f.root.querySelectorAll('input');
  assert.equal(switches.length, 2);
  const speakerSwitch = switches.find(input => input.attributes['aria-label'] === 'Auto-connect Speaker');
  const headphoneSwitch = switches.find(input => input.attributes['aria-label'] === 'Auto-connect Headphones');
  assert.ok(speakerSwitch);
  assert.ok(headphoneSwitch);
  assert.equal(speakerSwitch.checked, true);
  assert.equal(headphoneSwitch.checked, false);

  speakerSwitch.checked = false;
  await speakerSwitch.listeners.change();
  assert.equal(f.requests[0][0], '/api/bluetooth/auto-connect');
  assert.equal(String(f.requests[0][1]), 'address=00:11:22:33:44:01&enabled=false');
});

test('saved audio inputs offer Auto-connect and show their connection state', async () => {
  const f = fixture([
    device('Speaker', '01', { paired: true }),
    device('Phone', '02', { paired: true, auto_connect: true, audio_source: true }),
    device('Headset', '03', { paired: true, audio_source: true, audio_sink: true }),
  ]);
  await f.load();
  const switches = f.root.querySelectorAll('input');
  assert.deepEqual(switches.map(input => input.attributes['aria-label']).sort(),
    ['Auto-connect Headset', 'Auto-connect Phone', 'Auto-connect Speaker']);
  assert.match(f.root.text, /Phone Audio input, not connected/);
  assert.match(f.root.text, /Speaker Paired, not connected/);
});

test('connected devices report each direction and disconnect by address', async () => {
  const f = fixture([
    device('Phone', '02', { paired: true, connected: true, audio_source: true,
      input_connected: true, input_streaming: false, output_connected: false }),
    device('Transceiver', '03', { paired: true, connected: true, streaming: true, audio_source: true, audio_sink: true,
      input_connected: true, input_streaming: true, output_connected: true, output_streaming: true }),
  ]);
  await f.load();
  assert.match(f.root.text, /Phone Connected as audio input; waiting for audio/);
  assert.match(f.root.text,
    /Transceiver Connected as audio output · Connected as audio input; receiving audio/);
  const disconnect = f.root.querySelectorAll('button')
    .find(button => button.attributes['aria-label'] === 'Disconnect Phone');
  await disconnect.listeners.click();
  assert.deepEqual(f.requests[0], ['/api/bluetooth/disconnect', 'address=00:11:22:33:44:02']);
});

test('Bluetooth page exposes a checked and labelled switch with an explanation', () => {
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  assert.match(html, /<label class="bluetooth-filter">\s*<input id="bluetooth-hide-unnamed"[^>]*role="switch"[^>]*checked[^>]*>\s*Hide unnamed devices\s*<\/label>/);
  assert.match(html, /id="bluetooth-filter-hint"[^>]*>Only Classic Bluetooth is scanned/);
});

function routingFixture(thingIds) {
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const slice = (start, end) => source.slice(source.indexOf(start), source.indexOf(end));
  const nodeByThingId = new Map(thingIds.map(([id, inputs, outputs]) => [id, {
    inputs: Array.from({ length: inputs }), outputs: Array.from({ length: outputs }),
  }]));
  const context = vm.createContext({
    state: {
      graph: {}, nodes: [], edges: [], dormantEdges: [], selectedEdgeIdx: -1, nodeByThingId,
      graphCanvas: { setDirty() {} },
    },
    renderEdgesList() {},
  });
  vm.runInContext([
    slice('function edgeKey(', 'function findEdgeIndex('),
    slice('function formatRoutingText(', 'function makeThingsSignature('),
    slice('function normalizeThingId(', 'function isDeviceNode('),
    slice('function getOutputChannelCount(', 'function addEdgeFromConnection('),
  ].join('\n'), context);
  return context;
}

test('routing save keeps lines for Bluetooth devices that are not paired', () => {
  const ctx = routingFixture([
    ['alsa_usb_dev0_in', 0, 2],
    ['alsa_usb_dev0_out', 2, 0],
    ['bt_001122334401_out', 2, 0],
  ]);
  ctx.state.edges = [
    { src: 'alsa_usb_dev0_in', dst: 'bt_001122334401_out', srcChannel: 0, dstChannel: 0 },
    { src: 'alsa_usb_dev0_in', dst: 'bt_A4C6F0A57AA7_out', srcChannel: 0, dstChannel: 0 },
    { src: 'bt_A4C6F0A57AA7_in', dst: 'alsa_usb_dev0_out', srcChannel: 1, dstChannel: 1 },
    { src: 'bluetooth_in', dst: 'alsa_usb_dev0_out', srcChannel: 0, dstChannel: 0 },
    { src: 'alsa_gone_dev0_in', dst: 'alsa_usb_dev0_out', srcChannel: 0, dstChannel: 0 },
  ];
  ctx.syncEdgesFromGraph();
  assert.equal(ctx.state.edges.length, 1);
  assert.equal(ctx.state.dormantEdges.length, 3);
  const text = ctx.formatRoutingText();
  assert.match(text, /edge=alsa_usb_dev0_in,bt_001122334401_out,0,0/);
  assert.match(text, /edge=alsa_usb_dev0_in,bt_A4C6F0A57AA7_out,0,0/);
  assert.match(text, /edge=bt_A4C6F0A57AA7_in,alsa_usb_dev0_out,1,1/);
  assert.match(text, /edge=bluetooth_in,alsa_usb_dev0_out,0,0/);
  assert.doesNotMatch(text, /alsa_gone/);

  ctx.state.nodeByThingId.set('bt_A4C6F0A57AA7_in', { inputs: [], outputs: [{}, {}] });
  ctx.syncEdgesFromGraph();
  assert.equal(ctx.state.edges.length, 2);
  assert.ok(ctx.state.edges.some(edge => edge.src === 'bt_A4C6F0A57AA7_in'));
  assert.equal(ctx.state.dormantEdges.length, 2);
});
