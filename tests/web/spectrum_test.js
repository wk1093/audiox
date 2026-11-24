const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../../web/embedded/app/spectrum.js'), 'utf8');

function fixture() {
  class Element {
    listeners = {};
    textContent = '';
    value = '';
    checked = true;
    open = false;
    children = [];
    addEventListener(type, fn) { this.listeners[type] = fn; }
    replaceChildren(...children) { this.children = children; }
    showModal() { this.open = true; }
    close() { this.open = false; this.listeners.close?.(); }
  }
  const select = new Element();
  Object.defineProperty(select, 'value', {
    get() { return this.selected ?? ''; },
    set(value) { this.selected = this.children.some((o) => o.value === value) ? value : ''; },
  });
  const elements = new Map(['spectrum-dialog', 'spectrum-canvas', 'spectrum-status', 'spectrum-peaks',
    'btn-vol-spectrum', 'btn-spectrum-close'].map((id) => [id, new Element()]));
  elements.set('spectrum-node', select);
  const storage = new Map();
  const context = apiContext({
    window: { matchMedia: () => ({ matches: false }), devicePixelRatio: 1 },
    document: { hidden: false, getElementById: (id) => elements.get(id) },
    localStorage: { getItem: (k) => storage.get(k) ?? null, setItem: (k, v) => storage.set(k, v) },
    Option: class { constructor(text, value) { this.text = text; this.value = value; } },
    getComputedStyle: () => ({ getPropertyValue: () => '' }),
    requestAnimationFrame: () => 7,
    cancelAnimationFrame: () => { context.cancelled = true; },
    setInterval: (fn, delay) => { context.interval = { fn, delay }; return 3; },
    clearInterval: () => { context.interval = null; },
    AbortSignal,
    state: { volumes: [
      { id: 'soundboard_out', name: 'Soundboard' },
      { id: 'fx_1', name: 'Reverb' },
      { id: 'usb_gadget_in', name: 'From computer' },
      { id: 'usb_gadget_out', name: 'To computer' },
    ] },
    loadVolumes: async () => true,
    urls: [],
    responses: [],
    fetch: async (url, options) => {
      context.urls.push(url);
      assert.equal(options.cache, 'no-store');
      const body = context.responses.shift() ?? { active: true, min_hz: 20, max_hz: 20000, bands: [-6, -40] };
      return { ok: true, json: async () => body };
    },
  });
  vm.runInContext(source, context);
  return { context, elements, select, storage };
}

test('opening the spectrum defaults to an output channel and polls it until closed', async () => {
  const { context, elements, select } = fixture();
  context.responses.push({ active: false, min_hz: 20, max_hz: 20000, bands: [-120, -120] });
  await elements.get('btn-vol-spectrum').listeners.click();
  await new Promise(setImmediate);
  assert.equal(elements.get('spectrum-dialog').open, true);
  assert.equal(select.value, 'usb_gadget_out');
  assert.deepEqual(context.urls, ['/api/audio/spectrum?id=usb_gadget_out']);
  assert.equal(context.interval.delay, 50);
  assert.match(elements.get('spectrum-status').textContent, /Waiting for audio/);

  await context.interval.fn();
  assert.equal(elements.get('spectrum-status').textContent, '');
  assert.equal(vm.runInContext('JSON.stringify(spectrumView.targets)', context), '[-6,-40]');

  elements.get('btn-spectrum-close').listeners.click();
  assert.equal(context.interval, null);
  assert.equal(context.cancelled, true);
});

test('changing channel remembers the choice and discards the previous bands', async () => {
  const { context, elements, select, storage } = fixture();
  await elements.get('btn-vol-spectrum').listeners.click();
  await new Promise(setImmediate);
  select.value = 'fx_1';
  select.listeners.change();
  assert.equal(storage.get('audiox.spectrumNode'), 'fx_1');
  assert.equal(vm.runInContext('JSON.stringify(spectrumView.targets)', context), '[]');
  await new Promise(setImmediate);
  assert.equal(context.urls.at(-1), '/api/audio/spectrum?id=fx_1');

  elements.get('btn-spectrum-close').listeners.click();
  await elements.get('btn-vol-spectrum').listeners.click();
  assert.equal(select.value, 'fx_1');
});
