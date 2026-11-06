const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../../web/embedded/app.js'), 'utf8');

function fixture(gain = 1, volumes = [{ id: 'phone', name: 'Phone', gain }]) {
  class Element {
    children = [];
    listeners = {};
    attributes = {};
    classList = { add() {} };
    set innerHTML(value) { this.children = []; }
    appendChild(child) { this.children.push(child); }
    addEventListener(name, callback) { this.listeners[name] = callback; }
    setAttribute(name, value) { this.attributes[name] = value; }
  }
  const grid = new Element();
  const summary = new Element();
  const saves = [];
  const context = apiContext({
    volGridEl: grid,
    state: { volumes, ccVolumes: [], volShowEffects: false },
    volumeMeters: new Map(),
    document: { createElement: () => new Element(), getElementById: () => summary },
    clearTimeout() {},
    setTimeout(callback, delay) { assert.equal(delay, 120); callback(); return 1; },
    setVolumeGain: (id, value) => saves.push({ id, value }),
  });
  vm.runInContext(source.slice(source.indexOf('function isEffectThingId('),
    source.indexOf('function isRoutingHelperNode(')), context);
  vm.runInContext(source.slice(source.indexOf('function ccForThing('),
    source.indexOf('\nasync function ', source.indexOf('function renderVolGrid('))), context);
  context.renderVolGrid();
  const controls = grid.children[0].children.find(child => child.className === 'vol-channel-controls');
  const row = controls?.children[0];
  const unity = grid.children[0].children.find(child => child.className === 'flat vol-unity');
  return { context, grid, summary, controls, unity, slider: row?.children[0], label: row?.children[1], saves };
}

test('volume percentages map silence, half gain and unity without doubling gain', () => {
  const { context } = fixture();
  for (const [gain, percent] of [[0, 0], [0.25, 25], [0.5, 50], [1, 100], [2, 200]]) {
    assert.equal(context.gainToPercent(gain), percent);
    assert.equal(context.percentToGain(String(percent)), gain);
  }
});

test('mixer strips place vertical outgoing faders beside meters with accessible labels and unity reset', async () => {
  const f = fixture(0.5);
  const controls = f.controls;
  assert.equal(controls.className, 'vol-channel-controls');
  assert.equal(controls.children[0].className, 'vol-slider-row');
  assert.equal(controls.children[1].className, 'vol-meters');
  assert.equal(f.slider.attributes['aria-orientation'], 'vertical');
  assert.equal(f.slider.attributes['aria-label'], 'Phone outgoing gain');
  assert.equal(f.slider.attributes['aria-valuetext'], '50%');
  await f.unity.listeners.click();
  assert.equal(f.slider.value, '100');
  assert.equal(f.label.textContent, '100%');
  assert.deepEqual(f.saves, [{ id: 'phone', value: 1 }]);
  const css = fs.readFileSync(require.resolve('../../web/embedded/styles.css'), 'utf8');
  assert.match(css, /\.vol-grid\s*\{[^}]*overflow-x:\s*auto/);
  assert.match(css, /\.vol-slider-row input\[type="range"\]\s*\{[^}]*writing-mode:\s*vertical-lr;[^}]*direction:\s*rtl/s);
});

test('effect channels are hidden by default without changing saved gains or MIDI mappings', () => {
  const volumes = [
    { id: 'phone', name: 'Phone', gain: 1 },
    { id: 'fx_1', name: 'Reverb', gain: 0.5 },
    { id: 'routing_helper', name: 'Audio Mixer', gain: 1 },
    { id: 'fx_2', name: 'Gain', gain: 1 },
  ];
  const f = fixture(1, volumes);
  f.context.state.ccVolumes = [{ id: 'fx_1', cc: 7 }];
  const before = JSON.stringify(f.context.state);
  f.context.renderVolGrid();
  assert.equal(f.grid.children.length, 1);
  assert.match(f.summary.textContent, /2 effect channel.*hidden; 1 with adjusted gain/);
  assert.equal(f.context.volumeMeters.has('fx_1'), false);
  assert.equal(JSON.stringify(f.context.state), before);
  f.context.state.volShowEffects = true;
  f.context.renderVolGrid();
  assert.equal(f.grid.children.length, 3);
  assert.equal(f.summary.hidden, true);
  assert.equal(f.context.volumeMeters.has('fx_1'), true);
  assert.equal(f.context.state.volumes[1].gain, 0.5);
  assert.equal(f.context.state.ccVolumes[0].cc, 7);
  assert.equal(f.saves.length, 0);
  assert.equal(f.context.volumeMeters.has('routing_helper'), false);
});

