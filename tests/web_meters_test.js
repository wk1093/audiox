const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

function fixture() {
  class Element {
    children = [];
    textContent = '';
    attributes = {};
    append(...children) { this.children.push(...children); }
    appendChild(child) { this.append(child); }
    replaceChildren() { this.children = []; }
    setAttribute(name, value) { this.attributes[name] = value; }
  }
  const status = new Element();
  const context = vm.createContext({
    document: {
      hidden: false,
      getElementById: () => status,
      createElement: () => new Element(),
    },
    state: { activeTab: 'volume' },
    AudioxUI: { formatMessage: String },
    AbortSignal,
    setInterval: (fn, delay) => { context.interval = { fn, delay }; return 1; },
    clearInterval: () => { context.interval = null; },
    loadVolumes: async () => { context.reloads++; return true; },
    reloads: 0,
    requests: 0,
    response: { meters: [] },
    fetch: async (url, options) => {
      context.requests++;
      assert.equal(url, '/api/audio/meters');
      assert.equal(options.cache, 'no-store');
      return { ok: true, json: async () => context.response };
    },
  });
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  vm.runInContext(source.slice(source.indexOf('const volumeMeters ='),
    source.indexOf('async function loadVolumes()')), context);
  const add = id => {
    const group = new Element();
    context.group = group;
    context.id = id;
    vm.runInContext('volumeMeters.set(id, group)', context);
    return group;
  };
  return { context, status, add, poll: () => context.pollMeters() };
}

test('meters keep devices and channels separate, use a dB scale, and resize on channel changes', async () => {
  const f = fixture();
  const phone = f.add('bt_A4C6F0A57AA7_in');
  const headphones = f.add('bt_B8841102C99C_out');
  f.context.response = { meters: [
    { id: 'bt_A4C6F0A57AA7_in', levels: [0.5, 0.25] },
    { id: 'bt_B8841102C99C_out', levels: [1, 0] },
  ] };
  await f.poll();
  assert.equal(phone.children.length, 2);
  assert.equal(phone.children[0].children[2].textContent, '-6.0 dBFS');
  assert.equal(phone.children[1].children[2].textContent, '-12.0 dBFS');
  assert.ok(Math.abs(phone.children[0].children[1].value - 0.89966) < 0.0001);
  assert.equal(headphones.children[0].children[1].value, 1);
  assert.equal(headphones.children[1].children[1].value, 0);
  assert.match(phone.children[0].children[1].attributes['aria-label'], /A4C6F0A57AA7_in channel 1/);
  f.context.response.meters[0].levels = [0];
  await f.poll();
  assert.equal(phone.children.length, 1);
  assert.equal(f.context.reloads, 1);
  f.context.response.meters.pop();
  await f.poll();
  assert.equal(headphones.children.length, 0);
  assert.equal(f.context.reloads, 2);
});

test('polling is limited to the visible Volume page and uses a 250 ms timer', async () => {
  const f = fixture();
  f.context.state.activeTab = 'routing';
  await f.poll();
  f.context.state.activeTab = 'volume';
  f.context.document.hidden = true;
  await f.poll();
  assert.equal(f.context.requests, 0);
  f.context.startMeterPolling();
  assert.equal(f.context.interval, undefined);
  f.context.document.hidden = false;
  f.context.startMeterPolling();
  assert.equal(f.context.interval.delay, 250);
  f.context.stopMeterPolling();
  assert.equal(f.context.interval, null);
});

test('a slow request never overlaps another poll and leaving the page discards its result', async () => {
  const f = fixture();
  const group = f.add('phone');
  let release;
  f.context.fetch = () => {
    f.context.requests++;
    return new Promise(resolve => { release = resolve; });
  };
  const pending = f.poll();
  await f.poll();
  assert.equal(f.context.requests, 1);
  f.context.state.activeTab = 'routing';
  release({ ok: true, json: async () => ({ meters: [{ id: 'phone', levels: [1] }] }) });
  await pending;
  assert.equal(group.children.length, 0);
});

test('failed or malformed meter responses clear stale levels and report an inline error', async () => {
  const f = fixture();
  const group = f.add('phone');
  f.context.response = { meters: [{ id: 'phone', levels: [1, 0.5] }] };
  await f.poll();
  f.context.fetch = async () => ({ ok: false, status: 503 });
  await f.poll();
  assert.match(f.status.textContent, /Meters unavailable.*503/);
  assert.equal(group.children[0].children[1].value, 0);
  f.context.fetch = async () => ({ ok: true, json: async () => ({ meters: [{ id: 'phone', levels: [NaN] }] }) });
  await f.poll();
  assert.match(f.status.textContent, /Invalid meter data/);
  f.context.fetch = async () => ({ ok: true, json: async () => f.context.response });
  await f.poll();
  assert.match(f.status.textContent, /4 updates\/s/);
  assert.equal(group.children[0].children[1].value, 1);
});

test('a failed topology refresh is reported and retried on the next poll', async () => {
  const f = fixture();
  await f.poll();
  f.context.response = { meters: [{ id: 'new-phone', levels: [0, 0] }] };
  f.context.loadVolumes = async () => { f.context.reloads++; return false; };
  await f.poll();
  assert.match(f.status.textContent, /Could not refresh meter channels/);
  f.context.loadVolumes = async () => { f.context.reloads++; return true; };
  await f.poll();
  assert.equal(f.context.reloads, 2);
  assert.match(f.status.textContent, /4 updates\/s/);
});
