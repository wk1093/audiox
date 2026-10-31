const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');

function fixture(gain = 1) {
  class Element {
    children = [];
    listeners = {};
    appendChild(child) { this.children.push(child); }
    addEventListener(name, callback) { this.listeners[name] = callback; }
  }
  const grid = new Element();
  const saves = [];
  const context = vm.createContext({
    volGridEl: grid,
    state: { volumes: [{ id: 'phone', name: 'Phone', gain }], ccVolumes: [] },
    volumeMeters: new Map(),
    document: { createElement: () => new Element() },
    clearTimeout() {},
    setTimeout(callback, delay) { assert.equal(delay, 120); callback(); return 1; },
    setVolumeGain: (id, value) => saves.push({ id, value }),
  });
  vm.runInContext(source.slice(source.indexOf('function ccForThing('),
    source.indexOf('\nasync function ', source.indexOf('function renderVolGrid('))), context);
  context.renderVolGrid();
  const row = grid.children[0].children[3];
  return { context, slider: row.children[0], label: row.children[1], saves };
}

test('volume percentages map silence, half gain and unity without doubling gain', () => {
  const { context } = fixture();
  for (const [gain, percent] of [[0, 0], [0.25, 25], [0.5, 50], [1, 100], [2, 200]]) {
    assert.equal(context.gainToPercent(gain), percent);
    assert.equal(context.percentToGain(String(percent)), gain);
  }
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
