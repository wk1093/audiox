const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

const pluginsSource = fs.readFileSync(require.resolve('../web/plugins.js'), 'utf8');
const effectsSource = fs.readFileSync(require.resolve('../web/effects.js'), 'utf8');
const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
const app = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');

class Element {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.listeners = {};
    this.attributes = {};
    this.style = {};
    this.value = '';
    this.parentNode = null;
  }
  set innerHTML(_) { for (const child of this.children) child.parentNode = null; this.children = []; }
  get innerHTML() { throw new Error('innerHTML must not be read or used for metadata'); }
  append(...children) { for (const child of children) this.appendChild(child); }
  appendChild(child) { child.parentNode = this; this.children.push(child); }
  removeChild(child) { this.children = this.children.filter(c => c !== child); child.parentNode = null; }
  remove() { this.parentNode?.removeChild(this); }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  removeEventListener(name, callback) { if (this.listeners[name] === callback) delete this.listeners[name]; }
  setAttribute(name, value) { this.attributes[name] = String(value); this[name] = value; }
  getAttribute(name) { return this.attributes[name]; }
  contains(target) { return this === target || this.children.some(child => child.contains?.(target)); }
}

function find(element, predicate) {
  if (predicate(element)) return element;
  for (const child of element.children) { const result = find(child, predicate); if (result) return result; }
}
function findAll(element, predicate, out = []) {
  if (predicate(element)) out.push(element);
  for (const child of element.children) findAll(child, predicate, out);
  return out;
}

function fixture({ plugins: pluginRows = [], effects: effectRows = [] } = {}) {
  const document = {
    activeElement: null,
    createElement: tag => {
      const el = new Element(tag);
      if (tag === 'iframe') el.contentWindow = { posted: [], postMessage(data, origin) { this.posted.push({ data, origin }); } };
      return el;
    },
  };
  const calls = [], notices = [], timers = new Map(), windowListeners = {};
  const state = { plugins: pluginRows, effects: effectRows, fail: null, confirm: true, confirms: [] };
  let timerId = 0;
  const ui = {
    formatMessage: text => text, resolve() {},
    confirm: async (message, options) => { state.confirms.push({ message, ...options }); return state.confirm; },
  };
  const window = {
    AudioxUI: ui,
    addEventListener(name, fn) { (windowListeners[name] ||= []).push(fn); },
    removeEventListener(name, fn) { windowListeners[name] = (windowListeners[name] || []).filter(f => f !== fn); },
  };
  const context = vm.createContext({
    document, window, AudioxUI: ui, URLSearchParams, TextEncoder, setInterval() {}, clearInterval() {},
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; },
    clearTimeout: id => timers.delete(id),
    fetch: async (url, options = {}) => {
      calls.push({ url, ...options });
      if (state.fail && url.startsWith(state.fail.url)) {
        return { ok: false, status: state.fail.status, text: async () => state.fail.text };
      }
      if (url === '/api/plugins') return { ok: true, text: async () => JSON.stringify({ plugins: state.plugins }) };
      if (url === '/api/effect/set') {
        const fields = new URLSearchParams(options.body);
        const value = Number(fields.get('value'));
        const saved = state.roundToFloat ? Number(Math.fround(value).toPrecision(9)) : value;
        state.effects = state.effects.map(fx => fx.id !== fields.get('id') ? fx
          : { ...fx, params: { ...fx.params, [fields.get('param')]: saved } });
        const updated = state.effects.find(fx => fx.id === fields.get('id'));
        return { ok: true, text: async () => JSON.stringify({ ok: true, params: updated?.params }) };
      }
      return { ok: true, text: async () => JSON.stringify({ effects: state.effects }) };
    },
  });
  vm.runInContext(pluginsSource, context);
  vm.runInContext(effectsSource, context);
  const effects = window.AudioxEffects.createManager((...args) => notices.push(args));
  const panel = new Element('div');
  effects.mountPanel(panel, new Element('p'));
  let changed = 0;
  const plugins = window.AudioxPlugins.createManager({
    setStatus: (...args) => notices.push(args),
    onCatalog: catalog => effects.setPluginCatalog(catalog),
    onChanged: async () => { changed++; await effects.loadEffects(); },
  });
  const ui2 = { listEl: new Element('div'), statusEl: new Element('div'),
    fileInput: new Element('input'), installButton: new Element('button') };
  ui2.fileInput.files = [];
  plugins.mount(ui2);
  return {
    effects, plugins, panel, ui: ui2, document, calls, notices, state, windowListeners,
    api: window.AudioxPlugins, changed: () => changed,
    dispatch: event => { for (const fn of windowListeners.message || []) fn(event); },
    flush: async () => { const pending = [...timers.values()]; timers.clear(); for (const fn of pending) await fn(); },
  };
}

