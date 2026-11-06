const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const history = require('../web/routing-history.js');
const monitor = require('../web/system-monitor.js');

test('wire undo/redo preserves copies, saved baseline, and branches after undo', () => {
  let applied, status;
  const edits = history.create({ apply: (value) => { applied = value; }, changed: (value) => { status = value; } });
  const a = { src: 'a', dst: 'b', srcChannel: 0, dstChannel: 0 };
  const b = { ...a, srcChannel: 1 };
  edits.reset([]);
  edits.record([a]);
  edits.saved([a]);
  edits.record([a, b]);
  assert.equal(status.dirty, true);
  assert.equal(edits.undo(), true);
  assert.deepEqual(applied, [a]);
  assert.equal(status.dirty, false);
  applied[0].src = 'mutated';
  assert.equal(edits.undo(), true);
  assert.equal(status.dirty, true);
  assert.equal(edits.redo(), true);
  assert.deepEqual(applied, [a]);
  edits.record([b]);
  assert.equal(edits.redo(), false);
});

test('wire history is bounded and order changes do not create edits', () => {
  const edits = history.create({ apply: () => {}, changed: () => {}, limit: 3 });
  for (let n = 0; n < 10; n++) edits.record([{ src: String(n), dst: 'out', srcChannel: 0, dstChannel: 0 }]);
  assert.equal(edits.undo(), true);
  assert.equal(edits.undo(), true);
  assert.equal(edits.undo(), false);
});

test('device history merges overlapping batches, remains bounded and resets on runtime changes', () => {
  const history = monitor.createHistory(3);
  const sample = (id) => ({ id, now: id * 1000, temperature: 55, cpu: 50, xruns: 1 });
  const data = (ids, runtime_id = '1') => ({ runtime_id, samples: ids.map(sample) });
  assert.equal(history.merge(data([1, 2])).length, 2);
  assert.equal(history.merge(data([2, 3])).length, 3);
  assert.deepEqual(history.merge(data([3, 4])).map((sample) => sample.id), [2, 3, 4]);
  assert.equal(history.lastId(), 4);
  assert.equal(history.merge(data([1], '2')).length, 1);
  assert.throws(() => history.merge({ runtime_id: '2', samples: [{ ...sample(2), cpu: 'bad' }] }), /Invalid/);
  const svg = monitor.historySvg(history.merge(data([2], '2')));
  assert.match(svg, /1 X-runs/);
  assert.doesNotMatch(svg, /NaN|undefined/);
  assert.doesNotMatch(svg, /while System is open/);
  assert.throws(() => history.merge(data([3, 2], '2')), /Invalid/);
});

test('browser history expires at ten minutes even with sparse or empty updates and fixes the time axis', () => {
  const history = monitor.createHistory();
  const sample = (id, now) => ({ id, now, temperature: 55, cpu: 50, xruns: 1 });
  const data = (samples, now_ms) => ({ runtime_id: '1', samples, now_ms });
  const samples = Array.from({ length: 1000 }, (_, i) => sample(i + 1, (i + 1) * 1000));
  const retained = history.merge(data(samples, 1000000));
  assert.equal(retained.length, 600);
  assert.equal(retained[0].now, 401000);
  assert.match(monitor.historySvg(retained, 1000000), /10 minutes ago/);
  assert.match(monitor.historySvg([sample(1, 1000)], 1000), /10 minutes ago/);
  assert.equal(history.merge(data([], 1599999)).length, 1);
  assert.equal(history.merge(data([], 1600000)).length, 0);
  const sparse = history.merge(data([sample(1001, 1700000), sample(1002, 2299999)], 2299999));
  assert.equal(sparse.length, 2);
  assert.equal(history.merge(data([sample(1003, 2300000)], 2300000)).length, 2);
  assert.equal(history.lastId(), 1003);
  assert.equal(monitor.historySvg(sparse, 2900000), '');
  const svg = monitor.historySvg([sample(1004, 3000000)], 3100000);
  assert.match(svg, /10 minutes ago/);
  assert.doesNotMatch(svg, /M755\.0 /);
});

