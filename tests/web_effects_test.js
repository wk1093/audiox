const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require.resolve('../web/effects.js'), 'utf8');

function fixture() {
  class Element {
    constructor(tag) {
      this.tagName = tag.toUpperCase();
      this.children = [];
      this.listeners = {};
      this.style = {};
      this.value = '';
    }
    set innerHTML(_) { this.children = []; }
    append(...children) { this.children.push(...children); }
    appendChild(child) { this.children.push(child); }
    addEventListener(name, callback) { this.listeners[name] = callback; }
    setAttribute(name, value) { this[name] = value; }
    contains(target) { return this === target || this.children.some(child => child.contains?.(target)); }
  }
  const document = { createElement: tag => new Element(tag), activeElement: null };
  const notices = [], calls = [], timers = new Map();
  let effects = [], failure = false, timerId = 0;
  const ui = { formatMessage: text => text, resolve() {}, confirm: async () => true };
  const context = vm.createContext({ document, window: { AudioxUI: ui }, AudioxUI: ui,
    URLSearchParams, TextEncoder, setInterval() {}, clearInterval() {},
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; },
    clearTimeout: id => timers.delete(id),
    fetch: async (url, options) => {
      calls.push({ url, ...options });
      if (failure) return { ok: false, status: 500, text: async () => 'disk failure' };
      if (url === '/api/effect/name') {
        const fields = new URLSearchParams(options.body);
        const name = Buffer.from(fields.get('name_hex'), 'hex').toString('utf8');
        effects = effects.map(fx => fx.id === fields.get('id')
          ? { ...fx, custom_name: name, name: name || `Gain ${fx.id.slice(3)}` } : fx);
      }
      if (url === '/api/effect/set') {
        const fields = new URLSearchParams(options.body);
        effects = effects.map(fx => fx.id !== fields.get('id') ? fx : {
          ...fx, enabled: fields.has('enabled') ? fields.get('enabled') === '1' : fx.enabled,
          params: fields.has('param') ? { ...fx.params, [fields.get('param')]: Number(fields.get('value')) } : fx.params,
        });
      }
      return { ok: true, text: async () => JSON.stringify({ effects }) };
    },
  });
  vm.runInContext(source, context);
  const manager = context.window.AudioxEffects.createManager((...args) => notices.push(args));
  const panel = new Element('div');
  manager.mountPanel(panel, new Element('p'));
  return { manager, panel, document, calls, notices, api: context.window.AudioxEffects,
    setEffects: value => { effects = value; }, fail: () => { failure = true; },
    flush: async () => { const pending = [...timers.values()]; timers.clear(); for (const fn of pending) await fn(); } };
}

function find(element, predicate) {
  if (predicate(element)) return element;
  for (const child of element.children) { const result = find(child, predicate); if (result) return result; }
}

const gain = {
  id: 'fx_7', type: 'gain', name: 'Mic level', custom_name: 'Mic level', enabled: true,
  params: { level: 1 }, param_meta: [{ name: 'level', label: 'Level', min: 0, max: 4, default: 1 }],
};