const catalogEntry = {
  id: 'plugin_tremolo', name: 'Tremolo <b>', version: '1.2.0', ui: true,
  params: [{ name: 'level', label: 'Level', min: 0, max: 2, default: 1 }],
};
const pluginFx = {
  id: 'fx_3', type: 'plugin_tremolo', name: 'Wobble', custom_name: 'Wobble', enabled: true,
  params: { level: 1 }, param_meta: [{ name: 'level', label: 'Level', min: 0, max: 2, default: 1 }],
  plugin_ui: true, plugin_status: 'ready',
};

test('catalog lists metadata as text and rejects unsafe IDs', async () => {
  const f = fixture({ plugins: [catalogEntry, { id: '../x', name: 'bad' }, { id: 'gain', name: 'shadow' }] });
  const catalog = await f.plugins.load();
  assert.deepEqual([...catalog.map(p => p.id)], ['plugin_tremolo']);
  const row = find(f.ui.listEl, el => el.className === 'plugin-row');
  assert.equal(find(row, el => el.tagName === 'STRONG').textContent, 'Tremolo <b>');
  assert.match(row.children[0].children[1].textContent, /plugin_tremolo · version 1.2.0 · custom UI included/);
  assert.match(row.children[0].children[2].textContent, /Level \(0 to 2, default 1\)/);
  assert.equal(f.api.assetUrl('plugin_tremolo'), '/api/plugins/plugin_tremolo/assets/index.html');
  assert.equal(f.api.assetUrl('../x'), '');
  f.state.fail = { url: '/api/plugins', status: 503, text: 'registry offline' };
  await assert.rejects(f.plugins.load(), /Plugin list load failed: 503 registry offline/);
});

test('valid plugin parameter names matching JavaScript properties retain their labels and values', async () => {
  for (const id of ['plugin_constructor', 'plugin_default']) {
    const f = fixture({
      plugins: [{ ...catalogEntry, id }],
      effects: [{ ...pluginFx, type: id,
        params: { constructor: 1, default: 0.5 },
        param_meta: [
          { name: 'constructor', label: 'Custom amount', min: 0, max: 2, default: 1 },
          { name: 'default', label: 'Custom mix', min: 0, max: 1, default: 0.5 },
        ],
      }],
    });
    await f.plugins.load();
    await f.effects.loadEffects();
    assert.equal(f.effects.isPluginEffect(f.effects.effectsById.get('fx_3')), true);
    assert.match(f.effects.typeLabel(f.effects.effectsById.get('fx_3')), /Plugin: Tremolo/);
    const toggle = find(f.panel, el => el.textContent === 'Open custom UI');
    assert.ok(toggle && !toggle.disabled);
    toggle.listeners.click();
    assert.equal(f.effects.frames.get('fx_3').iframe.src, `/api/plugins/${id}/assets/index.html`);
    const parameters = f.effects.frameParameters(f.effects.effectsById.get('fx_3'));
    assert.equal(parameters.params.constructor, 1);
    assert.equal(parameters.params.default, 0.5);
    assert.ok(find(f.panel, el => el.tagName === 'LABEL' && el.textContent === 'Custom amount'));
  }
});

test('install validates, confirms native code risk, PUTs binary, then reloads catalog and effects', async () => {
  const f = fixture();
  await f.plugins.load();
  await assert.rejects(f.plugins.install({ name: 'x.zip', size: 10 }), /\.axm/);
  await assert.rejects(f.plugins.install({ name: 'x.axm', size: 12 * 1024 * 1024 + 1 }), /12 MiB/);
  const file = { name: 'tremolo.axm', size: 1024 };
  f.state.confirm = false;
  assert.equal(await f.plugins.install(file), false);
  assert.equal(f.calls.filter(c => c.url === '/api/plugins/install').length, 0);
  f.state.confirm = true;
  f.state.plugins = [catalogEntry];
  f.state.effects = [pluginFx];
  f.ui.fileInput.value = 'C:\\fakepath\\tremolo.axm';
  assert.equal(await f.plugins.install(file), true);
  assert.match(f.state.confirms.at(-1).message, /native code/);
  const put = f.calls.find(c => c.url === '/api/plugins/install');
  assert.equal(put.method, 'PUT');
  assert.equal(put.body, file);
  assert.equal(put.headers['Content-Type'], 'application/octet-stream');
  assert.equal(f.ui.fileInput.value, '');
  assert.equal(f.changed(), 1);
  assert.ok(f.calls.findLastIndex(c => c.url === '/api/plugins') > f.calls.indexOf(put));
  assert.equal(find(f.panel, el => el.className === 'fx-card-head').textContent, 'Wobble');
  assert.match(f.ui.statusEl.textContent, /Installed tremolo.axm/);

  f.ui.fileInput.value = 'kept.axm';
  f.state.fail = { url: '/api/plugins/install', status: 400, text: 'bad signature' };
  await assert.rejects(f.plugins.install(file), /Plugin install failed: 400 bad signature/);
  assert.equal(f.ui.fileInput.value, 'kept.axm');
  assert.equal(f.ui.installButton.disabled, false);
});