test('collapsed health tools defer requests; opening fills device history, then polls small batches', async () => {
  const nodes = new Map();
  const node = (id) => {
    if (!nodes.has(id)) nodes.set(id, { open: false, hidden: false, dataset: {}, listeners: {},
      addEventListener(type, fn) { this.listeners[type] = fn; }, replaceChildren() {} });
    return nodes.get(id);
  };
  const calls = [], notices = [];
  let last = 1000, runtime = '1';
  const context = { document: { getElementById: node }, AbortSignal, Map, URL, Blob,
    AudioxUI: { notify: (...args) => notices.push(args), resolve() {} },
    fetch: async (url) => {
      calls.push(url);
      const limit = Number(new URL(url, 'http://device').searchParams.get('limit'));
      const samples = Array.from({ length: Math.min(last, limit) }, (_, i) => {
        const id = Math.max(1, last - limit + 1) + i;
        return { id, now: id * 1000, temperature: 55, cpu: 40, xruns: 0 };
      });
      return { ok: true, json: async () => url.includes('/history') ? { runtime_id: runtime, now_ms: last * 1000, samples }
        : { detected: { devices: [] }, streams: [], stale: false } };
    },
  };
  context.window = context;
  vm.runInNewContext(fs.readFileSync(require.resolve('../web/system-monitor.js'), 'utf8'), context);
  const flush = () => new Promise(setImmediate);
  context.AudioxSystemMonitor.update({});
  await flush();
  assert.deepEqual(calls, []);
  node('system-advanced').open = true;
  node('system-advanced').listeners.toggle();
  await flush();
  assert.equal(calls.filter((url) => url.includes('/history')).at(-1), '/api/system/history?limit=600');
  assert.equal(node('system-history-status').hidden, true);
  last = 1003;
  context.AudioxSystemMonitor.update({});
  await flush();
  assert.equal(calls.filter((url) => url.includes('/history')).at(-1), '/api/system/history?limit=50');
  last = 1200;
  context.AudioxSystemMonitor.update({});
  await flush();
  assert.deepEqual(calls.filter((url) => url.includes('/history')).slice(-2),
    ['/api/system/history?limit=50', '/api/system/history?limit=600']);
  runtime = '2'; last = 3;
  context.AudioxSystemMonitor.update({});
  await flush();
  assert.doesNotMatch(node('system-history').innerHTML, /NaN|undefined/);
  assert.deepEqual(notices, []);
});

test('scenes and advanced tools are collapsed and scenes are hidden in expanded graphs', () => {
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  const css = fs.readFileSync(require.resolve('../web/scenes.css'), 'utf8');
  assert.match(html, /<details id="scenes-menu" class="scenes-menu">/);
  assert.match(html, /<details id="system-advanced" class="card system-advanced">/);
  assert.match(css, /\.graph-shell\.expanded \.scenes-menu.*display: none/);
  assert.doesNotMatch(html, /Firmware throttling flags do not identify|Collecting readings while/);
});

test('audio health remembers disconnected devices without presenting old streams as active', () => {
  const remembered = new Map();
  const stream = { id: 'alsa_usb_dev0_in', active: true, rate: 48000, mmap: true };
  monitor.deviceRows({ detected: { devices: [{ stableCardId: 'usb', device: 0, name: '<script>' }] }, streams: [stream] }, remembered);
  assert.equal(remembered.get('usb_dev0').connected, true);
  monitor.deviceRows({ detected: { devices: [] }, streams: [] }, remembered);
  assert.equal(remembered.get('usb_dev0').connected, false);
  assert.throws(() => monitor.deviceRows({}), /Invalid/);
});