test('pitch quality uses a named dropdown, supports reset and synchronizes MIDI changes', async () => {
  const f = fixture();
  const pitch = { ...gain, type: 'pitch', params: { quality: 0 },
    param_meta: [{ name: 'quality', label: 'Quality', min: 0, max: 2, default: 0 }] };
  f.setEffects([pitch]);
  await f.manager.loadEffects();
  const control = find(f.panel, el => el.className === 'fx-parameter');
  const select = find(control, el => el.tagName === 'SELECT');
  assert.deepEqual(select.children.map(el => el.textContent), ['High', 'Balanced', 'Economy']);
  assert.equal(find(control, el => el.type === 'range'), undefined);
  select.value = '1';
  select.listeners.change();
  await f.flush();
  let fields = new URLSearchParams(f.calls.find(call => call.url === '/api/effect/set').body);
  assert.equal(fields.get('param'), 'quality');
  assert.equal(fields.get('value'), '1');
  f.manager.syncParamView('fx_7', 'quality', 2);
  assert.equal(select.value, '2');
  assert.equal(find(f.panel, el => el.className === 'fx-map-meta' && el.textContent === 'Quality: Economy').textContent, 'Quality: Economy');
  find(control, el => el.textContent === 'Reset').listeners.click();
  await f.flush();
  fields = new URLSearchParams(f.calls.filter(call => call.url === '/api/effect/set').at(-1).body);
  assert.equal(fields.get('value'), '0');
  const node = { properties: { thingId: 'fx_7' }, addWidget(...args) { this.widgets.push(args); } };
  f.manager.applyWidgets(node, pitch);
  const combo = node.widgets.find(widget => widget[0] === 'combo');
  assert.equal(combo[2], 'High');
  combo[3]('Economy');
  await f.flush();
  fields = new URLSearchParams(f.calls.filter(call => call.url === '/api/effect/set').at(-1).body);
  assert.equal(fields.get('value'), '2');
  f.document.activeElement = select;
  const before = f.panel.children[0];
  await f.manager.loadEffects();
  assert.equal(f.panel.children[0], before);
});
test('advanced builtins have names, dynamic sliders and type-specific help, not plugin UI', async () => {
  const names = { compressor: 'Compressor', limiter: 'Limiter', eq: 'Parametric EQ',
    multiband: 'Multiband Compressor', noise_reduction: 'Noise Reduction' };
  for (const [type, name] of Object.entries(names)) {
    const f = fixture();
    assert.equal(f.api.effectName({ id: 'fx_9', type }), `${name} 9`);
    const param = type === 'compressor' ? 'threshold_db' : type === 'limiter' ? 'ceiling_db' :
      type === 'eq' ? 'mid_gain_db' : type === 'multiband' ? 'low_threshold_db' : 'reduction_db';
    const fx = { ...gain, type, name: `${name} 9`, params: { [param]: 0 },
      param_meta: [{ name: param, label: 'Test parameter', min: -60, max: 30, default: 0 }] };
    const [meta] = f.api.dynamicParamList(fx);
    assert.ok(meta.description.length > 20);
    if (type === 'compressor') {
      assert.match(meta.description, /above.*compressed/);
      assert.doesNotMatch(meta.description, /gate/);
    }
    f.setEffects([fx]);
    await f.manager.loadEffects();
    assert.ok(find(f.panel, el => el.className === 'fx-parameter'));
    assert.equal(find(f.panel, el => el.textContent === 'Open custom UI'), undefined);
  }
});
test('effects use saved or readable names, never internal IDs in their graph titles', async () => {
  const f = fixture();
  assert.equal(f.api.effectName({ id: 'fx_3', type: 'gate' }), 'Noise Gate 3');
  assert.equal(f.api.effectName({ id: 'fx_4', type: 'cut' }), 'Mute 4');
  f.setEffects([gain]);
  await f.manager.loadEffects();
  const node = { properties: { thingId: gain.id }, addWidget() {} };
  f.manager.registerNode(node);
  assert.equal(node.title, 'Mic level');
  assert.equal(find(f.panel, el => el.className === 'fx-card-head').textContent, 'Mic level');
  const advanced = find(f.panel, el => el.tagName === 'DETAILS' && el.children[0]?.textContent === 'Advanced and delete');
  assert.match(advanced.children[1].textContent, /fx_7/);
  const midi = find(f.panel, el => el.className === 'fx-midi-details');
  assert.equal(midi.open, false);
});

test('rename saves UTF-8 safely, updates graph, and preserves routing IDs', async () => {
  const f = fixture();
  f.setEffects([gain]);
  await f.manager.loadEffects();
  const node = { properties: { thingId: gain.id }, addWidget() {} };
  f.manager.registerNode(node);
  await f.manager.setName(gain.id, 'Voice "café" & +');
  const fields = new URLSearchParams(f.calls.find(call => call.url === '/api/effect/name').body);
  assert.equal(fields.get('id'), gain.id);
  assert.equal(Buffer.from(fields.get('name_hex'), 'hex').toString('utf8'), 'Voice "café" & +');
  assert.equal(node.title, 'Voice "café" & +');
  await f.manager.setName(gain.id, '');
  assert.equal(node.title, 'Gain 7');
  const count = f.calls.length;
  await assert.rejects(f.manager.setName(gain.id, 'é'.repeat(32)), /63 UTF-8 bytes/);
  await assert.rejects(f.manager.setName(gain.id, 'bad\nname'), /control characters/);
  assert.equal(f.calls.length, count);
  f.fail();
  await assert.rejects(f.manager.setName(gain.id, 'unsaved'), /500.*disk failure/);
  assert.equal(node.title, 'Gain 7');
  assert.equal(f.manager.mutationInFlight, 0);
});