test('remove and reload confirm, post id=<id>, and surface failures', async () => {
  const f = fixture({ plugins: [catalogEntry] });
  await f.plugins.load();
  const row = find(f.ui.listEl, el => el.className === 'plugin-row');
  const [, reload, remove] = row.children;
  assert.equal(reload.textContent, 'Reload');
  await reload.listeners.click();
  const reloadCall = f.calls.find(c => c.url === '/api/plugins/reload');
  assert.equal(reloadCall.method, 'POST');
  assert.equal(reloadCall.body, 'id=plugin_tremolo');
  assert.match(f.state.confirms.at(-1).title, /Reload/);
  f.state.fail = { url: '/api/plugins/remove', status: 409, text: 'plugin in use' };
  await remove.listeners.click();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(f.calls.find(c => c.url === '/api/plugins/remove').body, 'id=plugin_tremolo');
  assert.match(f.ui.statusEl.textContent, /Plugin remove failed: 409 plugin in use/);
  f.state.confirm = false;
  const count = f.calls.length;
  assert.equal(await f.plugins.remove('plugin_tremolo'), false);
  assert.equal(f.calls.length, count);
});

test('plugin effects render explicit statuses and errors without raw HTML', async () => {
  const f = fixture({ plugins: [catalogEntry], effects: [
    { ...pluginFx, plugin_status: 'fault', plugin_error: '<img src=x onerror=alert(1)>' },
    { ...pluginFx, id: 'fx_4', plugin_status: 'unavailable', plugin_ui: false },
  ] });
  await f.plugins.load();
  await f.effects.loadEffects();
  const statuses = findAll(f.panel, el => String(el.className).startsWith('small fx-plugin-status'));
  assert.deepEqual(statuses.map(el => el.className.split(' ').at(-1)), ['fault', 'unavailable']);
  assert.match(statuses[0].textContent, /Plugin fault/);
  assert.match(statuses[1].textContent, /Plugin unavailable/);
  assert.equal(find(f.panel, el => el.className === 'small fx-plugin-error').textContent,
    'Plugin error: <img src=x onerror=alert(1)>');
  assert.equal(findAll(f.panel, el => el.textContent === 'Plugin: Tremolo <b> 1.2.0').length, 2);
  const toggle = find(f.panel, el => el.textContent === 'Open custom UI');
  assert.equal(toggle.disabled, true);
  assert.equal(findAll(f.panel, el => el.className === 'fx-plugin-ui').length, 1);
});

