const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

test('meter bank has explicit width so Firefox cannot collapse absolutely positioned channel bars', () => {
  const css = fs.readFileSync(require.resolve('../web/styles.css'), 'utf8');
  const bank = css.match(/\.vol-meter-bars\s*\{([^}]+)\}/)[1];
  assert.match(bank, /(?:^|;)\s*width:\s*100%\s*;/);
  assert.match(bank, /justify-content:\s*safe center\s*;/);
  assert.match(bank, /overflow-x:\s*auto\s*;/);
});

function fixture() {
  class Element {
    children = [];
    textContent = '';
    attributes = {};
    style = { setProperty: (name, value) => { this.attributes[name] = value; } };
    append(...children) { this.children.push(...children); }
    appendChild(child) { this.append(child); }
    replaceChildren() { this.children = []; }
    setAttribute(name, value) { this.attributes[name] = value; }
  }
  const status = new Element();
  const frames = new Map();
  let now = 0;
  let frameId = 0;
  const motion = { matches: false };
  const context = vm.createContext({
    window: { matchMedia: () => motion },
    performance: { now: () => now },
    requestAnimationFrame: callback => { frames.set(++frameId, callback); return frameId; },
    cancelAnimationFrame: id => frames.delete(id),
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
  const frame = elapsed => {
    now += elapsed;
    const callbacks = [...frames.values()];
    frames.clear();
    callbacks.forEach(callback => callback(now));
  };
  return { context, status, add, frame, frames, motion, poll: () => context.pollMeters() };
}

test('separate channel bars retain their peaks with one averaged reading per device', async () => {
  const f = fixture();
  const phone = f.add('bt_A4C6F0A57AA7_in');
  const headphones = f.add('bt_B8841102C99C_out');
  f.context.response = { meters: [
    { id: 'bt_A4C6F0A57AA7_in', levels: [0.5, 0.25] },
    { id: 'bt_B8841102C99C_out', levels: [1, 0] },
  ] };
  await f.poll();
  f.frame(3000);
  assert.equal(phone.children.length, 2);
  assert.equal(phone.children[0].children.length, 2);
  assert.equal(phone.children[1].textContent, '-8.5 dB');
  assert.ok(Math.abs(phone.children[0].children[0].children[0].value - (20 * Math.log10(0.5) + 60) / 60) < 1e-10);
  assert.ok(Math.abs(phone.children[0].children[1].children[0].value - (20 * Math.log10(0.25) + 60) / 60) < 1e-10);
  assert.equal(headphones.children[0].children[0].children[0].value, 1);
  assert.equal(headphones.children[0].children[1].children[0].value, 0);
  assert.equal(headphones.children[1].textContent, '-6.0 dB');
  assert.match(phone.children[0].children[0].children[0].attributes['aria-label'], /A4C6F0A57AA7_in channel 1 signal level/);
  assert.equal(phone.children[0].children[0].children[0].attributes['aria-valuetext'], '-6.0 dBFS');
  assert.ok(Math.abs(parseFloat(phone.children[0].children[0].attributes['--meter-level']) -
    phone.children[0].children[0].children[0].value * 100) < 1e-10);
  f.context.response.meters[0].levels = [0];
  await f.poll();
  assert.equal(phone.children[0].children.length, 1);
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
  assert.equal(f.status.hidden, false);
  assert.equal(group.children[0].children[0].children[0].value, 0);
  assert.equal(group.children[0].children[0].attributes['--meter-level'], '0%');
  assert.equal(group.children[0].children.length, 2);
  assert.equal(group.children[0].children[1].children[0].value, 0);
  f.context.fetch = async () => ({ ok: true, json: async () => ({ meters: [{ id: 'phone', levels: [NaN] }] }) });
  await f.poll();
  assert.match(f.status.textContent, /Invalid meter data/);
  f.context.fetch = async () => ({ ok: true, json: async () => f.context.response });
  await f.poll();
  assert.equal(f.status.textContent, '');
  assert.equal(f.status.hidden, true);
  f.frame(3000);
  assert.equal(group.children[0].children[0].children[0].value, 1);
  assert.equal(group.children[1].textContent, '-2.5 dB');
});

test('bars animate in dB with fast attack and slower release while numbers show raw snapshots', async () => {
  const f = fixture();
  const group = f.add('phone');
  f.context.response = { meters: [{ id: 'phone', levels: [1] }] };
  await f.poll();
  const meter = group.children[0].children[0].children[0];
  assert.equal(meter.value, 0);
  assert.equal(group.children[1].textContent, '0.0 dB');
  f.frame(35);
  assert.ok(Math.abs(meter.value - (1 - Math.exp(-1))) < 1e-10);
  f.frame(300);
  assert.equal(meter.value, 1);
  assert.equal(f.frames.size, 0);
  f.context.response.meters[0].levels = [0];
  await f.poll();
  assert.equal(group.children[1].textContent, '-inf dB');
  f.frame(300);
  assert.ok(Math.abs(meter.value - Math.exp(-1)) < 1e-10);
  f.frame(3000);
  assert.equal(meter.value, 0);
  assert.equal(f.frames.size, 0);
});

test('meter motion is time-based rather than frame-rate dependent and only schedules one loop', async () => {
  const a = fixture(), b = fixture();
  for (const f of [a, b]) {
    f.add('phone');
    f.context.response = { meters: [{ id: 'phone', levels: [0.5] }] };
    await f.poll();
    await f.poll();
    assert.equal(f.frames.size, 1);
  }
  a.frame(32);
  b.frame(16);
  b.frame(16);
  assert.ok(Math.abs(a.context.group.children[0].children[0].children[0].value -
    b.context.group.children[0].children[0].children[0].value) < 1e-10);
});

test('errors and stopping polling cancel animation and immediately clear displayed levels', async () => {
  const f = fixture();
  const group = f.add('phone');
  f.context.response = { meters: [{ id: 'phone', levels: [1] }] };
  await f.poll();
  f.frame(35);
  assert.ok(group.children[0].children[0].children[0].value > 0);
  f.context.fetch = async () => ({ ok: false, status: 503 });
  await f.poll();
  assert.equal(f.frames.size, 0);
  assert.equal(group.children[0].children[0].children[0].value, 0);
  assert.equal(group.children[1].textContent, '-inf dB');
  f.context.updateVolumeMeter('phone', [1]);
  assert.equal(f.frames.size, 1);
  f.context.stopMeterPolling();
  assert.equal(f.frames.size, 0);
  assert.equal(group.children[0].children[0].children[0].value, 0);
});

test('reduced motion shows snapshots directly and also stops an animation already in flight', async () => {
  const f = fixture();
  const group = f.add('phone');
  f.motion.matches = true;
  f.context.response = { meters: [{ id: 'phone', levels: [1] }] };
  await f.poll();
  assert.equal(group.children[0].children[0].children[0].value, 1);
  assert.equal(f.frames.size, 0);
  f.motion.matches = false;
  f.context.updateVolumeMeter('phone', [0]);
  assert.equal(f.frames.size, 1);
  f.motion.matches = true;
  f.frame(16);
  assert.equal(group.children[0].children[0].children[0].value, 0);
  assert.equal(f.frames.size, 0);
});

test('hidden pages clear animation and detached channels cannot keep a frame loop alive', async () => {
  const f = fixture();
  const group = f.add('phone');
  f.context.response = { meters: [{ id: 'phone', levels: [1, 0.5] }] };
  await f.poll();
  f.context.document.hidden = true;
  f.frame(16);
  assert.equal(f.frames.size, 0);
  assert.equal(group.children[0].children[0].children[0].value, 0);
  f.context.document.hidden = false;
  await f.poll();
  assert.equal(f.frames.size, 1);
  f.context.response = { meters: [] };
  await f.poll();
  assert.equal(group.children.length, 0);
  f.frame(16);
  assert.equal(f.frames.size, 0);
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
  assert.equal(f.status.textContent, '');
  assert.equal(f.status.hidden, true);
});

test('one compact average covers mono, silence, multichannel and empty channel lists', () => {
  const f = fixture();
  const group = f.add('phone');
  f.context.state.volumes = [{ id: 'phone', name: 'My phone' }];
  for (const [levels, reading] of [
    [[0.5], '-6.0 dB'],
    [[0, 0], '-inf dB'],
    [Array(16).fill(0.25), '-12.0 dB'],
    [[1, 0], '-6.0 dB'],
    [[2, -1], '-6.0 dB'],
  ]) {
    f.context.updateVolumeMeter('phone', levels, true);
    assert.equal(group.children.length, 2);
    assert.equal(group.children[0].children.length, levels.length);
    assert.equal(group.children[1].textContent, reading);
    for (const [channel, row] of group.children[0].children.entries()) {
      assert.equal(row.children.length, 1);
      assert.equal(row.children[0].attributes['aria-label'], `My phone channel ${channel + 1} signal level`);
      assert.equal(row.textContent, '');
    }
  }
  f.context.updateVolumeMeter('phone', []);
  assert.equal(group.children.length, 0);
});