test('a compiled routing helper never creates a volume strip or a misleading effects-only message', () => {
  const f = fixture(1, [{ id: 'routing_helper', name: 'Audio Mixer', gain: 2 }]);
  for (const showEffects of [false, true]) {
    f.context.state.volShowEffects = showEffects;
    f.context.renderVolGrid();
    assert.equal(f.grid.children.length, 1);
    assert.match(f.grid.children[0].textContent, /No active devices/);
    assert.equal(f.context.volumeMeters.has('routing_helper'), false);
    assert.equal(f.context.state.volumes[0].gain, 2);
    assert.equal(f.saves.length, 0);
  }
});
test('an effect-only graph explains how to reveal hidden channels', () => {
  const f = fixture(1, [{ id: 'fx_1', name: 'Reverb', gain: 1 }]);
  assert.match(f.grid.children[0].textContent, /Enable Show effect channels/);
  f.context.state.volShowEffects = true;
  f.context.renderVolGrid();
  assert.equal(f.grid.children[0].className, 'vol-card');
});

test('hiding effect channels cancels their pending MIDI assignment without changing mappings', () => {
  const f = fixture();
  let change;
  const cancelled = [];
  const status = {};
  f.context.document.getElementById = () => ({ addEventListener: (type, callback) => {
    assert.equal(type, 'change');
    change = callback;
  } });
  f.context.clearInterval = id => cancelled.push(id);
  f.context.volCcStatusEl = status;
  f.context.renderVolGrid = () => {};
  f.context.state.volAssignTarget = 'fx_7';
  f.context.state.volCcPollTimer = 42;
  f.context.state.ccVolumes = [{ id: 'fx_7', cc: 7 }];
  const start = source.indexOf("document.getElementById('vol-show-effects').addEventListener");
  vm.runInContext(source.slice(start, source.indexOf("\ndocument.getElementById(", start + 1)), f.context);
  change({ currentTarget: { checked: false } });
  assert.equal(f.context.state.volShowEffects, false);
  assert.equal(f.context.state.volAssignTarget, null);
  assert.equal(f.context.state.volCcPollTimer, null);
  assert.deepEqual(cancelled, [42]);
  assert.match(status.textContent, /cancelled/);
  assert.equal(f.context.state.ccVolumes[0].cc, 7);
  change({ currentTarget: { checked: true } });
  assert.equal(f.context.state.volShowEffects, true);
});

test('a failed unity reset restores the saved level rather than pretending it was applied', async () => {
  const f = fixture(0.5);
  f.context.setVolumeGain = async () => false;
  await f.unity.listeners.click();
  assert.equal(f.slider.value, '50');
  assert.equal(f.label.textContent, '50%');
  assert.equal(f.slider.attributes['aria-valuetext'], '50%');
});

test('volume save reports actual percentages and friendly names, and explicitly reports failures', async () => {
  const messages = [];
  const context = apiContext({
    state: { volumes: [{ id: 'phone', name: 'My phone', gain: 1 }] },
    gainToPercent: gain => Math.round(gain * 100),
    setStatus: (...args) => messages.push(args),
    fetch: async () => ({ ok: true, text: async () => 'ok' }),
  });
  vm.runInContext(source.slice(source.indexOf('async function setVolumeGain('),
    source.indexOf('async function pollVolCcCapture(')), context);
  assert.equal(await context.setVolumeGain('phone', 1.5), true);
  assert.equal(context.state.volumes[0].gain, 1.5);
  assert.deepEqual(messages[0], ['My phone volume set to 150%', 'ok']);
  context.fetch = async () => ({ ok: false, status: 500, text: async () => 'write failed' });
  assert.equal(await context.setVolumeGain('phone', 1), false);
  assert.equal(context.state.volumes[0].gain, 1.5);
  assert.match(messages[1][0], /500 write failed/);
});

test('volume slider sends 0–2 gain, with 100% at unity and 200% at double gain', () => {
  const f = fixture();
  assert.equal(f.slider.value, '100');
  assert.equal(f.label.textContent, '100%');
  assert.equal(f.slider.min, '0');
  assert.equal(f.slider.max, '200');
  for (const [percent, gain] of [[50, 0.5], [100, 1], [150, 1.5], [200, 2], [0, 0]]) {
    f.slider.value = String(percent);
    f.slider.listeners.input();
    assert.equal(f.label.textContent, `${percent}%`);
    assert.deepEqual(f.saves.at(-1), { id: 'phone', value: gain });
  }
});

test('gains above unity set both the slider and label without silently resaving', () => {
  const f = fixture(1.5);
  assert.equal(f.slider.value, '150');
  assert.equal(f.label.textContent, '150%');
  assert.equal(f.saves.length, 0);
  f.slider.value = '75';
  f.slider.listeners.input();
  assert.equal(f.label.textContent, '75%');
  assert.equal(f.saves[0].value, 0.75);
  const maximum = fixture(2);
  assert.equal(maximum.slider.value, '200');
  assert.equal(maximum.label.textContent, '200%');
  assert.equal(maximum.saves.length, 0);
});