test('custom UI is opt-in, sandboxed, and bridges only validated parameter changes', async () => {
  const f = fixture({ plugins: [catalogEntry], effects: [pluginFx] });
  await f.plugins.load();
  await f.effects.loadEffects();
  assert.equal(find(f.panel, el => el.tagName === 'IFRAME'), undefined);
  assert.match(find(f.panel, el => el.className === 'small fx-plugin-warning').textContent, /malicious/);
  const toggle = find(f.panel, el => el.textContent === 'Open custom UI');
  toggle.listeners.click();
  const iframe = find(f.panel, el => el.tagName === 'IFRAME');
  assert.equal(iframe.getAttribute('sandbox'), 'allow-scripts');
  assert.equal(iframe.getAttribute('referrerpolicy'), 'no-referrer');
  assert.equal(iframe.src, '/api/plugins/plugin_tremolo/assets/index.html');
  assert.equal(toggle.textContent, 'Close custom UI');
  assert.equal(f.windowListeners.message.length, 1);

  iframe.listeners.load();
  const [initial] = iframe.contentWindow.posted;
  assert.equal(initial.origin, '*');
  assert.deepEqual(JSON.parse(JSON.stringify(initial.data)), { type: 'audiox:parameters', id: 'fx_3',
    params: { level: 1 }, param_meta: [{ name: 'level', label: 'Level', min: 0, max: 2, default: 1 }] });

  const source = iframe.contentWindow;
  const sets = () => f.calls.filter(c => c.url === '/api/effect/set');
  f.dispatch({ source: {}, origin: 'null', data: { type: 'audiox:set-param', param: 'level', value: 1.5 } });
  f.dispatch({ source, origin: 'http://evil', data: { type: 'audiox:set-param', param: 'level', value: 1.5 } });
  f.dispatch({ source, origin: 'null', data: { type: 'audiox:set-param', param: 'drive', value: 1 } });
  f.dispatch({ source, origin: 'null', data: { type: 'audiox:set-param', param: 'level', value: 3 } });
  f.dispatch({ source, origin: 'null', data: { type: 'audiox:set-param', param: 'level', value: '1.5' } });
  f.dispatch({ source, origin: 'null', data: { type: 'audiox:set-param', param: 'level', value: NaN } });
  f.dispatch({ source, origin: 'null', data: 'audiox:set-param' });
  await f.flush();
  assert.equal(sets().length, 0);
  f.dispatch({ source, origin: 'null', data: { type: 'audiox:set-param', id: 'fx_99', param: 'level', value: 1.5 } });
  await f.flush();
  assert.equal(sets().length, 1);
  const fields = new URLSearchParams(sets()[0].body);
  assert.equal(fields.get('id'), 'fx_3');
  assert.equal(fields.get('value'), '1.5');
  assert.equal(find(f.panel, el => el.className === 'fx-parameter').children[1].children[1].value, '1.5');
  assert.equal(iframe.contentWindow.posted.at(-1).data.params.level, 1.5);

  // A background poll after our own save must not rebuild the panel and reload the frame.
  await f.effects.loadEffects();
  assert.equal(find(f.panel, el => el.tagName === 'IFRAME'), iframe);

  // Re-rendering replaces the frame without duplicating listeners.
  f.state.effects = [{ ...pluginFx, name: 'Renamed', params: { level: 1.5 } }];
  await f.effects.loadEffects();
  const replaced = findAll(f.panel, el => el.tagName === 'IFRAME');
  assert.equal(replaced.length, 1);
  assert.notEqual(replaced[0], iframe);
  assert.equal(iframe.parentNode, null);
  assert.equal(f.windowListeners.message.length, 1);
  f.dispatch({ source, origin: 'null', data: { type: 'audiox:set-param', param: 'level', value: 0.5 } });
  await f.flush();
  assert.equal(sets().length, 1);

  f.effects.closeCustomUis();
  assert.equal(findAll(f.panel, el => el.tagName === 'IFRAME').length, 0);
  assert.equal(f.windowListeners.message.length, 0);
  assert.equal(find(f.panel, el => el.textContent === 'Open custom UI').getAttribute('aria-expanded'), 'false');

  find(f.panel, el => el.textContent === 'Open custom UI').listeners.click();
  f.state.effects = [{ ...pluginFx, plugin_status: 'fault', params: { level: 1.5 } }];
  await f.effects.loadEffects();
  assert.equal(findAll(f.panel, el => el.tagName === 'IFRAME').length, 0);
  assert.equal(f.windowListeners.message.length, 0);
});

test('plugin parameters retain small values in updates and refresh signatures', async () => {
  const fx = { ...pluginFx, params: { amount: 0.00001 },
    param_meta: [{ name: 'amount', label: 'Amount', min: 0.000001, max: 0.0001, default: 0.00001 }] };
  const f = fixture({ plugins: [catalogEntry], effects: [fx] });
  await f.plugins.load();
  await f.effects.loadEffects();
  find(f.panel, el => el.textContent === 'Open custom UI').listeners.click();
  const frame = f.effects.frames.get('fx_3').iframe;
  f.state.roundToFloat = true;
  const before = f.effects.buildEffectsSignature();
  await f.effects.setParam('fx_3', 'amount', 0.00002);
  const call = f.calls.find(c => c.url === '/api/effect/set');
  assert.equal(Number(new URLSearchParams(call.body).get('value')), 0.00002);
  assert.notEqual(f.effects.buildEffectsSignature(), before);
  assert.equal(f.effects.effectsById.get('fx_3').params.amount, Number(Math.fround(0.00002).toPrecision(9)));
  await f.effects.loadEffects();
  assert.equal(f.effects.frames.get('fx_3').iframe, frame, 'canonical float readback does not reload custom UI');
  f.state.effects = [{ ...fx, params: { amount: 0.00003 } }];
  await f.effects.loadEffects();
  assert.equal(f.effects.effectsById.get('fx_3').params.amount, 0.00003);
});

test('Effects page hosts the plugin manager and routing offers installed plugin templates', () => {
  const effectsPage = html.split('data-page="effects"')[1].split('<!-- Volume page -->')[0];
  assert.match(effectsPage, /id="plugin-file" type="file" accept="\.axm"/);
  assert.match(effectsPage, /id="btn-plugin-install"/);
  assert.match(effectsPage, /id="plugin-list"/);
  assert.match(effectsPage, /native code/);
  assert.ok(html.indexOf('/plugins.js') < html.indexOf('/app.js'));
  assert.match(app, /registerNodeType\(type, AudioxPluginEffectNode\)/);
  assert.match(app, /isCreatableEffectType\(templateType\)/);
  assert.match(app, /closeCustomUis\(\)/);
  assert.doesNotMatch(effectsSource + pluginsSource, /allow-same-origin|allow-forms|allow-popups|allow-top-navigation/);
});
