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

test('Bluetooth page exposes a checked and labelled switch with an explanation', () => {
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  assert.match(html, /<label class="bluetooth-filter">\s*<input id="bluetooth-hide-unnamed"[^>]*role="switch"[^>]*checked[^>]*>\s*Hide unnamed devices\s*<\/label>/);
  assert.match(html, /id="bluetooth-filter-hint"[^>]*>Only Classic Bluetooth is scanned/);
});