test('parameter controls pair sliders and numeric fields with help, units and reset', async () => {
  const f = fixture();
  f.setEffects([gain]);
  await f.manager.loadEffects();
  const control = find(f.panel, el => el.className === 'fx-parameter');
  assert.match(control.children[0].children[0].textContent, /Level \(x\)/);
  const info = control.children[0].children[1];
  assert.equal(info.tagName, 'DETAILS');
  assert.equal(info.open, undefined);
  assert.equal(info.children[0]['aria-label'], 'Help for Level');
  assert.match(info.children[0].title, /1 is unchanged/);
  assert.match(info.children[1].textContent, /1 is unchanged/);
  const [slider, number, reset] = control.children[1].children;
  assert.equal(number['aria-describedby'], info.children[1].id);
  number.value = '2.5';
  number.listeners.change();
  assert.equal(slider.value, '2.5');
  await f.flush();
  assert.equal(new URLSearchParams(f.calls.at(-1).body).get('value'), '2.5');
  assert.match(f.notices.at(-1)[0], /Mic level: Level saved/);
  number.value = '5';
  number.listeners.change();
  await f.flush();
  assert.match(f.notices.at(-1)[0], /between 0 and 4/);
  reset.listeners.click();
  await f.flush();
  assert.equal(number.value, '1');
  assert.equal(new URLSearchParams(f.calls.at(-1).body).get('value'), '1');
});

test('parameter help closes on Escape and effect disclosures survive rerendering', async () => {
  const f = fixture();
  f.setEffects([gain]);
  await f.manager.loadEffects();
  const info = find(f.panel, el => el.className === 'fx-param-info');
  let focused = false, stopped = false;
  info.children[0].focus = () => { focused = true; };
  info.open = true;
  info.listeners.keydown({ key: 'Escape', stopPropagation: () => { stopped = true; } });
  assert.equal(info.open, false);
  assert.equal(focused, true);
  assert.equal(stopped, true);
  const midi = find(f.panel, el => el.className === 'fx-midi-details');
  midi.open = true;
  midi.listeners.toggle();
  f.manager.renderPanel();
  assert.equal(find(f.panel, el => el.className === 'fx-midi-details').open, true);
  const reopened = find(f.panel, el => el.className === 'fx-midi-details');
  reopened.open = false;
  reopened.listeners.toggle();
  f.manager.renderPanel();
  assert.equal(find(f.panel, el => el.className === 'fx-midi-details').open, false);
});

test('noise floor keeps four decimals, channels stay integral, and failures are visible', async () => {
  const f = fixture();
  const pitch = { ...gain, type: 'pitch', params: { noise_floor: 0.001 },
    param_meta: [{ name: 'noise_floor', label: 'Noise Floor', min: 0.0001, max: 0.02, default: 0.001 }] };
  const [meta] = f.api.dynamicParamList(pitch);
  assert.equal(meta.precision, 4);
  assert.equal(meta.step, 0.0001);
  assert.ok(meta.description.length > 20);
  f.setEffects([pitch]);
  await f.manager.loadEffects();
  await f.manager.setParam(gain.id, 'noise_floor', 0.0011);
  assert.equal(new URLSearchParams(f.calls.at(-1).body).get('value'), '0.0011');
  f.fail();
  f.manager.queueParamUpdate(gain.id, 'noise_floor', 0.0012);
  await f.flush();
  assert.match(f.notices.at(-1)[0], /500.*disk failure/);
  const channels = f.api.dynamicParamList({ type: 'cut', params: { channels: 2 },
    param_meta: [{ name: 'channels', min: 1, max: 16, default: 2 }] })[0];
  assert.equal(channels.step, 1);
  assert.equal(channels.precision, 0);
});

test('background refresh does not destroy a focused name or parameter draft', async () => {
  const f = fixture();
  f.setEffects([gain]);
  await f.manager.loadEffects();
  const name = find(f.panel, el => el.type === 'text');
  name.value = 'Draft';
  name.listeners.input();
  f.document.activeElement = name;
  const count = f.calls.length;
  f.setEffects([{ ...gain, name: 'Remote' }]);
  await f.manager.loadEffects();
  assert.equal(f.calls.length, count);
  assert.equal(name.value, 'Draft');
  f.document.activeElement = null;
  await f.manager.loadEffects();
  assert.equal(find(f.panel, el => el.className === 'fx-card-head').textContent, 'Remote');
  assert.equal(find(f.panel, el => el.type === 'text').value, 'Draft');
});

test('failed enable leaves the displayed state unchanged', async () => {
  const f = fixture();
  f.setEffects([gain]);
  await f.manager.loadEffects();
  f.fail();
  await assert.rejects(f.manager.setEnabled(gain.id, false), /500/);
  assert.equal(f.manager.effectsById.get(gain.id).enabled, true);
});