test('offline assets use pinned packages, local scripts/styles/fonts and retain licenses', () => {
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  const css = fs.readFileSync(require.resolve('../web/styles.css'), 'utf8');
  const stage = fs.readFileSync(require.resolve('../web/stage-assets.js'), 'utf8');
  assert.doesNotMatch(html, /<(script|link)[^>]+(?:src|href)="https:\/\/(?:cdn|fonts)/);
  assert.match(html, /src="\/vendor\/litegraph.js"/);
  assert.match(css, /url\('\/vendor\/space-grotesk-400.woff2'\)/);
  assert.match(stage, /litegraph-LICENSE/);
  assert.match(stage, /space-grotesk-LICENSE/);
});

test('power health distinguishes active, historical, and unavailable telemetry', () => {
  assert.equal(monitor.flagSummary({ status: 'available', current: true, since_boot: true }), 'Active now');
  assert.match(monitor.flagSummary({ status: 'available', current: false, since_boot: true }), /inactive/);
  assert.match(monitor.flagSummary({ status: 'unsupported', current: null }), /unsupported/);
  assert.match(monitor.flagSummary(undefined), /unavailable/);
});

test('fan feedback does not report intentionally stopped fans as stalled and identifies unapplied curves', () => {
  const fan = { driver_status: 'available', rpm_status: 'available', rpm: 0,
    cooling_status: 'available', cooling_state: 0, cooling_max_state: 4,
    cooling_requested: false, stall_status: 'available', stall_warning: false,
    saved_matches_active: false };
  assert.match(monitor.fanSummary(fan), /intentionally stopped/);
  assert.doesNotMatch(monitor.fanSummary(fan), /WARNING/);
  assert.match(monitor.fanSummary(fan), /restart to apply/);
  assert.match(monitor.fanSummary({ ...fan, cooling_requested: true, stall_warning: true }), /WARNING/);
  assert.match(monitor.fanSummary({ driver_status: 'missing' }), /driver missing/);
  assert.match(monitor.fanSummary({ module_status: 'available', device_status: 'missing' }), /module loaded; fan device missing/);
  const unmanaged = monitor.fanSummary({ ...fan, saved_matches_active: null, saved_curve: { status: 'missing' } });
  assert.match(unmanaged, /No custom curve/);
  assert.doesNotMatch(unmanaged, /firmware defaults/);
  assert.match(monitor.curveSummary({ status: 'available', temperatures_c: [50, 60, 67.5, 75],
    speeds_pwm: [75, 125, 175, 250], hysteresis_c: 5 }), /67.5 °C/);
});

test('active routing refresh retains drafts on failed reads and resets history only after success', async () => {
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const draft = [{ src: 'draft', dst: 'output', srcChannel: 0, dstChannel: 0 }];
  const active = [{ ...draft[0], src: 'active' }];
  const state = { edges: draft, dormantEdges: [], sceneRoutingConflict: true };
  let reset = null, rebuilt = 0;
  const controls = new Map(['scene-routing-conflict', 'btn-route-draft'].map((id) => [id, { hidden: false }]));
  const context = vm.createContext({
    state, routingPath: '/routing', AudioxUI: { hasUnsavedChanges: () => false },
    fetch: async () => ({ ok: false, status: 503 }),
    parseRoutingText: () => active,
    loadRoutingThingsWithOptions: async () => true,
    rebuildRoutingGraph: () => { rebuilt++; },
    routingHistory: { reset: (edges) => { reset = edges; } },
    effectsManager: { loadEffects: async () => {} }, loadVolumes: async () => {},
    document: { getElementById: (id) => controls.get(id) },
  });
  vm.runInContext(source.slice(source.indexOf('async function loadActiveRouting()'),
    source.indexOf("window.addEventListener('audiox-scene-recalled'")), context);
  await assert.rejects(context.loadActiveRouting(), /Draft retained/);
  assert.equal(state.edges, draft);
  assert.equal(rebuilt, 0);
  assert.equal(reset, null);
  context.fetch = async () => ({ ok: true, text: async () => 'active routing' });
  await context.loadActiveRouting();
  assert.deepEqual(state.edges, active);
  assert.deepEqual(JSON.parse(JSON.stringify(reset)), active);
  assert.equal(state.sceneRoutingConflict, false);
  assert.equal(controls.get('scene-routing-conflict').hidden, true);
});

test('external scene conflicts pause graph polling and block stale routing saves', async () => {
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const notices = [];
  const context = vm.createContext({ state: { sceneRoutingConflict: true },
    setStatus: (...args) => notices.push(args) });
  vm.runInContext(source.slice(source.indexOf('async function pollRoutingThingsIfNeeded()'),
    source.indexOf('function startRoutingThingsPolling()')), context);
  vm.runInContext(source.slice(source.indexOf('async function saveRouting()'),
    source.indexOf('async function loadVersion(')), context);
  await context.pollRoutingThingsIfNeeded();
  assert.equal(await context.saveRouting(), false);
  assert.match(notices[0][0], /live setup/);
});
