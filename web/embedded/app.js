const graphWrapEl = document.getElementById('graph-wrap');
const graphCanvasEl = document.getElementById('graph-canvas');
const zoomLabelEl = document.getElementById('zoom-label');
const tabsEl = document.getElementById('tabs');
const appVersionEl = document.getElementById('app-version');
const sbGridEl = document.getElementById('sb-grid');
const sbMappingStatusEl = document.getElementById('sb-mapping-status');
const sbStopAllMidiEl = document.getElementById('sb-stop-all-midi');
const sbSamplerToggleMidiEl = document.getElementById('sb-sampler-toggle-midi');
const midiActionCaptureStatusEl = document.getElementById('midi-action-capture-status');
const volGridEl = document.getElementById('vol-grid');
const volCcStatusEl = document.getElementById('vol-cc-status');
const effectsMapGridEl = document.getElementById('effects-map-grid');
const effectsMapStatusEl = document.getElementById('effects-map-status');
const logsSourceEl = document.getElementById('logs-source');
const logsLevelEl = document.getElementById('logs-level');
const logsTextEl = document.getElementById('logs-text');
const logsLimitEl = document.getElementById('logs-limit');
const logsSummaryEl = document.getElementById('logs-summary');
const logsViewEl = document.getElementById('logs-view');
const bluetoothStatusEl = document.getElementById('bluetooth-status');
const bluetoothDevicesEl = document.getElementById('bluetooth-devices');

const configPath = '/api/rootfs/config/config.staging.txt';
const realConfigPath = '/api/rootfs/config/config.txt';
const routingPath = '/api/rootfs/config/routing.txt';
const defaultFanCurve = {
  enabled: true,
  temperatures: [50, 60, 68, 75],
  speeds: [75, 125, 175, 250],
  hysteresis: 5,
};

const ROUTING_ADVANCED_KEY = 'audiox-routing-advanced';

function readRoutingAdvanced() {
  try { return window.localStorage?.getItem(ROUTING_ADVANCED_KEY) === '1'; } catch (_) { return false; }
}

const state = {
  nodes: [],
  edges: [],
  // Routes to Bluetooth devices that are not currently paired. They stay in
  // routing.txt untouched so the routing comes back when the device returns.
  dormantEdges: [],
  // Simple routing mode collapses 2-channel sockets into stereo wires; advanced shows every channel.
  advancedRouting: readRoutingAdvanced(),
  // Browser-only split/merge nodes: { id, kind: 'split'|'merge', thing: bound thing ID or null }.
  virtuals: [],
  virtualSeq: 0,
  virtualNodeById: new Map(),
  routingView: null,
  positions: {},
  selectedEdgeIdx: -1,
  autoArrange: true,
  soundboardEditing: false,
  soundboardOrderSupported: false,
  soundboardOrderSaving: false,
  edgeDeleteEnabled: true,
  activeTab: 'routing',
  mappingPollTimer: null,
  mappingLastSeq: 0,
  mappingAssignTarget: null,
  midiMappingsByNote: {},
  midiMappingsBySfx: {},
  sfxFiles: [],
  graph: null,
  graphCanvas: null,
  canvasResolution: null,
  nodeByThingId: new Map(),
  suspendGraphSync: false,
  internalGraphMutation: false,
  thingsPollTimer: null,
  thingsPollInFlight: false,
  thingsSignature: '',
  effectsPollInFlight: false,
  soundboardModesBySfx: {},
  soundboardLayout: { mode: 'normal', rows: 4, columns: 4, corners: [null, null, null, null], cells: [] },
  soundboardCellsBySfx: {},
  soundboardGridSaving: false,
  soundboardOpenSettings: new Set(),
  framebufferView: 'meters',
  stopAllMidiNote: null,
  samplerToggleMidiNote: null,
  passthroughToggleMidiNote: null,
  passthroughToggleMidiChannel: null,
  samplerKeyboardEnabled: true,
  samplerKeyboardChannel: 0,
  samplerRootNote: 60,
  // Volume page
  volumes: [],         // [{id, name, gain}] from /api/audio/volumes
  ccVolumes: [],       // [{cc, id}] from /api/midi/cc_volumes
  volAssignTarget: null,
  volCcPollTimer: null,
  volCcLastSeq: 0,
  volShowEffects: false,
  systemPollTimer: null,
  systemPollInFlight: false,
  pi5FanSettingsLoaded: false,
  pi5FanSettingsLoading: false,
  pi5FanIsPi5: false,
  pi5FanEditor: {
    enabled: defaultFanCurve.enabled,
    temperatures: [...defaultFanCurve.temperatures],
    speeds: [...defaultFanCurve.speeds],
    hysteresis: defaultFanCurve.hysteresis,
  },
  cpuTimesSample: null,
  sceneRoutingConflict: false,
};

const routingHistory = AudioxRoutingHistory.create({
  apply(edges) {
    state.edges = edges;
    state.dormantEdges = [];
    state.selectedEdgeIdx = -1;
    rebuildRoutingGraph();
    const restored = [...state.edges, ...state.dormantEdges];
    if (restored.length !== edges.length) {
      routingHistory.record(restored);
      setStatus('Some restored wires refer to removed nodes or channels and could not be restored. Review routing before saving.', 'warn');
    } else {
      setStatus('Wire edit restored; Save Routing to apply it.', 'ok');
    }
  },
  changed({ undo, redo, dirty }) {
    document.getElementById('btn-route-undo').disabled = !undo;
    document.getElementById('btn-route-redo').disabled = !redo;
    document.getElementById('routing-dirty').textContent = dirty ? 'Unsaved wire edits' : 'Routing saved';
    document.querySelector('.tab-btn[data-tab="routing"]')?.toggleAttribute('data-unsaved', dirty);
    AudioxUI.setUnsaved('routing', dirty);
  },
});

function recordRoutingHistory() {
  routingHistory.record([...state.edges, ...state.dormantEdges]);
}

const soundboardOrder = AudioxSoundboardOrder.attach(sbGridEl, {
  enabled: () => state.soundboardEditing && state.soundboardLayout.mode !== 'grid' &&
    state.soundboardOrderSupported &&
    !state.soundboardOrderSaving && !AudioxUI.isBusy(),
  save: saveSoundboardOrder,
  status: document.getElementById('sb-order-status'),
});

const soundboardGridDrag = AudioxSoundboardGrid.attach(document.getElementById('soundboard-layout'), {
  enabled: () => state.soundboardEditing && state.soundboardLayout.mode === 'grid' &&
    !state.soundboardGridSaving && !AudioxUI.isBusy(),
  save: saveSoundboardGridCell,
  status: document.getElementById('sb-order-status'),
});

const logViewer = AudioxLogs.create({
  view: logsViewEl, summary: logsSummaryEl, source: logsSourceEl, level: logsLevelEl,
  text: logsTextEl, limit: logsLimitEl,
  follow: document.getElementById('btn-logs-follow'),
  refresh: document.getElementById('btn-logs-refresh'),
  clear: document.getElementById('btn-logs-clear-filter'),
  notify: setStatus, resolve: AudioxUI.resolve,
});
const loadLogs = () => document.getElementById('system-advanced').open ? logViewer.load() : Promise.resolve();
document.getElementById('system-advanced').addEventListener('toggle', () => {
  if (document.getElementById('system-advanced').open && state.activeTab === 'system') loadLogs();
});
const notifyAudioHealth = AudioxLogs.createHealthNotifier(setStatus);
const showAudioHealth = (data) => {
  notifyAudioHealth(data);
  window.AudioxSystemMonitor?.updateWorker(data?.audio_worker);
};

async function pollSystemIfNeeded() {
  if (AudioxUI.isBusy()) return;
  if (state.activeTab !== 'system') {
    return;
  }
  if (document.visibilityState && document.visibilityState !== 'visible') {
    return;
  }
  if (state.systemPollInFlight) {
    return;
  }

  state.systemPollInFlight = true;
  try {
    await Promise.all([
      loadSystemInfo(),
      loadLogs(),
    ]);
  } finally {
    state.systemPollInFlight = false;
  }
}

function startSystemPolling() {
  if (state.systemPollTimer) {
    clearInterval(state.systemPollTimer);
  }
  state.systemPollTimer = setInterval(() => {
    pollSystemIfNeeded();
  }, 1000);
}

function stopSystemPolling() {
  if (!state.systemPollTimer) {
    return;
  }
  clearInterval(state.systemPollTimer);
  state.systemPollTimer = null;
}

function setStatus(text, kind = '', key = '') {
  if (kind === 'warn') console.warn('AudioX:', text);
  AudioxUI.taskProgress(text);
  AudioxUI.notify(text, kind, key);
}

async function runLoadingTask(title, action) {
  try {
    return await AudioxUI.runTask(title, action);
  } catch (error) {
    setStatus(error, 'warn');
    return false;
  }
}

const effectsManager = (window.AudioxEffects && typeof window.AudioxEffects.createManager === 'function')
  ? window.AudioxEffects.createManager(setStatus, arrangeRoutingGraph)
  : null;

if (effectsManager) {
  effectsManager.mountPanel(effectsMapGridEl, effectsMapStatusEl);
}

const builtinEffectTypes = ['gain', 'distortion', 'pitch', 'reverb', 'gate', 'cut',
  'compressor', 'limiter', 'eq', 'multiband', 'noise_reduction'];
const pluginTemplateIds = new Set();

function pluginTemplateNodeType(id) {
  return `plugin/${id}`;
}

function registerPluginTemplates(plugins) {
  const lite = window.LiteGraph;
  const next = new Map();
  for (const plugin of plugins) {
    const id = window.AudioxPlugins?.normalizePluginId(plugin?.id);
    if (id) next.set(id, plugin);
  }
  for (const id of [...pluginTemplateIds]) {
    if (next.has(id)) continue;
    pluginTemplateIds.delete(id);
    const ctor = lite?.registered_node_types?.[pluginTemplateNodeType(id)];
    if (ctor) ctor.filter = 'audiox-internal';
  }
  for (const [id, plugin] of next) {
    pluginTemplateIds.add(id);
    if (!lite?.registered_node_types) continue;
    const type = pluginTemplateNodeType(id);
    const label = `${plugin.name || id} (plugin)`;
    const existing = lite.registered_node_types[type];
    if (existing) {
      existing.title = label;
      existing.filter = 'audiox';
      continue;
    }
    function AudioxPluginEffectNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: id, nodeKind: 'template', pluginTemplate: true };
      this.title = `Add ${label}`;
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxPluginEffectNode.title = label;
    AudioxPluginEffectNode.filter = 'audiox';
    lite.registerNodeType(type, AudioxPluginEffectNode);
  }
}

function isCreatableEffectType(type) {
  return builtinEffectTypes.includes(type) || pluginTemplateIds.has(type);
}

async function refreshAfterPluginChange() {
  if (effectsManager) await effectsManager.loadEffects();
  await loadRoutingThingsWithOptions({ silent: true });
}

const pluginManager = (window.AudioxPlugins && typeof window.AudioxPlugins.createManager === 'function')
  ? window.AudioxPlugins.createManager({
    setStatus,
    onCatalog: (plugins) => {
      effectsManager?.setPluginCatalog(plugins);
      registerPluginTemplates(plugins);
    },
    onChanged: refreshAfterPluginChange,
  })
  : null;

if (pluginManager) {
  pluginManager.mount({
    listEl: document.getElementById('plugin-list'),
    statusEl: document.getElementById('plugin-status'),
    fileInput: document.getElementById('plugin-file'),
    installButton: document.getElementById('btn-plugin-install'),
  });
}

async function loadPluginCatalog() {
  if (!pluginManager) return;
  try {
    await pluginManager.load();
  } catch (err) {
    pluginManager.setPanelStatus(String(err?.message || err), 'warn');
  }
}

function setActiveTab(tab) {
  if (tab !== 'scripts') {
    cancelChordLearn();
  }
  state.activeTab = tab;
  document.querySelectorAll('.tab-btn').forEach((el) => {
    el.classList.toggle('active', el.dataset.tab === tab);
    if (el.dataset.tab === tab) el.setAttribute('aria-current', 'page');
    else el.removeAttribute('aria-current');
  });
  document.querySelectorAll('.page').forEach((el) => {
    el.classList.toggle('active', el.dataset.page === tab);
  });
  if (tab === 'routing') {
    ensureRoutingGraph();
    resizeRoutingCanvas();
    state.graphCanvas?.setDirty(true, true);
    updateZoomLabel();
    requestAnimationFrame(() => { if (state.autoArrange) fitRoutingGraph(); });
  }
  if (tab === 'system') {
    loadSystemInfo();
    loadLogs();
    startSystemPolling();
  } else {
    stopSystemPolling();
  }
  if (tab === 'volume') {
    loadVolumes();
    startMeterPolling();
  } else {
    stopMeterPolling();
  }
  if (tab === 'bluetooth') {
    loadBluetoothDevices();
  }
  if (tab === 'scripts') {
    loadMidiChords();
  }
  if (tab === 'midi') {
    loadLightDim();
    renderStopAllMidiStatus();
    renderSamplerToggleMidiStatus();
  }
  if (tab === 'effects') {
    loadPluginCatalog();
  } else {
    effectsManager?.closeCustomUis();
  }
  if (tab === 'effects' && effectsManager && !state.sceneRoutingConflict) {
    effectsManager.loadEffects().catch((err) => {
      setStatus(String(err), 'warn', 'effects-load');
    });
  }
}

function updateZoomLabel() {
  const scale = state.graphCanvas ? state.graphCanvas.ds.scale : 1;
  const txt = `${Math.round(scale * 100)}%`;
  zoomLabelEl.textContent = `zoom ${txt}`;
  document.getElementById('btn-zoom-reset').textContent = txt;
}

const bluetoothHideUnnamed = document.getElementById('bluetooth-hide-unnamed');
let bluetoothDevices = [];

bluetoothHideUnnamed.addEventListener('change', () => renderBluetoothDevices(bluetoothDevices));

function isBluetoothInputOnly(device) {
  return Boolean(device?.audio_source) && !device?.audio_sink;
}

function bluetoothDeviceStatus(device) {
  if (!device.paired && !device.connected) {
    return 'Discovered; put device in pairing mode to pair';
  }
  // Older status payloads only report the combined connected/streaming flags.
  const outputUp = device.output_connected ?? (device.connected && !isBluetoothInputOnly(device));
  const inputUp = device.input_connected ?? (device.connected && isBluetoothInputOnly(device));
  const parts = [];
  if (outputUp) {
    parts.push((device.output_streaming ?? device.streaming) === false
      ? 'Connected as audio output; audio paused' : 'Connected as audio output');
  }
  if (inputUp) {
    parts.push((device.input_streaming ?? device.streaming)
      ? 'Connected as audio input; receiving audio' : 'Connected as audio input; waiting for audio');
  }
  if (parts.length) {
    return parts.join(' · ');
  }
  if (device.connected) {
    return 'Connected for audio';
  }
  if (device.connecting) {
    return 'Connecting...';
  }
  return isBluetoothInputOnly(device) ? 'Audio input, not connected' : 'Paired, not connected';
}

function bluetoothDeviceHasName(device) {
  const name = String(device.name || '').trim();
  const compact = (value) => value.replace(/[:-]/g, '').toLowerCase();
  return Boolean(name) && compact(name) !== compact(String(device.address || ''));
}

function renderBluetoothDevices(devices) {
  if (!bluetoothDevicesEl) {
    return;
  }
  const focused = bluetoothDevicesEl.contains(document.activeElement)
    ? document.activeElement.dataset.deviceAction : null;
  const supported = devices.filter((device) => device.classic);
  const nearby = supported.filter((device) => !device.paired && !device.connected);
  const hiddenCount = bluetoothHideUnnamed.checked
    ? nearby.filter((device) => !bluetoothDeviceHasName(device)).length : 0;
  bluetoothDevicesEl.replaceChildren();
  for (const [title, members, emptyText] of [
    ['My devices', supported.filter((device) => device.paired || device.connected), 'No saved devices. Choose Add device to find and pair an audio device.'],
    ['Nearby devices', nearby.filter((device) => !bluetoothHideUnnamed.checked || bluetoothDeviceHasName(device)),
      hiddenCount ? `${hiddenCount} unnamed device(s) hidden. Turn off Hide unnamed devices to show them.`
        : 'No nearby devices found. Put your device in pairing mode, then choose Add device.'],
  ]) {
    const group = document.createElement('section');
    group.className = 'bluetooth-group';
    const heading = document.createElement('h2');
    heading.textContent = title;
    const list = document.createElement('div');
    list.className = 'bluetooth-device-list';
    group.append(heading, list);
    if (title === 'Nearby devices' && hiddenCount && members.length) {
      const notice = document.createElement('p');
      notice.className = 'small';
      notice.textContent = `${hiddenCount} unnamed device(s) hidden.`;
      heading.after(notice);
    }
    bluetoothDevicesEl.append(group);
    if (!members.length) {
      const empty = document.createElement('div');
      empty.className = 'bluetooth-empty';
      empty.textContent = emptyText;
      list.append(empty);
    }
    for (const device of members.sort((a, b) => Number(b.connected) - Number(a.connected) ||
      String(a.name || a.address).localeCompare(String(b.name || b.address)))) {
      const card = document.createElement('section');
      card.className = `card bluetooth-device${device.connected ? ' connected' : ''}`;
      const info = document.createElement('div');
      info.className = 'bluetooth-device-info';
      const name = document.createElement('strong');
      name.textContent = device.name || device.address;
      const status = document.createElement('span');
      status.className = 'bluetooth-device-state';
      status.textContent = bluetoothDeviceStatus(device);
      const address = document.createElement('div');
      address.className = 'bluetooth-device-address';
      address.textContent = `${device.address} · Classic Bluetooth`;
      info.append(name, status, address);
      const actions = document.createElement('div');
      actions.className = 'bluetooth-device-actions';
      card.append(info, actions);
      const addAction = (label, action, style = 'flat') => {
        const button = document.createElement('button');
        button.textContent = label;
        button.className = style;
        button.disabled = bluetoothBusy;
        button.dataset.deviceAction = `${device.address}:${label}`;
        button.setAttribute('aria-label', `${label} ${device.name || device.address}`);
        button.addEventListener('click', action);
        actions.append(button);
      };
      if (!device.paired) {
        addAction('Pair', () => pairBluetoothDevice(device.address), 'primary');
      } else {
        const autoConnectLabel = document.createElement('label');
        autoConnectLabel.className = 'bluetooth-auto-connect';
        const autoConnect = document.createElement('input');
        autoConnect.type = 'checkbox';
        autoConnect.checked = device.auto_connect !== false;
        autoConnect.dataset.deviceAction = `${device.address}:Auto-connect`;
        autoConnect.setAttribute('role', 'switch');
        autoConnect.setAttribute('aria-label', `Auto-connect ${device.name || device.address}`);
        autoConnect.disabled = bluetoothBusy;
        autoConnect.addEventListener('change', () => setBluetoothAutoConnect(device.address, autoConnect.checked));
        const autoConnectText = document.createElement('span');
        autoConnectText.textContent = 'Auto-connect';
        autoConnectLabel.append(autoConnect, autoConnectText);
        actions.append(autoConnectLabel);
        if (device.connected) {
          addAction('Disconnect', () => disconnectBluetoothDevice(device.address));
        } else if (device.classic) {
          addAction('Connect', () => connectBluetoothDevice(device.address), 'primary');
        }
        addAction('Forget', async () => {
          if (await AudioxUI.confirm(`Forget ${device.name || device.address}? You will need to pair it again to reconnect.`, {
            title: 'Forget Bluetooth device?', label: 'Forget device',
          })) await unpairBluetoothDevice(device.address);
        });
      }
      list.append(card);
    }
  }
  if (focused) {
    Array.from(bluetoothDevicesEl.querySelectorAll('[data-device-action]'))
      .find((button) => button.dataset.deviceAction === focused)?.focus();
  }
}

let bluetoothBusy = false;
let bluetoothPollTimer = null;
let bluetoothLoadInFlight = false;
let bluetoothRequestInFlight = false;

async function loadBluetoothDevices() {
  if (!bluetoothStatusEl || bluetoothLoadInFlight || AudioxUI.isBusy()) {
    return;
  }
  if (bluetoothPollTimer) {
    clearTimeout(bluetoothPollTimer);
    bluetoothPollTimer = null;
  }
  bluetoothLoadInFlight = true;
  try {
    const res = await apiFetch('/api/bluetooth/devices', { method: 'GET', signal: AbortSignal.timeout(10000) });
    const data = await res.json();
    if (!res.ok || !data.ok) {
      throw new Error(data.error || `request failed: ${res.status}`);
    }
    bluetoothBusy = bluetoothRequestInFlight || Boolean(data.scanning || data.pairing || data.connecting);
    bluetoothDevices = Array.isArray(data.devices) ? data.devices : [];
    renderBluetoothDevices(bluetoothDevices);
    bluetoothStatusEl.textContent = data.message || (data.ready ? 'Ready' : 'Adapter not ready');
    AudioxUI.resolve('bluetooth-load');
    const scanButton = document.getElementById('btn-bluetooth-scan');
    if (scanButton) {
      scanButton.disabled = bluetoothBusy || Boolean(data.connected) || !data.ready;
      scanButton.textContent = data.scanning ? 'Scanning...' : 'Add device';
    }
    const hint = document.getElementById('bluetooth-scan-hint');
    hint.hidden = !data.connected;
    hint.textContent = 'Disconnect the current audio device before adding another device.';
  } catch (err) {
    bluetoothStatusEl.textContent = AudioxUI.formatMessage(err);
    setStatus(err, 'warn', 'bluetooth-load');
  } finally {
    bluetoothLoadInFlight = false;
    if (state.activeTab === 'bluetooth') {
      bluetoothPollTimer = setTimeout(loadBluetoothDevices, bluetoothBusy ? 1000 : 3000);
    }
  }
}

async function bluetoothRequest(path, body) {
  if (bluetoothBusy) return;
  bluetoothRequestInFlight = true;
  bluetoothBusy = true;
  document.getElementById('btn-bluetooth-scan').disabled = true;
  bluetoothDevicesEl.querySelectorAll('button').forEach((button) => { button.disabled = true; });
  bluetoothStatusEl.textContent = 'Applying Bluetooth request...';
  try {
    const res = await apiFetch(path, { method: 'POST', body });
    const data = await res.json();
    if (!res.ok || !data.ok) {
      throw new Error(data.error || `request failed: ${res.status}`);
    }
    AudioxUI.resolve('bluetooth-action');
  } catch (err) {
    bluetoothStatusEl.textContent = AudioxUI.formatMessage(err);
    setStatus(err, 'warn', 'bluetooth-action');
  } finally {
    bluetoothRequestInFlight = false;
    bluetoothBusy = false;
    await loadBluetoothDevices();
  }
}

function scanBluetoothDevices() {
  return bluetoothRequest('/api/bluetooth/scan', '');
}

function pairBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/pair', `address=${address}`);
}

function connectBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/connect', `address=${address}`);
}

function disconnectBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/disconnect', address ? `address=${address}` : '');
}

function unpairBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/unpair', `address=${address}`);
}

function setBluetoothAutoConnect(address, enabled) {
  return bluetoothRequest('/api/bluetooth/auto-connect', `address=${address}&enabled=${enabled}`);
}

function setRoutingZoom(nextScale) {
  if (!state.graphCanvas) {
    return;
  }

  const clamped = Math.max(0.01, Math.min(2.8, nextScale));
  const [width, height] = state.canvasResolution.size();
  const center = [width * 0.5, height * 0.5];
  state.graphCanvas.setZoom(clamped, center);
  state.graphCanvas.setDirty(true, true);
  updateZoomLabel();
}

function edgeKey(edge) {
  return `${edge.src}|${edge.dst}|${edge.srcChannel}|${edge.dstChannel}`;
}

function findEdgeIndex(edge) {
  const key = edgeKey(edge);
  return state.edges.findIndex((e) => edgeKey(e) === key);
}

function parseConfigText(text) {
  const cfg = {
    usb_playback_channels: 2,
    usb_capture_channels: 2,
    usb_sample_rate: 48000,
    usb_sample_size: 2,
    soundboard_mode: 'play',
    framebuffer_view: 'meters',
    midi_passthrough: 'off',
    keyboard_macros: 0,
  };

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    if (key === 'midi_passthrough') {
      const mode = line.slice(eq + 1).trim();
      cfg.midi_passthrough = ['off', 'unmapped', 'all'].includes(mode) ? mode : 'off';
      continue;
    }
    if (key === 'soundboard_mode') {
      const mode = line.slice(eq + 1).trim().toLowerCase();
      cfg.soundboard_mode = (mode === 'hold') ? 'hold' : 'play';
      continue;
    }
    if (key === 'framebuffer_view') {
      cfg.framebuffer_view = line.slice(eq + 1).trim() === 'soundboard' ? 'soundboard' : 'meters';
      continue;
    }
    const value = Number(line.slice(eq + 1).trim());
    if (Number.isNaN(value)) continue;
    if (key in cfg) cfg[key] = value;
  }
  return cfg;
}

let configuredUsbSampleRate = 48000;

function formatConfigText(cfg) {
  return [
    `usb_playback_channels=${cfg.usb_playback_channels}`,
    `usb_capture_channels=${cfg.usb_capture_channels}`,
    `usb_sample_rate=${cfg.usb_sample_rate}`,
    `usb_sample_size=${cfg.usb_sample_size}`,
    `soundboard_mode=${cfg.soundboard_mode || 'play'}`,
    `framebuffer_view=${cfg.framebuffer_view || 'meters'}`,
    `midi_passthrough=${cfg.midi_passthrough || 'off'}`,
    `keyboard_macros=${cfg.keyboard_macros ? 1 : 0}`,
    ''
  ].join('\n');
}

function parseRoutingText(text) {
  const edges = [];
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    if (!line.startsWith('edge=')) continue;

    const fields = line.slice(5).split(',').map((s) => s.trim());
    if (fields.length !== 4) continue;

    const srcChannel = Number(fields[2]);
    const dstChannel = Number(fields[3]);
    if (Number.isNaN(srcChannel) || Number.isNaN(dstChannel) || srcChannel < 0 || dstChannel < 0) {
      continue;
    }

    edges.push({
      src: fields[0],
      dst: fields[1],
      srcChannel,
      dstChannel,
    });
  }
  return edges;
}

function formatRoutingText() {
  const out = ['# audiox routing v1'];
  if (state.nodes.some((node) => node.id === 'routing_helper')) {
    out.push('node=routing_helper');
  }
  for (const edge of [...state.edges, ...state.dormantEdges]) {
    out.push(`edge=${edge.src},${edge.dst},${edge.srcChannel},${edge.dstChannel}`);
  }
  out.push('');
  return out.join('\n');
}

function makeThingsSignature(things) {
  const rows = [];
  for (const raw of Array.isArray(things) ? things : []) {
    const id = normalizeThingId(raw?.id);
    if (!id) {
      continue;
    }
    const name = String(raw?.name || id);
    const inputs = Math.max(0, Math.min(16, Number(raw?.inputs) || 0));
    const outputs = Math.max(0, Math.min(16, Number(raw?.outputs) || 0));
    rows.push(`${id}|${name}|${inputs}|${outputs}`);
  }
  rows.sort((a, b) => a.localeCompare(b));
  return rows.join('\n');
}

function setFormFromConfig(cfg) {
  if (!cfg) return;
  document.getElementById('usb_playback_channels').value = String(cfg.usb_playback_channels ?? 2);
  document.getElementById('usb_capture_channels').value = String(cfg.usb_capture_channels ?? 2);
  configuredUsbSampleRate = Number(cfg.usb_sample_rate) || 48000;
  document.getElementById('usb_sample_size').value = String(cfg.usb_sample_size ?? 2);
  document.getElementById('midi_passthrough').value = cfg.midi_passthrough || 'off';
  document.getElementById('keyboard_macros').value = String(cfg.keyboard_macros ? 1 : 0);
}

function getConfigFromForm() {
  return {
    usb_playback_channels: Number(document.getElementById('usb_playback_channels').value) || 2,
    usb_capture_channels: Number(document.getElementById('usb_capture_channels').value) || 2,
    usb_sample_rate: configuredUsbSampleRate,
    usb_sample_size: Number(document.getElementById('usb_sample_size').value) || 2,
    soundboard_mode: 'play',
    framebuffer_view: state.framebufferView,
    midi_passthrough: document.getElementById('midi_passthrough').value,
    keyboard_macros: Number(document.getElementById('keyboard_macros').value),
  };
}

function normalizeThingId(v) {
  return String(v || '').trim();
}

const BLUETOOTH_ROUTE_ID = /^(bt_[0-9A-Fa-f]{12}_(in|out)|bluetooth_(in|out))$/;

function isBluetoothRouteId(id) {
  return BLUETOOTH_ROUTE_ID.test(normalizeThingId(id));
}

function isDeviceNode(node) {
  return String(node?.properties?.nodeKind || '') === 'device';
}

function isProtectedNode(node) {
  const kind = String(node?.properties?.nodeKind || '');
  return kind === 'device';
}

function isEffectNode(node) {
  return String(node?.properties?.nodeKind || '') === 'effect';
}

function isEffectThingId(thingId) {
  return /^fx_[a-zA-Z0-9_]+$/.test(String(thingId || '').trim());
}

function isRoutingHelperNode(node) {
  return String(node?.properties?.nodeKind || '') === 'routing_helper';
}

async function createRoutingHelper() {
  const res = await apiFetch('/api/routing/helper/create', { method: 'POST' });
  const txt = await res.text();
  if (!res.ok) {
    throw new Error(`Audio Mixer creation failed: ${res.status} ${txt.trim()}`);
  }
}

async function deleteRoutingHelper() {
  const res = await apiFetch('/api/routing/helper/delete', { method: 'POST' });
  const txt = await res.text();
  if (!res.ok) {
    throw new Error(`Audio Mixer deletion failed: ${res.status} ${txt.trim()}`);
  }
}

async function createEffectFromTemplate(type) {
  const requested = String(type || '').trim();
  if (requested && !isCreatableEffectType(requested)) {
    throw new Error(`effect create failed: plugin ${requested} is not installed`);
  }
  const safeType = requested || 'gain';
  const res = await apiFetch('/api/effect/create', {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: `type=${encodeURIComponent(safeType)}`,
  });
  const txt = await res.text();
  if (!res.ok) {
    throw new Error(`effect create failed: ${res.status} ${txt.trim()}`);
  }
  return JSON.parse(txt || '{}');
}

async function deleteEffectThing(thingId) {
  const id = normalizeThingId(thingId);
  if (!id || !isEffectThingId(id) || !effectsManager) {
    return false;
  }
  const name = effectsManager.displayName(id);
  if (!await AudioxUI.confirm(`Delete ${name}? Its settings and routing connections will be removed.`, {
    title: 'Delete effect?', label: 'Delete effect',
  })) return false;

  try {
    await effectsManager.deleteEffect(id);
    const node = state.nodeByThingId.get(id);
    if (node && state.graph && state.graphCanvas) {
      state.internalGraphMutation = true;
      state.graph.remove(node);
      state.internalGraphMutation = false;
      state.nodeByThingId.delete(id);
      syncEdgesFromGraph();
      state.graphCanvas.setDirty(true, true);
    }
    await loadRoutingThingsWithOptions({ silent: true, force: true });
    await loadRoutingFile();
    await effectsManager.loadEffects();
    setStatus(`${name} deleted`, 'ok');
    return true;
  } catch (err) {
    setStatus(String(err), 'warn');
    return false;
  }
}

function shortNodeTitle(label) {
  const txt = String(label || '').trim();
  if (!txt) {
    return 'thing';
  }
  if (txt.length <= 18) {
    return txt;
  }
  return `${txt.slice(0, 15)}...`;
}

function getOutputChannelCount(node) {
  if (Number.isFinite(Number(node?._baseOutputCount))) {
    return Number(node._baseOutputCount);
  }
  return Array.isArray(node?.outputs) ? node.outputs.length : 0;
}

function getInputChannelCount(node) {
  if (Number.isFinite(Number(node?._baseInputCount))) {
    return Number(node._baseInputCount);
  }
  return Array.isArray(node?.inputs) ? node.inputs.length : 0;
}

function syncEdgesFromGraph() {
  if (!state.graph || state.suspendGraphSync) {
    return;
  }

  const prevSelectedKey = (state.selectedEdgeIdx >= 0 && state.selectedEdgeIdx < state.edges.length)
    ? edgeKey(state.edges[state.selectedEdgeIdx])
    : null;

  const next = [];
  const dormant = [];
  const seen = new Set();
  for (const edge of [...state.edges, ...state.dormantEdges]) {
    const srcNode = state.nodeByThingId.get(edge.src);
    const dstNode = state.nodeByThingId.get(edge.dst);
    if (!srcNode || !dstNode) {
      if ((srcNode || isBluetoothRouteId(edge.src)) && (dstNode || isBluetoothRouteId(edge.dst))) {
        const key = edgeKey(edge);
        if (!seen.has(key)) {
          seen.add(key);
          dormant.push(edge);
        }
      }
      continue;
    }

    const srcChannel = Number(edge.srcChannel);
    const dstChannel = Number(edge.dstChannel);
    if (!Number.isFinite(srcChannel) || !Number.isFinite(dstChannel)) {
      continue;
    }
    if (srcChannel < 0 || srcChannel >= getOutputChannelCount(srcNode)) {
      continue;
    }
    if (dstChannel < 0 || dstChannel >= getInputChannelCount(dstNode)) {
      continue;
    }

    const normalized = {
      src: normalizeThingId(edge.src),
      dst: normalizeThingId(edge.dst),
      srcChannel,
      dstChannel,
    };
    if (!normalized.src || !normalized.dst) {
      continue;
    }

    const key = edgeKey(normalized);
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    next.push(normalized);
  }

  next.sort((a, b) => edgeKey(a).localeCompare(edgeKey(b)));
  state.edges = next;
  state.dormantEdges = dormant;

  if (prevSelectedKey) {
    state.selectedEdgeIdx = state.edges.findIndex((e) => edgeKey(e) === prevSelectedKey);
  }
  if (state.selectedEdgeIdx >= state.edges.length) {
    state.selectedEdgeIdx = state.edges.length - 1;
  }

  syncRoutingVirtualNodes();
  state.graphCanvas.setDirty(true, true);
}

function addEdgeFromConnection(srcNode, srcSlot, dstNode, dstSlot) {
  const result = AudioxRouting.resolveRoutingConnection(
    { things: routingThingsForView(), virtuals: state.virtuals, advanced: state.advancedRouting },
    { id: routingViewId(srcNode), slot: Number(srcSlot) },
    { id: routingViewId(dstNode), slot: Number(dstSlot) });
  if (result.error) {
    setStatus(result.error, 'warn');
    return false;
  }
  if (result.bind) {
    state.virtuals = state.virtuals.map((v) => (v.id === result.bind.id ? { ...v, thing: result.bind.thing } : v));
    syncEdgesFromGraph();
    setStatus('stereo wire connected; now wire its Left/Right sockets', 'ok');
    return true;
  }

  const fresh = result.edges.filter((edge) => findEdgeIndex(edge) < 0);
  if (!fresh.length) {
    setStatus('route already exists', 'warn');
    return false;
  }

  state.edges.push(...fresh);
  AudioxUI.setUnsaved('routing');
  syncEdgesFromGraph();
  recordRoutingHistory();
  setStatus('route added (not saved yet)', 'ok');
  return true;
}

const VIRTUAL_NODE_TITLES = { split: 'Split Channels', merge: 'Merge Channels' };
const ROUTING_WIRE_COLORS = { stereo: '#c3a6ff', mono: '#79c8ff' };

function routingViewId(node) {
  return normalizeThingId(node?.properties?.thingId || node?.properties?.virtualId);
}

function isVirtualRoutingNode(node) {
  return !!node?.properties?.virtualId;
}

function routingNodeById(id) {
  return state.nodeByThingId.get(id) || state.virtualNodeById.get(id);
}

function routingThingsForView() {
  return state.nodes.map(({ id, inputs, outputs }) => ({ id, inputs, outputs }));
}

function computeRoutingView() {
  const view = AudioxRouting.buildRoutingView({
    things: routingThingsForView(), edges: state.edges, virtuals: state.virtuals,
    advanced: state.advancedRouting, nextId: (kind) => `virtual:${kind}:${++state.virtualSeq}`,
  });
  state.virtuals = view.virtuals;
  state.routingView = view;
  return view;
}

function routingWires() {
  if (state.routingView) return state.routingView.wires;
  return state.edges.map((edge) => ({ key: edgeKey(edge), type: 'mono', edges: [edge],
    src: { id: edge.src, slot: Number(edge.srcChannel) }, dst: { id: edge.dst, slot: Number(edge.dstChannel) } }));
}

function addRoutingSlots(node, slots) {
  for (const slot of slots?.inputs || []) node.addInput(slot.label, slot.type);
  for (const slot of slots?.outputs || []) node.addOutput(slot.label, slot.type);
}

function routingConnectInput(inputIndex, _outputType, _outputSlot, outputNode, outputIndex) {
  addEdgeFromConnection(outputNode, outputIndex, this, inputIndex);
  return false;
}

function addVirtualGraphNode(virtual) {
  const node = window.LiteGraph.createNode('audiox/thing');
  if (!node) return;
  node.title = VIRTUAL_NODE_TITLES[virtual.kind];
  node.properties = { virtualId: virtual.id, nodeKind: virtual.kind };
  node.color = '#3d3466';
  node.bgcolor = '#1c1833';
  addRoutingSlots(node, state.routingView?.slots.get(virtual.id));
  node.size = [190, Math.max(60, node.size[1])];
  const anchor = virtual.thing ? state.nodeByThingId.get(virtual.thing) : null;
  const saved = state.positions[virtual.id];
  if (saved) node.pos = [saved.x, saved.y];
  else if (anchor) {
    node.pos = virtual.kind === 'split' ? [anchor.pos[0] + anchor.size[0] + 50, anchor.pos[1]] :
      [anchor.pos[0] - node.size[0] - 50, anchor.pos[1]];
  } else {
    const [x, y] = getViewCenterGraphCoords();
    node.pos = [x - node.size[0] / 2, y];
  }
  node.onConnectInput = routingConnectInput;
  state.graph.add(node);
  state.virtualNodeById.set(virtual.id, node);
}

function syncRoutingVirtualNodes() {
  const view = computeRoutingView();
  if (!state.graph || state.suspendGraphSync) return view;
  const wanted = new Set(view.virtuals.map((v) => v.id));
  state.internalGraphMutation = true;
  for (const [id, node] of state.virtualNodeById) {
    if (wanted.has(id)) continue;
    state.graph.remove(node);
    state.virtualNodeById.delete(id);
  }
  state.internalGraphMutation = false;
  for (const virtual of view.virtuals) {
    if (!state.virtualNodeById.has(virtual.id)) addVirtualGraphNode(virtual);
  }
  return view;
}

function removeRoutingWire(wire) {
  const keys = new Set(wire.edges.map(edgeKey));
  state.edges = state.edges.filter((edge) => !keys.has(edgeKey(edge)));
  if (wire.binding) {
    state.virtuals = state.virtuals.map((v) => (v.id === wire.binding ? { ...v, thing: null } : v));
  }
  if (keys.size) AudioxUI.setUnsaved('routing');
  syncEdgesFromGraph();
  if (keys.size) recordRoutingHistory();
  setStatus(keys.size ? 'route removed (not saved yet)' : 'stereo wire removed', 'ok');
}

function removeVirtualRoutingNode(id) {
  const binding = routingWires().find((wire) => wire.binding === id);
  state.virtuals = state.virtuals.filter((v) => v.id !== id);
  if (binding) removeRoutingWire({ ...binding, binding: null });
  else syncEdgesFromGraph();
}

function setRoutingAdvanced(advanced) {
  state.advancedRouting = !!advanced;
  try { window.localStorage?.setItem(ROUTING_ADVANCED_KEY, advanced ? '1' : '0'); } catch (_) {}
  const toggle = document.getElementById('routing-advanced');
  if (toggle) toggle.checked = state.advancedRouting;
  for (const kind of Object.keys(VIRTUAL_NODE_TITLES)) {
    const ctor = window.LiteGraph?.registered_node_types?.[`utility/${kind}_channels`];
    if (ctor) ctor.filter = state.advancedRouting ? 'audiox-internal' : 'audiox';
  }
  state.virtuals = [];
  rebuildRoutingGraph();
}

function getViewCenterGraphCoords() {
  if (!state.graphCanvas) {
    return [0, 0];
  }

  const area = state.graphCanvas.visible_area;
  if (area && area.length >= 4) {
    return [area[0] + (area[2] * 0.5), area[1] + (area[3] * 0.5)];
  }

  const scale = state.graphCanvas.ds.scale;
  const offset = state.graphCanvas.ds.offset;
  const [width, height] = state.canvasResolution.size();
  return [
    width * 0.5 / scale - offset[0],
    height * 0.5 / scale - offset[1],
  ];
}

function resizeRoutingCanvas() {
  if (!state.graphCanvas || !graphWrapEl) {
    return;
  }

  const rect = graphWrapEl.getBoundingClientRect();
  const w = Math.max(1, Math.floor(rect.width));
  const h = Math.max(1, Math.floor(rect.height));

  state.canvasResolution.resize(w, h, window.devicePixelRatio);
}

let routingWireCacheKey = '';
let routingWireCache = new Map();

function routingWirePaths() {
  const wires = routingWires().flatMap(wire => {
    const src = routingNodeById(wire.src.id), dst = routingNodeById(wire.dst.id);
    if (!src || !dst) return [];
    const a = src.getConnectionPos(false, Number(wire.src.slot));
    const b = dst.getConnectionPos(true, Number(wire.dst.slot));
    return a && b ? [{ key: wire.key, a: Array.from(a), b: Array.from(b) }] : [];
  });
  const key = JSON.stringify(wires);
  if (key !== routingWireCacheKey) {
    routingWireCache = new Map(wires.map(({ key: wireKey, a, b }) => [
      wireKey, AudioxRouting.wirePath(a, b),
    ]));
    routingWireCacheKey = key;
  }
  return routingWireCache;
}

function distToSegment(px, py, ax, ay, bx, by) {
  const vx = bx - ax;
  const vy = by - ay;
  const wx = px - ax;
  const wy = py - ay;
  const c1 = (wx * vx) + (wy * vy);
  if (c1 <= 0) {
    const dx = px - ax;
    const dy = py - ay;
    return Math.sqrt((dx * dx) + (dy * dy));
  }
  const c2 = (vx * vx) + (vy * vy);
  if (c2 <= c1) {
    const dx = px - bx;
    const dy = py - by;
    return Math.sqrt((dx * dx) + (dy * dy));
  }
  const t = c1 / c2;
  const ix = ax + (t * vx);
  const iy = ay + (t * vy);
  const dx = px - ix;
  const dy = py - iy;
  return Math.sqrt((dx * dx) + (dy * dy));
}

function linkDistance(wire, x, y) {
  const path = routingWirePaths().get(wire.key);
  if (!path) return Number.POSITIVE_INFINITY;
  let best = Number.POSITIVE_INFINITY;
  let prev = path[0];
  for (let i = 1; i < path.length; i += 1) {
    const next = path[i];
    const d = distToSegment(x, y, prev[0], prev[1], next[0], next[1]);
    if (d < best) {
      best = d;
    }
    prev = next;
  }
  return best;
}

function removeNearestLinkAt(x, y) {
  if (!state.graph) {
    return false;
  }

  const onNode = state.graph.getNodeOnPos(x, y, null, 6);
  if (onNode) {
    return false;
  }

  const threshold = 16 / Math.max(0.35, state.graphCanvas.ds.scale);
  const wires = routingWires();
  let bestIdx = -1;
  let bestDist = Number.POSITIVE_INFINITY;

  for (let i = 0; i < wires.length; i += 1) {
    const d = linkDistance(wires[i], x, y);
    if (d < bestDist) {
      bestDist = d;
      bestIdx = i;
    }
  }

  if (bestIdx < 0 || bestDist > threshold) {
    return false;
  }

  removeRoutingWire(wires[bestIdx]);
  return true;
}

function removeEdgeByIndex(index) {
  if (index < 0 || index >= state.edges.length) {
    return;
  }

  state.edges.splice(index, 1);

  AudioxUI.setUnsaved('routing');
  syncEdgesFromGraph();
  recordRoutingHistory();
  setStatus('route removed (not saved yet)', 'ok');
}

function deleteSelectedEdge() {
  if (state.selectedEdgeIdx < 0) {
    setStatus('no selected route to remove', 'warn');
    return;
  }
  removeEdgeByIndex(state.selectedEdgeIdx);
}

function ensureRoutingGraph() {
  if (state.graph && state.graphCanvas) {
    return;
  }

  if (typeof window.LGraph !== 'function' || typeof window.LGraphCanvas !== 'function' || typeof window.LiteGraph !== 'object') {
    setStatus('litegraph failed to load', 'warn');
    return;
  }

  state.graph = new window.LGraph();
  const graphRemove = state.graph.remove.bind(state.graph);
  state.graph.remove = (node) => {
    if (!state.internalGraphMutation && isProtectedNode(node)) {
      setStatus('device nodes cannot be deleted', 'warn');
      return;
    }

    if (!state.internalGraphMutation && isEffectNode(node)) {
      setStatus('delete effects from the Effects page panel', 'warn');
      return;
    }

    if (!state.internalGraphMutation && isVirtualRoutingNode(node)) {
      removeVirtualRoutingNode(node.properties.virtualId);
      return;
    }

    graphRemove(node);
  };
  state.graphCanvas = new window.LGraphCanvas(graphCanvasEl, state.graph, { autoresize: false });
  state.canvasResolution = AudioxRouting.createCanvasResolution(state.graphCanvas);

  if (!window.LiteGraph.registered_node_types['audiox/thing']) {
    function AudioxThingNode() {
      this.size = [260, 60];
      this.properties = { thingId: '' };
    }
    AudioxThingNode.title = 'Thing';
    AudioxThingNode.filter = 'audiox-internal';
    window.LiteGraph.registerNodeType('audiox/thing', AudioxThingNode);
  }

  const thingCtor = window.LiteGraph.registered_node_types['audiox/thing'];
  if (thingCtor) {
    thingCtor.filter = 'audiox-internal';
  }

  if (!window.LiteGraph.registered_node_types['effects/gain']) {
    function AudioxEffectGainNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'gain', nodeKind: 'template' };
      this.title = 'Add Gain Effect';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectGainNode.title = 'Gain Effect';
    AudioxEffectGainNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('effects/gain', AudioxEffectGainNode);
  }

  if (!window.LiteGraph.registered_node_types['utility/mixer']) {
    function AudioxRoutingHelperNode() {
      this.size = [220, 80];
      this.properties = { routingHelperTemplate: true, nodeKind: 'template' };
      this.title = 'Add Audio Mixer';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxRoutingHelperNode.title = 'Audio Mixer';
    AudioxRoutingHelperNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('utility/mixer', AudioxRoutingHelperNode);
  }

  if (!window.LiteGraph.registered_node_types['effects/distortion']) {
    function AudioxEffectDistortionNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'distortion', nodeKind: 'template' };
      this.title = 'Add Distortion Effect';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectDistortionNode.title = 'Distortion Effect';
    AudioxEffectDistortionNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('effects/distortion', AudioxEffectDistortionNode);
  }

  if (!window.LiteGraph.registered_node_types['effects/pitch']) {
    function AudioxEffectPitchNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'pitch', nodeKind: 'template' };
      this.title = 'Add Pitch Effect';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectPitchNode.title = 'Pitch Effect';
    AudioxEffectPitchNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('effects/pitch', AudioxEffectPitchNode);
  }

  if (!window.LiteGraph.registered_node_types['effects/reverb']) {
    function AudioxEffectReverbNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'reverb', nodeKind: 'template' };
      this.title = 'Add Reverb Effect';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectReverbNode.title = 'Reverb Effect';
    AudioxEffectReverbNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('effects/reverb', AudioxEffectReverbNode);
  }

  if (!window.LiteGraph.registered_node_types['effects/gate']) {
    function AudioxEffectGateNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'gate', nodeKind: 'template' };
      this.title = 'Add Noise Gate Effect';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectGateNode.title = 'Noise Gate Effect';
    AudioxEffectGateNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('effects/gate', AudioxEffectGateNode);
  }

  if (!window.LiteGraph.registered_node_types['effects/cut']) {
    function AudioxEffectCutNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'cut', nodeKind: 'template' };
      this.title = 'Add Mute (Cut)';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectCutNode.title = 'Mute (Cut)';
    AudioxEffectCutNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('effects/cut', AudioxEffectCutNode);
  }
  const advancedEffects = {
    compressor: 'Compressor', limiter: 'Limiter', eq: 'Parametric EQ',
    multiband: 'Multiband Compressor', noise_reduction: 'Noise Reduction',
  };
  for (const [type, label] of Object.entries(advancedEffects)) {
    if (window.LiteGraph.registered_node_types[`effects/${type}`]) continue;
    function AdvancedEffectNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: type, nodeKind: 'template' };
      this.title = `Add ${label} Effect`;
      for (let channel = 0; channel < 2; channel++) {
        this.addInput(`in ${channel + 1}`, 'audio');
        this.addOutput(`out ${channel + 1}`, 'audio');
      }
    }
    AdvancedEffectNode.title = `${label} Effect`;
    AdvancedEffectNode.filter = 'audiox';
    window.LiteGraph.registerNodeType(`effects/${type}`, AdvancedEffectNode);
  }
  state.graphCanvas.background_image = null;
  window.LiteGraph.NODE_DEFAULT_COLOR = '#1f4a62';
  window.LiteGraph.NODE_DEFAULT_BGCOLOR = '#102635';
  window.LiteGraph.NODE_DEFAULT_BOXCOLOR = '#7fd1ff';
  window.LiteGraph.NODE_TEXT_COLOR = '#eaf5ff';
  window.LiteGraph.LINK_COLOR = '#79c8ff';
  window.LiteGraph.EVENT_LINK_COLOR = '#79c8ff';
  state.graphCanvas.default_link_color = '#79c8ff';
  for (const [kind, label] of Object.entries(VIRTUAL_NODE_TITLES)) {
    const key = `utility/${kind}_channels`;
    if (!window.LiteGraph.registered_node_types[key]) {
      function VirtualChannelsNode() {
        this.size = [190, 60];
        this.properties = { virtualTemplate: kind, nodeKind: 'template' };
        this.title = label;
      }
      VirtualChannelsNode.title = label;
      window.LiteGraph.registerNodeType(key, VirtualChannelsNode);
    }
    window.LiteGraph.registered_node_types[key].filter = state.advancedRouting ? 'audiox-internal' : 'audiox';
  }
  // Typed sockets: LiteGraph only lets stereo connect to stereo and mono to mono.
  window.LGraphCanvas.link_type_colors.stereo = ROUTING_WIRE_COLORS.stereo;
  window.LGraphCanvas.link_type_colors.mono = ROUTING_WIRE_COLORS.mono;
  state.graphCanvas.default_connection_color_byType = { ...ROUTING_WIRE_COLORS };
  state.graphCanvas.default_connection_color_byTypeOff = { stereo: '#6f5f99', mono: '#47718f' };
  state.graphCanvas.clear_background = false;
  state.graphCanvas.canvas.style.background = '#0d1e2a';
  if (state.graphCanvas.bgcanvas) {
    state.graphCanvas.bgcanvas.style.background = '#0d1e2a';
  }
  state.graphCanvas.onDrawBackground = (ctx, visibleArea) => {
    const x = Array.isArray(visibleArea) ? visibleArea[0] - 4096 : -4096;
    const y = Array.isArray(visibleArea) ? visibleArea[1] - 4096 : -4096;
    const w = Array.isArray(visibleArea) ? visibleArea[2] + 8192 : 8192;
    const h = Array.isArray(visibleArea) ? visibleArea[3] + 8192 : 8192;
    ctx.fillStyle = '#0d1e2a';
    ctx.fillRect(x, y, w, h);
  };
  state.graphCanvas.allow_searchbox = false;
  state.graphCanvas.filter = 'audiox';
  state.graphCanvas.render_shadows = true;
  state.graphCanvas.render_curved_connections = true;
  state.graphCanvas.connections_width = 3;
  state.graphCanvas.getNodeMenuOptions = (node) => {
    if (isProtectedNode(node)) {
      return [];
    }
    if (isEffectNode(node)) {
      return [];
    }
    return [
      {
        content: 'Remove Node',
        callback: async () => {
          if (isRoutingHelperNode(node)) {
            if (!await AudioxUI.confirm('Delete the Audio Mixer? Any connections through it will be removed.', {
              title: 'Delete Audio Mixer?', label: 'Delete mixer',
            })) return;
            try {
              await deleteRoutingHelper();
              await loadRoutingThingsWithOptions({ silent: true, force: true });
              await loadRoutingFile();
              setStatus('Audio Mixer deleted', 'ok');
            } catch (err) {
              setStatus(String(err), 'warn');
            }
            return;
          }
          if (isProtectedNode(node)) {
            return;
          }
          state.graph.remove(node);
          syncEdgesFromGraph();
          state.graphCanvas.setDirty(true, true);
        },
      },
    ];
  };

  state.graphCanvas.onNodeMoved = (node) => {
    if (!routingViewId(node)) {
      return;
    }
    state.autoArrange = false;
    for (const [id, current] of [...state.nodeByThingId, ...state.virtualNodeById]) {
      state.positions[id] = { x: Math.round(current.pos[0]), y: Math.round(current.pos[1]) };
    }
    state.graphCanvas.setDirty(true, true);
  };

  state.graph.onNodeAdded = (node) => {
    if (!node) {
      return;
    }

    const virtualKind = node.properties?.virtualTemplate;
    if (virtualKind) {
      const pos = Array.isArray(node.pos) ? { x: node.pos[0], y: node.pos[1] } : null;
      state.internalGraphMutation = true;
      state.graph.remove(node);
      state.internalGraphMutation = false;
      if (state.advancedRouting) {
        setStatus('Split and merge nodes are only used in simple routing mode', 'warn');
        return;
      }
      const id = `virtual:${virtualKind}:${++state.virtualSeq}`;
      if (pos) state.positions[id] = pos;
      state.virtuals = [...state.virtuals, { id, kind: virtualKind, thing: null }];
      setTimeout(() => {
        syncEdgesFromGraph();
        setStatus(`${VIRTUAL_NODE_TITLES[virtualKind]} added; connect its stereo socket first`, 'ok');
      }, 0);
      return;
    }
    const templateType = String(node.properties?.effectTypeTemplate || '').trim();
    if (node.properties?.routingHelperTemplate) {
      const pos = Array.isArray(node.pos) ? { x: node.pos[0], y: node.pos[1] } : null;
      state.internalGraphMutation = true;
      state.graph.remove(node);
      state.internalGraphMutation = false;
      createRoutingHelper()
        .then(async () => {
          await loadRoutingThingsWithOptions({ silent: true, force: true });
          await loadRoutingFile();
          if (pos) {
            state.positions.routing_helper = pos;
          }
          rebuildRoutingGraph();
          setStatus('Audio Mixer created', 'ok');
        })
        .catch((err) => setStatus(String(err), 'warn'));
      return;
    }
    if (templateType && (isCreatableEffectType(templateType) || node.properties?.pluginTemplate)) {
      const pos = Array.isArray(node.pos) ? { x: node.pos[0], y: node.pos[1] } : null;
      state.internalGraphMutation = true;
      state.graph.remove(node);
      state.internalGraphMutation = false;

      createEffectFromTemplate(templateType)
        .then(async (created) => {
          const createdId = normalizeThingId(created?.id);
          await loadRoutingThingsWithOptions({ silent: true, force: true });
          await loadRoutingFile();
          if (pos && createdId && isEffectThingId(createdId)) {
            state.positions[createdId] = { x: pos.x, y: pos.y };
            rebuildRoutingGraph();
          }
          if (effectsManager) {
            await effectsManager.loadEffects();
          }
          setStatus(`${templateType} effect created`, 'ok');
        })
        .catch((err) => {
          setStatus(String(err), 'warn');
        });
      return;
    }

    const thingId = normalizeThingId(node.properties?.thingId);
    if (!thingId) {
      return;
    }
    state.nodeByThingId.set(thingId, node);
    syncEdgesFromGraph();
  };

  state.graphCanvas.onDrawForeground = (ctx) => {
    const selected = new Set(Object.values(state.graphCanvas.selected_nodes || {})
      .map(node => routingViewId(node)).filter(Boolean));

    const paths = routingWirePaths();
    ctx.save();
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';
    for (const wire of routingWires()) {
      const path = paths.get(wire.key);
      if (!path) continue;

      const isSelected = selected.has(wire.src.id) || selected.has(wire.dst.id);
      const stereo = wire.type === 'stereo';
      const [a, first, second, b] = AudioxRouting.wireCurve(path[0], path[path.length - 1]);
      ctx.beginPath();
      ctx.moveTo(a[0], a[1]);
      ctx.bezierCurveTo(first[0], first[1], second[0], second[1], b[0], b[1]);
      ctx.strokeStyle = isSelected ? '#ffcf66' : ROUTING_WIRE_COLORS[wire.type] || ROUTING_WIRE_COLORS.mono;
      ctx.lineWidth = (isSelected ? 4 : 3) + (stereo ? 1.5 : 0);
      ctx.globalAlpha = isSelected ? 0.85 : stereo ? 0.6 : 0.45;
      ctx.stroke();
    }
    ctx.restore();
  };

  state.graphCanvas.onMouse = (ev) => {
    if (!state.edgeDeleteEnabled) {
      return false;
    }
    if (ev.type !== 'mousedown' || ev.button !== 0) {
      return false;
    }

    const pos = state.graphCanvas.convertEventToCanvasOffset(ev);
    if (!pos || pos.length < 2) {
      return false;
    }
    if (state.graph.getNodeOnPos(pos[0], pos[1])) return false;

    if (removeNearestLinkAt(pos[0], pos[1])) {
      ev.preventDefault();
      ev.stopPropagation();
      return true;
    }
    return false;
  };

  updateZoomLabel();
  resizeRoutingCanvas();
}

function routingGraphNodes() {
  return [...state.nodeByThingId, ...(state.virtualNodeById || [])];
}

function routingNodeBoxes() {
  return routingGraphNodes().map(([id, node]) => ({
    id, kind: node.properties.nodeKind,
    inputs: node.inputs?.length || 0, outputs: node.outputs?.length || 0,
    width: node.size[0], height: node.size[1], x: node.pos[0], y: node.pos[1],
  }));
}

function fitRoutingGraph() {
  if (!state.graphCanvas) return;
  resizeRoutingCanvas();
  const [width, height] = state.canvasResolution.size();
  const view = AudioxRouting.fitNodes(routingNodeBoxes(), width, height);
  if (!view) return;
  state.graphCanvas.ds.scale = view.scale;
  state.graphCanvas.ds.offset[0] = view.offset[0];
  state.graphCanvas.ds.offset[1] = view.offset[1];
  state.graphCanvas.setDirty(true, true);
  updateZoomLabel();
}

function arrangeRoutingGraph() {
  const boxes = routingNodeBoxes();
  const routes = state.routingView ?
    state.routingView.wires.map((wire) => ({ src: wire.src.id, dst: wire.dst.id })) : state.edges;
  const positions = AudioxRouting.layoutNodes(boxes, routes);
  let newNodeY = Math.max(60, ...boxes.filter((node) => state.positions[node.id])
    .map((node) => state.positions[node.id].y + node.height + 80));
  for (const [id, node] of routingGraphNodes()) {
    const pos = positions[id];
    if (state.autoArrange) {
      node.pos = [pos.x, pos.y];
    } else if (!state.positions[id] && node.properties?.virtualId) {
      // Already placed beside the stereo socket it was derived from.
      state.positions[id] = { x: node.pos[0], y: node.pos[1] };
    } else if (!state.positions[id]) {
      node.pos = [40, newNodeY];
      state.positions[id] = { x: 40, y: newNodeY };
      newNodeY += node.size[1] + 80;
    } else {
      node.pos = [state.positions[id].x, state.positions[id].y];
    }
  }
  if (state.autoArrange && state.activeTab === 'routing') fitRoutingGraph();
}

function rebuildRoutingGraph() {
  ensureRoutingGraph();
  if (!state.graph || !state.graphCanvas) {
    return;
  }

  state.suspendGraphSync = true;
  state.internalGraphMutation = true;
  state.graph.clear();
  state.nodeByThingId.clear();
  state.virtualNodeById.clear();
  const view = computeRoutingView();
  if (effectsManager) {
    effectsManager.nodeById.clear();
  }

  state.nodes.forEach((thing) => {
    const node = window.LiteGraph.createNode('audiox/thing');
    if (!node) {
      return;
    }
    node.title = shortNodeTitle(thing.label);
    node.size = [260, 60];
    const isEffect = isEffectThingId(thing.id);
    const isHelper = thing.id === 'routing_helper';
    node.properties = {
      thingId: thing.id,
      nodeKind: isEffect ? 'effect' : (isHelper ? 'routing_helper' : 'device'),
    };
    node.removable = isEffect || isHelper;
    node._baseInputCount = thing.inputs;
    node._baseOutputCount = thing.outputs;
    addRoutingSlots(node, view.slots.get(thing.id));

    const pos = state.positions[thing.id] || { x: 40, y: 60 };
    node.pos = [pos.x, pos.y];

    node.onConnectInput = routingConnectInput;

    node.onConnectionsChange = function() {
      if (!state.suspendGraphSync) {
        syncEdgesFromGraph();
      }
    };

    state.graph.add(node);
    state.nodeByThingId.set(thing.id, node);
    if (isEffect && effectsManager) {
      effectsManager.registerNode(node);
    }
  });
  view.virtuals.forEach(addVirtualGraphNode);

  state.internalGraphMutation = false;
  state.suspendGraphSync = false;
  arrangeRoutingGraph();
  syncEdgesFromGraph();
  state.graphCanvas.setDirty(true, true);
}

function buildNodesFromThings(things) {
  const previous = { ...state.positions };
  state.nodes = [];
  state.positions = Object.fromEntries(Object.entries(previous).filter(([id]) => id.startsWith('virtual:')));

  const safeThings = Array.isArray(things) ? things : [];
  for (const raw of safeThings) {
    const id = normalizeThingId(raw?.id);
    if (!id) {
      continue;
    }

    const inputs = Math.max(0, Math.min(16, Number(raw.inputs) || 0));
    const outputs = Math.max(0, Math.min(16, Number(raw.outputs) || 0));

    state.nodes.push({
      id,
      label: String(raw.name || id),
      inputs,
      outputs,
    });

    if (previous[id]) {
      state.positions[id] = previous[id];
    }
  }

  if (!state.nodes.length) {
    setStatus('no routing things from backend', 'warn');
  }
}

async function loadRoutingThings() {
  return loadRoutingThingsWithOptions({ silent: false, force: true });
}

async function loadRoutingThingsWithOptions(options = {}) {
  const silent = !!options.silent;
  const force = !!options.force;

  try {
    const res = await apiFetch('/api/routing/things', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      if (!silent) {
        setStatus(`things load failed: ${res.status} ${txt.trim()}`, 'warn');
      }
      return false;
    }

    const parsed = JSON.parse(txt);
    const nextThings = parsed?.things || [];
    const nextSig = makeThingsSignature(nextThings);
    if (!force && nextSig === state.thingsSignature) {
      return false;
    }

    state.thingsSignature = nextSig;
    buildNodesFromThings(nextThings);
    rebuildRoutingGraph();
    return true;
  } catch (err) {
    if (!silent) {
      setStatus(`things load error: ${err}`, 'warn');
    }
    return false;
  }
}

async function pollRoutingThingsIfNeeded() {
  if (state.sceneRoutingConflict) return;
  if (AudioxUI.isBusy()) return;
  if (state.activeTab !== 'routing') {
    return;
  }
  if (document.visibilityState && document.visibilityState !== 'visible') {
    return;
  }
  if (state.thingsPollInFlight) {
    return;
  }

  state.thingsPollInFlight = true;
  try {
    const changed = await loadRoutingThingsWithOptions({ silent: true, force: false });
    if (changed) {
      setStatus('routing things updated', 'ok');
    }
  } finally {
    state.thingsPollInFlight = false;
  }
}

function startRoutingThingsPolling() {
  if (state.thingsPollTimer) {
    clearInterval(state.thingsPollTimer);
  }
  state.thingsPollTimer = setInterval(() => {
    pollRoutingThingsIfNeeded();
    pollEffectsIfNeeded();
    if (state.activeTab === 'midi' && !document.hidden) loadPassthroughOnly();
  }, 1000);
}

async function pollEffectsIfNeeded() {
  if (state.sceneRoutingConflict) return;
  if (!effectsManager) {
    return;
  }
  if (effectsManager.mutationInFlight > 0) {
    return;
  }
  if (state.activeTab !== 'routing' && state.activeTab !== 'effects') {
    return;
  }
  if (document.visibilityState && document.visibilityState !== 'visible') {
    return;
  }
  if (state.effectsPollInFlight) {
    return;
  }

  state.effectsPollInFlight = true;
  try {
    await effectsManager.loadEffects();
  } catch (_) {
  } finally {
    state.effectsPollInFlight = false;
  }
}

async function loadRoutingFile() {
  setStatus('loading routing...');
  try {
    const res = await apiFetch(routingPath, { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      state.edges = [];
      state.dormantEdges = [];
      state.virtuals = [];
      rebuildRoutingGraph();
      AudioxUI.setUnsaved('routing', false);
      routingHistory.reset([]);
      setStatus('routing file missing, starting empty', 'warn');
      return;
    }

    state.edges = parseRoutingText(txt);
    state.dormantEdges = [];
    state.virtuals = [];
    state.selectedEdgeIdx = -1;
    rebuildRoutingGraph();
    AudioxUI.setUnsaved('routing', false);
    routingHistory.reset([...state.edges, ...state.dormantEdges]);
    setStatus('routing loaded', 'ok');
  } catch (err) {
    setStatus(`routing load error: ${err}`, 'warn');
  }
}

window.deleteEffectThing = deleteEffectThing;
window.loadRoutingFile = loadRoutingFile;
window.loadRoutingThingsWithOptions = loadRoutingThingsWithOptions;

async function reloadRoutingFast(successMessage = null) {
  try {
    const res = await apiFetch('/api/routing/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`routing reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }
    const graph = JSON.parse(txt);
    if (Number(graph.skippedEdges) > 0) {
      setStatus(`routing loaded with warnings: ${graph.status}`, 'warn');
    } else if (successMessage) {
      setStatus(successMessage, 'ok');
    }
    return true;
  } catch (err) {
    setStatus(`routing reload error: ${err}`, 'warn');
    return false;
  }
}

async function saveRouting() {
  if (state.sceneRoutingConflict) {
    setStatus('A scene changed the live setup. Download your wire draft, then load active routing before saving.', 'warn');
    return false;
  }
  setStatus('saving routing...');
  try {
    const savedEdges = [...state.edges, ...state.dormantEdges].map((edge) => ({ ...edge }));
    const res = await apiFetch(routingPath, {
      method: 'PUT',
      headers: { 'Content-Type': 'text/plain' },
      body: formatRoutingText(),
    });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`routing save failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    const reloaded = await reloadRoutingFast('routing saved and reloaded');
    if (!reloaded) {
      return false;
    }

    routingHistory.saved(savedEdges);
    return true;
  } catch (err) {
    setStatus(`routing save error: ${err}`, 'warn');
    return false;
  }
}

async function loadVersion() {
  try {
    const res = await apiFetch('/api/version', { method: 'GET' });
    const txt = (await res.text()).trim();
    if (!res.ok || !txt) {
      return;
    }
    appVersionEl.textContent = `version ${txt}`;
  } catch (err) {
  }
}

function renderResourceMeter(id, value, max, warningAt, criticalAt, valueText) {
  const meter = document.getElementById(id);
  if (!meter) return;
  if (!Number.isFinite(value) || !Number.isFinite(max) || max <= 0) {
    meter.hidden = true;
    return;
  }
  const bounded = Math.max(0, Math.min(max, value));
  const percent = bounded / max * 100;
  const palettes = {
    'sysinfo-temperature-meter': [[0, 53, 190, 151], [66.7, 53, 190, 151], [83.3, 241, 195, 72], [100, 239, 79, 91]],
    'sysinfo-memory-meter': [[0, 53, 190, 151], [70, 53, 190, 151], [85, 241, 195, 72], [100, 239, 79, 91]],
    'sysinfo-storage-meter': [[0, 53, 190, 151], [65, 53, 190, 151], [75, 241, 195, 72], [90, 239, 79, 91], [100, 239, 79, 91]],
    'sysinfo-load-meter': [[0, 53, 190, 151], [60, 53, 190, 151], [80, 241, 195, 72], [100, 239, 79, 91]],
    'sysinfo-cpu-utilization-meter': [[0, 53, 190, 151], [65, 53, 190, 151], [80, 241, 195, 72], [100, 239, 79, 91]],
  };
  const stops = palettes[id];
  if (stops) {
    const position = percent;
    let upper = stops.findIndex(([stop]) => stop >= position);
    if (upper < 0) upper = stops.length - 1;
    const lower = Math.max(0, upper - 1);
    const [lowerAt, ...lowerRgb] = stops[lower];
    const [upperAt, ...upperRgb] = stops[upper];
    const amount = upperAt === lowerAt ? 0 : (position - lowerAt) / (upperAt - lowerAt);
    const rgb = lowerRgb.map((channel, index) => Math.round(channel + (upperRgb[index] - channel) * amount));
    const rgbCss = `rgb(${rgb.join(', ')})`;
    meter.style.setProperty('--meter-color', rgbCss);
    meter.style.setProperty('--meter-color-soft',
      `rgb(${rgb.map((channel) => Math.round(channel + (255 - channel) * 0.28)).join(', ')})`);
    meter.style.setProperty('--meter-glow', `rgba(${rgb.join(', ')}, 0.28)`);
  }
  meter.style.setProperty('--meter-level', `${percent}%`);
  meter.dataset.state = bounded >= criticalAt ? 'critical' : bounded >= warningAt ? 'warning' : 'normal';
  meter.setAttribute('aria-valuenow', String(Math.round(bounded * 10) / 10));
  meter.setAttribute('aria-valuetext', valueText || `${Math.round(percent)}%`);
  meter.hidden = false;
}

function calculateCpuUtilization(previous, current) {
  if (!previous || !current
      || !Number.isSafeInteger(previous.total_ticks) || !Number.isSafeInteger(previous.idle_ticks)
      || !Number.isSafeInteger(current.total_ticks) || !Number.isSafeInteger(current.idle_ticks)) return null;
  const totalDelta = current.total_ticks - previous.total_ticks;
  const idleDelta = current.idle_ticks - previous.idle_ticks;
  if (totalDelta <= 0 || idleDelta < 0 || idleDelta > totalDelta) return null;
  return Math.max(0, Math.min(100, (totalDelta - idleDelta) / totalDelta * 100));
}

function renderStorageUsage(storage) {
  const value = document.getElementById('sysinfo-storage');
  const free = document.getElementById('sysinfo-storage-free');
  const fields = ['total_bytes', 'used_bytes', 'free_bytes', 'reserved_bytes'];
  if (!storage || !fields.every((key) => Number.isSafeInteger(storage[key]) && storage[key] >= 0)
      || storage.total_bytes === 0
      || storage.used_bytes + storage.free_bytes + storage.reserved_bytes !== storage.total_bytes) {
    value.textContent = 'Unavailable';
    renderResourceMeter('sysinfo-storage-meter', NaN, 100, 70, 90);
    free.textContent = 'Storage usage could not be read.';
    return;
  }
  const format = (bytes) => bytes >= 1024 ** 3
    ? `${(bytes / 1024 ** 3).toFixed(1)} GiB`
    : `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  value.textContent = `${format(storage.used_bytes)} / ${format(storage.total_bytes)} used`;
  const occupiedPercent = (storage.total_bytes - storage.free_bytes) / storage.total_bytes * 100;
  renderResourceMeter('sysinfo-storage-meter', occupiedPercent, 100, 70, 90,
    `${occupiedPercent.toFixed(1)}% used or reserved`);
  free.textContent = `${format(storage.free_bytes)} free`
    + (storage.reserved_bytes > 0 ? ` (${format(storage.reserved_bytes)} reserved)` : '');
}

async function loadSystemInfo() {
  try {
    const data = await AudioxUI.probeDevice();

    const vEl = document.getElementById('sysinfo-version');
    const kEl = document.getElementById('sysinfo-kernel');
    const modelEl = document.getElementById('sysinfo-model');
    const cpuClockEl = document.getElementById('sysinfo-cpu-clock');
    const temperatureEl = document.getElementById('sysinfo-temperature');
    const temperatureMeter = document.getElementById('sysinfo-temperature-meter');
    state.pi5FanIsPi5 = data.board === 'pi5';
    const uEl = document.getElementById('sysinfo-uptime');
    const mEl = document.getElementById('sysinfo-memory');
    const lEl = document.getElementById('sysinfo-load');
    const fEl = document.getElementById('sysinfo-ffmpeg');
    const audioBufferEl = document.getElementById('sysinfo-audio-buffer');
    const audioBufferDetailEl = document.getElementById('sysinfo-audio-buffer-detail');
    const stressPanel = document.getElementById('system-audio-stress');
    const stressStatus = document.getElementById('audio-stress-status');
    const stressAudioButton = document.getElementById('btn-audio-stress-thread');
    const stressCpuButton = document.getElementById('btn-audio-stress-cpu');
    const stressStopButton = document.getElementById('btn-audio-stress-stop');
    if (!vEl) return;

    vEl.textContent = data.version ? `v${data.version}` : '\u2014';
    kEl.textContent = data.kernel || '\u2014';
    if (modelEl) {
      modelEl.textContent = data.model || '\u2014';
    }
    if (cpuClockEl) {
      const cpuFreq = Number(data.cpu_freq_mhz || 0);
      cpuClockEl.textContent = cpuFreq > 0 ? `${cpuFreq} MHz` : '\u2014';
    }
    if (temperatureEl && temperatureMeter) {
      const temperature = Number(data.temperature_c);
      if (data.temperature_c !== null && data.temperature_c !== undefined
          && Number.isFinite(temperature) && temperature >= 0 && temperature <= 150) {
        temperatureEl.textContent = `${temperature.toFixed(1)} °C`;
        renderResourceMeter('sysinfo-temperature-meter', temperature, 90, 60, 75,
          `${temperature.toFixed(1)} degrees Celsius`);
      } else {
        temperatureEl.textContent = 'Unavailable';
        renderResourceMeter('sysinfo-temperature-meter', NaN, 90, 60, 75);
      }
    }
    const secs = Number(data.uptime_secs || 0);
    const d = Math.floor(secs / 86400);
    const h = Math.floor((secs % 86400) / 3600);
    const m = Math.floor((secs % 3600) / 60);
    uEl.textContent = d > 0 ? `${d}d ${h}h ${m}m` : `${h}h ${m}m`;

    const totalMb = Number(data.mem_total_mb || 0);
    const availMbRaw = Number(data.mem_avail_mb || 0);
    const availMb = Math.max(0, Math.min(totalMb, availMbRaw));
    const usedMb = Math.max(0, totalMb - availMb);
    if (Number.isFinite(totalMb) && totalMb > 0 && Number.isFinite(availMbRaw)) {
      mEl.textContent = `${usedMb} / ${totalMb} MB`;
      const memoryPercent = usedMb / totalMb * 100;
      renderResourceMeter('sysinfo-memory-meter', memoryPercent, 100, 75, 90,
        `${memoryPercent.toFixed(1)}% of memory used`);
    } else {
      mEl.textContent = 'Unavailable';
      renderResourceMeter('sysinfo-memory-meter', NaN, 100, 75, 90);
    }
    renderStorageUsage(data.storage);

    const cpuSample = data.cpu_times;
    const utilization = calculateCpuUtilization(state.cpuTimesSample, cpuSample);
    state.cpuTimesSample = cpuSample && Number.isSafeInteger(cpuSample.total_ticks)
      && Number.isSafeInteger(cpuSample.idle_ticks) ? cpuSample : null;
    const cpuUtilization = document.getElementById('sysinfo-cpu-utilization');
    if (Number.isFinite(utilization)) {
      cpuUtilization.textContent = `${utilization.toFixed(1)}%`;
      renderResourceMeter('sysinfo-cpu-utilization-meter', utilization, 100, 70, 90,
        `${utilization.toFixed(1)}% CPU utilization`);
    } else {
      cpuUtilization.textContent = state.cpuTimesSample ? 'Sampling…' : 'Unavailable';
      renderResourceMeter('sysinfo-cpu-utilization-meter', NaN, 100, 70, 90);
    }

    const load = Number(data.load1);
    const onlineCpus = Number(data.cpu_count);
    if (Number.isFinite(load) && load >= 0) {
      lEl.textContent = load.toFixed(2);
      if (Number.isFinite(onlineCpus) && onlineCpus > 0) {
        const loadPercent = load / onlineCpus * 100;
        renderResourceMeter('sysinfo-load-meter', loadPercent, 100, 70, 100,
          `${load.toFixed(2)} load average relative to ${onlineCpus} online CPUs`);
      } else {
        renderResourceMeter('sysinfo-load-meter', NaN, 100, 70, 100);
      }
    } else {
      lEl.textContent = 'Unavailable';
      renderResourceMeter('sysinfo-load-meter', NaN, 100, 70, 100);
    }

    if (fEl) {
      fEl.textContent = data.ffmpeg || '\u2014';
    }
    if (audioBufferEl && audioBufferDetailEl) {
      const buffer = data.audio_buffer;
      const base = Number(buffer?.base_block_frames);
      const target = Number(buffer?.target_period_frames);
      const period = Number(buffer?.largest_period_frames);
      const largest = Number(buffer?.largest_buffer_frames);
      const streams = Number(buffer?.active_streams);
      const latency = Number(buffer?.estimated_largest_buffer_ms);
      const xruns = Number(data.audio_health?.xruns);
      const downshiftDelay = Number(buffer?.downshift_delay_ms);
      const downshiftFailures = Number(buffer?.downshift_failures);
      const downshiftDelaySeconds = Number.isFinite(downshiftDelay)
        ? Math.round(downshiftDelay / 1000) : 0;
      const downshiftFailureCount = Number.isFinite(downshiftFailures) ? downshiftFailures : 0;
      if (Number.isFinite(base) && base > 0 && Number.isFinite(target) && target > 0) {
        audioBufferEl.textContent = target > base
          ? `${target} frames (adaptive)`
          : `${target} frames (base)`;
        audioBufferDetailEl.textContent = streams > 0 && Number.isFinite(largest) && largest > 0
          ? `Period ${Number.isFinite(period) && period > 0 ? period : '—'} · buffer ${largest} frames (~${latency.toFixed(1)} ms capacity) · ${streams} streams · ${Number.isFinite(xruns) ? xruns : '—'} X-runs${target > base ? ` · downshift after ${downshiftDelaySeconds}s quiet · ${downshiftFailureCount} failed downshifts` : ''}`
          : `No active streams · ${Number.isFinite(xruns) ? xruns : '—'} X-runs`;
      } else {
        audioBufferEl.textContent = 'Unavailable';
        audioBufferDetailEl.textContent = 'Audio buffer status could not be read.';
      }
    }
    showAudioHealth(data);
    window.AudioxSystemMonitor?.update(data, utilization);
    if (stressPanel) {
      const stress = data.audio_stress_test;
      stressPanel.hidden = stress?.available !== true;
      if (stressStatus && stress) {
        stressStatus.textContent = stress.mode === 'idle'
          ? 'No stress test running.'
          : `${stress.mode === 'audio' ? 'Audio-thread overload' : 'System CPU contention'} active; ${Number(stress.remaining_ms || 0)} ms remain${stress.mode === 'cpu' ? ` (${Number(stress.workers || 0)} workers)` : ''}.`;
      }
      const running = stress?.mode !== 'idle';
      if (stressAudioButton) stressAudioButton.disabled = running;
      if (stressCpuButton) stressCpuButton.disabled = running;
      if (stressStopButton) stressStopButton.disabled = !running;
    }
    AudioxUI.resolve('system-info');
  } catch (err) {
    renderStorageUsage(null);
    window.AudioxSystemMonitor?.unavailable();
    setStatus(`device info load error: ${err}`, 'warn', 'system-info');
  }
}

function validFanCurve(settings) {
  if (!settings || !Array.isArray(settings.temperatures) || settings.temperatures.length !== 4
      || !Array.isArray(settings.speeds) || settings.speeds.length !== 4
      || !Number.isInteger(settings.hysteresis) || settings.hysteresis < 1 || settings.hysteresis > 15) {
    return false;
  }
  for (let i = 0; i < 4; i += 1) {
    if (!Number.isInteger(settings.temperatures[i]) || settings.temperatures[i] < 40 || settings.temperatures[i] > 80
        || !Number.isInteger(settings.speeds[i]) || settings.speeds[i] < 1 || settings.speeds[i] > 255) {
      return false;
    }
    if (i > 0 && (settings.temperatures[i] - settings.temperatures[i - 1] <= settings.hysteresis
        || settings.speeds[i] <= settings.speeds[i - 1])) return false;
  }
  return typeof settings.enabled === 'boolean';
}

function renderPi5FanCurve() {
  const { temperatures, speeds, hysteresis, enabled } = state.pi5FanEditor;
  const svg = document.getElementById('pi5-fan-graph');
  const save = document.getElementById('btn-pi5-fan-save');
  const enabledControl = document.getElementById('pi5-fan-enabled');
  const hysteresisControl = document.getElementById('pi5-fan-hysteresis');
  const summary = document.getElementById('pi5-fan-curve-summary');
  if (!svg || !save || !enabledControl || !hysteresisControl || !summary) return;

  const ready = !state.pi5FanIsPi5 || state.pi5FanSettingsLoaded;
  const left = 82, right = 724, top = 38, bottom = 334;
  const x = (temperature) => left + (temperature - 40) / 40 * (right - left);
  const y = (speed) => bottom - speed / 255 * (bottom - top);
  const points = temperatures.map((temperature, index) => ({ x: x(temperature), y: y(speeds[index]) }));
  const line = points.map((point, index) => `${index ? 'L' : 'M'} ${point.x} ${point.y}`).join(' ');
  const area = `${line} L ${points[3].x} ${bottom} L ${points[0].x} ${bottom} Z`;
  const verticalGrid = [40, 50, 60, 70, 80].map((temperature) => {
    const position = x(temperature);
    return `<line class="fan-gridline" x1="${position}" y1="${top}" x2="${position}" y2="${bottom}"/><text x="${position}" y="${bottom + 25}" text-anchor="middle">${temperature}°</text>`;
  }).join('');
  const horizontalGrid = [0, 64, 128, 192, 255].map((speed) => {
    const position = y(speed);
    return `<line class="fan-gridline" x1="${left}" y1="${position}" x2="${right}" y2="${position}"/><text x="${left - 14}" y="${position + 4}" text-anchor="end">${speed}</text>`;
  }).join('');
  const handles = points.map((point, index) => `
    <g class="fan-point${enabled ? '' : ' is-disabled'}" data-step="${index}" tabindex="${enabled && ready ? '0' : '-1'}" role="slider"
      aria-label="Fan curve step ${index + 1}; use left and right arrows to change temperature, up and down to change speed"
      aria-valuemin="40" aria-valuemax="80" aria-valuenow="${temperatures[index]}"
      aria-valuetext="${temperatures[index]} degrees Celsius, fan PWM ${speeds[index]} of 255">
      <title>Step ${index + 1}: ${temperatures[index]} °C, fan speed ${speeds[index]} / 255</title>
      <circle class="fan-point-halo" cx="${point.x}" cy="${point.y}" r="22"/>
      <circle class="fan-point-ring" cx="${point.x}" cy="${point.y}" r="11"/>
      <circle class="fan-point-core" cx="${point.x}" cy="${point.y}" r="4"/>
      <text class="fan-point-label" x="${point.x}" y="${point.y - 20}" text-anchor="middle">${temperatures[index]}° · ${speeds[index]}</text>
    </g>`).join('');
  svg.innerHTML = `
    <defs>
      <linearGradient id="fan-line-gradient" x1="0" x2="1" y1="0" y2="0">
        <stop offset="0%" stop-color="#55d6a5"/><stop offset="55%" stop-color="#e7d35d"/><stop offset="100%" stop-color="#f06b68"/>
      </linearGradient>
      <linearGradient id="fan-area-gradient" x1="0" x2="0" y1="0" y2="1">
        <stop offset="0%" stop-color="#f07167" stop-opacity=".24"/><stop offset="100%" stop-color="#53cda0" stop-opacity=".015"/>
      </linearGradient>
    </defs>
    <text x="20" y="22">FAN SPEED · PWM</text>${horizontalGrid}${verticalGrid}
    <line class="fan-axis" x1="${left}" y1="${bottom}" x2="${right}" y2="${bottom}"/>
    <line class="fan-axis" x1="${left}" y1="${top}" x2="${left}" y2="${bottom}"/>
    <path class="fan-area" d="${area}"/><path class="fan-line" d="${line}"/>
    ${handles}
    <text x="${(left + right) / 2}" y="396" text-anchor="middle">CPU TEMPERATURE · °C</text>`;
  svg.classList.toggle('is-disabled', !enabled || !ready);
  svg.setAttribute('aria-disabled', String(!enabled || !ready));
  enabledControl.checked = enabled;
  enabledControl.disabled = state.pi5FanIsPi5 && !ready;
  hysteresisControl.value = String(hysteresis);
  const maxHysteresis = Math.min(15, ...temperatures.slice(1).map((value, index) => value - temperatures[index] - 1));
  hysteresisControl.max = String(Math.max(1, maxHysteresis));
  hysteresisControl.disabled = !enabled || !ready;
  document.getElementById('pi5-fan-hysteresis-value').textContent = `${hysteresis} °C`;
  summary.replaceChildren(...temperatures.map((temperature, index) => {
    const row = document.createElement('div');
    row.className = 'fan-curve-summary-row';
    const label = document.createElement('span');
    label.textContent = `Step ${index + 1}`;
    const value = document.createElement('strong');
    value.textContent = `${temperature} °C · PWM ${speeds[index]}`;
    row.append(label, value);
    return row;
  }));
  save.disabled = !state.pi5FanIsPi5 || !ready;
}

function editPi5FanPoint(index, temperatureDelta, speedDelta, keepFocus = false) {
  const curve = state.pi5FanEditor;
  if (!curve.enabled || (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded)) return;
  const minTemperature = index === 0 ? 40 : curve.temperatures[index - 1] + curve.hysteresis + 1;
  const maxTemperature = index === 3 ? 80 : curve.temperatures[index + 1] - curve.hysteresis - 1;
  const minSpeed = index === 0 ? 1 : curve.speeds[index - 1] + 1;
  const maxSpeed = index === 3 ? 255 : curve.speeds[index + 1] - 1;
  curve.temperatures[index] = Math.max(minTemperature,
    Math.min(maxTemperature, curve.temperatures[index] + temperatureDelta));
  curve.speeds[index] = Math.max(minSpeed, Math.min(maxSpeed, curve.speeds[index] + speedDelta));
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
  if (keepFocus) document.querySelector(`#pi5-fan-graph [data-step="${index}"]`)?.focus();
}

function updatePi5FanPointFromPointer(event, index) {
  if (!state.pi5FanEditor.enabled || (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded)) return;
  const svg = document.getElementById('pi5-fan-graph');
  const matrix = svg.getScreenCTM();
  if (!matrix) return;
  const pointer = svg.createSVGPoint();
  pointer.x = event.clientX;
  pointer.y = event.clientY;
  const position = pointer.matrixTransform(matrix.inverse());
  const left = 82, right = 724, top = 38, bottom = 334;
  const temperature = Math.round(40 + (position.x - left) / (right - left) * 40);
  const speed = Math.round((bottom - position.y) / (bottom - top) * 255);
  const curve = state.pi5FanEditor;
  const minTemperature = index === 0 ? 40 : curve.temperatures[index - 1] + curve.hysteresis + 1;
  const maxTemperature = index === 3 ? 80 : curve.temperatures[index + 1] - curve.hysteresis - 1;
  const minSpeed = index === 0 ? 1 : curve.speeds[index - 1] + 1;
  const maxSpeed = index === 3 ? 255 : curve.speeds[index + 1] - 1;
  const nextTemperature = Math.max(minTemperature, Math.min(maxTemperature, temperature));
  const nextSpeed = Math.max(minSpeed, Math.min(maxSpeed, speed));
  if (nextTemperature === curve.temperatures[index] && nextSpeed === curve.speeds[index]) return;
  curve.temperatures[index] = nextTemperature;
  curve.speeds[index] = nextSpeed;
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
}

async function loadPi5FanSettings() {
  if (!state.pi5FanIsPi5 || state.pi5FanSettingsLoading) return;
  state.pi5FanSettingsLoading = true;
  const status = document.getElementById('pi5-fan-status');
  status.textContent = 'Loading saved fan curve…';
  renderPi5FanCurve();
  try {
    const response = await apiFetch('/api/system/fan', { cache: 'no-store' });
    const text = (await response.text()).trim();
    if (!response.ok) throw new Error(`${response.status} ${text}`);
    const settings = JSON.parse(text);
    if (!validFanCurve(settings)) throw new Error('device returned an invalid fan curve');
    state.pi5FanEditor = {
      enabled: settings.enabled,
      temperatures: settings.temperatures,
      speeds: settings.speeds,
      hysteresis: settings.hysteresis,
    };
    state.pi5FanSettingsLoaded = true;
    AudioxUI.setUnsaved('pi5-fan', false);
    status.textContent = settings.enabled
      ? 'Saved fan curve loaded. Save changes, then restart to apply.'
      : 'No AudioX-managed curve is set. Firmware defaults or other boot-config fan settings remain active.';
    renderPi5FanCurve();
  } catch (error) {
    status.textContent = `Could not load Pi 5 fan settings: ${error.message}. Close and reopen to retry.`;
  } finally {
    state.pi5FanSettingsLoading = false;
    renderPi5FanCurve();
  }
}

function openPi5FanDialog() {
  const dialog = document.getElementById('pi5-fan-dialog');
  const warning = document.getElementById('pi5-fan-warning');
  const status = document.getElementById('pi5-fan-status');
  warning.hidden = state.pi5FanIsPi5;
  if (state.pi5FanIsPi5) {
    status.textContent = state.pi5FanSettingsLoaded
      ? status.textContent : 'Loading saved fan curve…';
  } else {
    status.textContent = 'Preview mode · saving disabled';
  }
  renderPi5FanCurve();
  dialog.showModal();
  if (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded) loadPi5FanSettings();
}

async function loadLegacyConfigStatus() {
  const notice = document.getElementById('legacy-config-notice');
  if (!notice) return;
  try {
    const response = await apiFetch('/api/config/legacy');
    if (!response.ok) throw new Error(`${response.status} ${(await response.text()).trim()}`);
    const data = await response.json();
    notice.hidden = data.legacy_entries === 0;
  } catch (error) {
    setStatus(`legacy configuration check failed: ${error}`, 'warn');
  }
}

async function deleteLegacyConfig() {
  const accepted = await AudioxUI.confirm(
    'Delete the retained legacy configuration and old script data? The active configuration is unchanged. Back up first. Older firmware will no longer have its original settings, and rollback may require a compatible backup.', {
      label: 'Delete legacy data', danger: true,
    });
  if (!accepted) return;
  const button = document.getElementById('btn-delete-legacy-config');
  button.disabled = true;
  try {
    const response = await apiFetch('/api/config/legacy', { method: 'DELETE' });
    if (!response.ok) throw new Error(`${response.status} ${(await response.text()).trim()}`);
    await loadLegacyConfigStatus();
    setStatus('legacy configuration deleted; active settings preserved', 'ok');
  } catch (error) {
    setStatus(`legacy configuration cleanup failed: ${error}`, 'warn');
  } finally {
    button.disabled = false;
  }
}

async function loadConfig() {
  setStatus('loading config...');
  try {
    let res = await apiFetch(configPath, { method: 'GET' });
    let txt = await res.text();
    if (!res.ok) {
      res = await apiFetch(realConfigPath, { method: 'GET' });
      txt = await res.text();
      if (!res.ok) {
        setStatus(`config load failed: ${res.status} ${txt.trim()}`, 'warn');
        return null;
      }
    }

    const cfg = parseConfigText(txt);
    setFormFromConfig(cfg);
    AudioxUI.setUnsaved('usb-config', false);
    setStatus('config loaded', 'ok');
    return cfg;
  } catch (err) {
    setStatus(`config load error: ${err}`, 'warn');
    return null;
  }
}

function renderFramebufferView() {
  const button = document.getElementById('btn-fb-view');
  const active = state.framebufferView === 'soundboard';
  button.textContent = active ? 'Show Meters on Display' : 'Show Soundboard on Display';
  button.setAttribute('aria-pressed', String(active));
}

async function loadFramebufferView() {
  try {
    const res = await apiFetch('/api/framebuffer/view');
    if (!res.ok) throw new Error(`${res.status}`);
    const data = await res.json();
    state.framebufferView = data.view === 'soundboard' ? 'soundboard' : 'meters';
    renderFramebufferView();
  } catch (err) {
    setStatus(`display mode load failed: ${err}`, 'warn');
  }
}

async function toggleFramebufferView() {
  const view = state.framebufferView === 'soundboard' ? 'meters' : 'soundboard';
  const button = document.getElementById('btn-fb-view');
  button.disabled = true;
  try {
    const res = await apiFetch('/api/framebuffer/view', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `view=${view}`,
    });
    if (!res.ok) throw new Error(`${res.status} ${(await res.text()).trim()}`);
    state.framebufferView = view;
    renderFramebufferView();
    setStatus(`display: ${view}`, 'ok');
  } catch (err) {
    setStatus(`display mode save failed: ${err}`, 'warn');
  } finally {
    button.disabled = false;
  }
}

async function saveConfig() {
  const cfg = getConfigFromForm();
  const payload = formatConfigText(cfg);
  setStatus('saving config...');

  try {
    const res = await apiFetch(configPath, {
      method: 'PUT',
      headers: { 'Content-Type': 'text/plain' },
      body: payload,
    });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`config save failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    setStatus('config saved', 'ok');
    AudioxUI.setUnsaved('usb-config', false);
    return true;
  } catch (err) {
    setStatus(`config save error: ${err}`, 'warn');
    return false;
  }
}

let editingMacroId = null;
const defaultScriptSource = 'hook("run", function(event)\n  log("Hello from AudioX")\nend)\n';
let macroCount = 0;
const scriptControlEdits = new Set();

function scriptListReloadSafe(preserveEditor) {
  return !(preserveEditor && AudioxUI.hasUnsavedChanges('macro-editor')) &&
    !Array.from(scriptControlEdits).some(control => control.pending || control.dirty);
}

async function loadMidiChords({ preserveEditor = false } = {}) {
  if (!scriptListReloadSafe(preserveEditor)) return;
  try {
    const res = await apiFetch('/api/scripts', { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const data = await res.json();
    if (scriptListReloadSafe(preserveEditor)) renderMidiChords(data.scripts);
  } catch (err) {
    setStatus(`Script load failed: ${err}`, 'warn', 'midi-chords');
  }
}

function renderMidiChords(chords) {
  macroCount = chords.length;
  const list = document.getElementById('chord-list');
  list.replaceChildren();
  scriptControlEdits.clear();
  for (const chord of chords) {
    const row = document.createElement('div');
    const label = document.createElement('span');
    label.textContent = `${chord.name} — ${chord.midi_enabled ? `Channel ${chord.channel}, note ${chord.note}` : 'Event hooks'} ` +
      `(LED idle ${chord.idle_velocity ?? 0}, pressed ${chord.pressed_velocity ?? 0})`;
    if (chord.runtime_error_hex) {
      label.textContent += ` — Last execution failed: ${chordHexDecode(chord.runtime_error_hex)}`;
    }
    if (chord.ui_error_hex) {
      label.textContent += ` — Script controls/settings load failed: ${chordHexDecode(chord.ui_error_hex)}`;
    }
    const edit = document.createElement('button');
    edit.textContent = 'Edit';
    edit.addEventListener('click', async () => {
      if (AudioxUI.hasUnsavedChanges('macro-editor') && !await AudioxUI.confirm(
        'Discard unsaved script changes and edit this saved script?', {
          title: 'Discard script changes?', label: 'Discard and edit',
        })) return;
      cancelChordLearn();
      editingMacroId = chord.id;
      document.getElementById('macro-name').value = chord.name;
      document.getElementById('macro-midi-enabled').checked = chord.midi_enabled;
      document.getElementById('chord-channel').value = chord.channel;
      document.getElementById('chord-note').value = chord.note;
      AudioxLuaEditor.setValue(document.getElementById('chord-script'), chordHexDecode(chord.script_hex || ''));
      document.getElementById('macro-config-write').checked = Boolean(chord.config_write_allowed);
      document.getElementById('chord-idle-velocity').value = chord.idle_velocity ?? 0;
      document.getElementById('chord-pressed-velocity').value = chord.pressed_velocity ?? 0;
      updateMacroBinding();
      AudioxUI.setUnsaved('macro-editor', false);
      document.getElementById('btn-chord-save').textContent = 'Save Changes';
      document.getElementById('macro-name').focus();
    });
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.addEventListener('click', async () => {
      if (!await AudioxUI.confirm(`Delete the saved script "${chord.name}"? This cannot be undone.`, {
        title: 'Delete script?', label: 'Delete script',
      })) return;
      await saveMidiChord('DELETE', chord, remove);
    });
    const run = document.createElement('button');
    run.textContent = 'Run';
    run.disabled = !(chord.event_mask & 128);
    if (run.disabled) run.title = 'Add hook("run", function(event) ... end) to enable Run';
    run.addEventListener('click', () => runMacro(chord.id, run));
    row.className = 'stack';
    const actions = document.createElement('div');
    actions.className = 'macro-actions';
    actions.append(edit, remove, run);
    row.append(label, actions);
    if (Array.isArray(chord.ui) && chord.ui.length) {
      const controls = document.createElement('div');
      controls.className = 'script-controls';
      for (const descriptor of chord.ui) {
        if (!descriptor || typeof descriptor.key !== 'string' ||
            !['boolean', 'number', 'string', 'button'].includes(descriptor.type)) continue;
        const field = document.createElement('label');
        field.className = 'script-control';
        const input = document.createElement(descriptor.type === 'button' ? 'button' : 'input');
        const caption = String(descriptor.label ?? descriptor.key);
        if (descriptor.type === 'button') {
          input.type = 'button';
          input.textContent = caption;
        } else {
          const text = document.createElement('span');
          text.textContent = caption;
          field.append(text);
          input.type = { boolean: 'checkbox', number: 'number', string: 'text' }[descriptor.type];
          if (descriptor.type === 'number') {
            input.step = 'any';
            for (const bound of ['min', 'max']) {
              if (typeof descriptor[bound] === 'number' && Number.isFinite(descriptor[bound])) {
                input[bound] = String(descriptor[bound]);
              }
            }
          } else if (descriptor.type === 'string') {
            input.maxLength = 127;
            input.title = 'Up to 127 UTF-8 bytes';
          }
        }
        const control = { input, descriptor, savedValue: descriptor.value, pending: false, dirty: false };
        scriptControlEdits.add(control);
        restoreScriptControl(control);
        input.addEventListener('input', () => { control.dirty = true; });
        input.addEventListener(descriptor.type === 'button' ? 'click' : 'change',
          () => saveScriptControl(chord.id, control));
        field.append(input);
        controls.append(field);
      }
      row.append(controls);
    }
    list.appendChild(row);
  }
}

function restoreScriptControl(control) {
  if (control.descriptor.type === 'boolean') control.input.checked = Boolean(control.savedValue);
  else if (control.descriptor.type !== 'button') control.input.value = String(control.savedValue);
  control.dirty = false;
}

async function saveScriptControl(id, control) {
  if (control.pending || control.input.disabled) return;
  const { input, descriptor } = control;
  control.pending = true;
  input.disabled = true;
  let saved = false;
  try {
    let value;
    if (descriptor.type === 'boolean') value = input.checked;
    else if (descriptor.type === 'number') {
      value = Number(input.value);
      if (!String(input.value).trim() || !Number.isFinite(value) ||
          (input.min !== undefined && input.min !== '' && value < Number(input.min)) ||
          (input.max !== undefined && input.max !== '' && value > Number(input.max))) {
        throw new Error('Enter a finite number within the displayed bounds');
      }
    } else if (descriptor.type === 'string') {
      value = input.value;
      if (new TextEncoder().encode(value).length > 127) {
        throw new Error('Text must be at most 127 UTF-8 bytes');
      }
    }
    const body = `id=${id}&key_hex=${chordHexEncode(descriptor.key)}` +
      (descriptor.type === 'button' ? '' : `&value_hex=${chordHexEncode(String(value))}`);
    const res = await apiFetch('/api/scripts/control', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' }, body,
    });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const result = await res.text();
    control.savedValue = value;
    descriptor.value = value;
    control.dirty = false;
    saved = true;
    setStatus(descriptor.type === 'button' ?
      'Script button queued (not confirmation of completion)' :
      result.includes('config_change queued') ?
        'Script setting saved; config_change queued (not confirmation of completion)' :
        'Script setting saved', 'ok', 'midi-chords');
  } catch (err) {
    restoreScriptControl(control);
    setStatus(`Script control failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    control.pending = false;
    input.disabled = false;
  }
  if (saved) await loadMidiChords({ preserveEditor: true });
}

async function saveMidiChord(method, chord, button) {
  if (button.disabled) return;
  button.disabled = true;
  try {
    const activityScript = method === 'POST' &&
      /\bdevice_active\b/.test(chordHexDecode(chord.script_hex || ''));
    if (activityScript && !await AudioxUI.confirm(
      'Device activity hooks can run ten times per second per device. Repeated Lua evaluation can cause significant CPU usage and compete with audio processing. Keep callbacks small. Save this script?', {
        title: 'High-frequency activity hook', label: 'Acknowledge and save',
      })) return;
    const configurationScript = method === 'POST' &&
      (Boolean(chord.config_write_allowed) ||
        /\b(?:config|settings|system|bluetooth)\b/.test(chordHexDecode(chord.script_hex || '')));
    if (configurationScript && !await AudioxUI.confirm(
      'This script may write official configuration, restart the system, or manage Bluetooth devices. Configuration writes can replace persistent settings, including MIDI bindings, routing and startup configuration, and whole-file writes are not format-validated. Back up your settings first; reload or restart may be required. Allow these permissions and save this script?', {
        title: 'Allow configuration and system management?', label: 'Allow and save script',
      })) return;
    const payload = method === 'DELETE' ? `id=${chord.id}` :
      (chord.id ? `id=${chord.id}&` : '') +
      `name_hex=${chordHexEncode(chord.name)}&midi_enabled=${chord.midi_enabled ? 1 : 0}` +
      `&channel=${chord.channel}&note=${chord.note}` +
      `&idle_velocity=${chord.idle_velocity ?? 0}&pressed_velocity=${chord.pressed_velocity ?? 0}` +
      `&config_write_ack=${configurationScript ? 1 : 0}` +
      `&activity_ack=${activityScript ? 1 : 0}` +
      `&script_hex=${chord.script_hex || ''}`;
    let res = await apiFetch('/api/scripts', {
      method, headers: { 'Content-Type': 'text/plain' }, body: payload,
    });
    if (method === 'POST' && res.status === 409 && !activityScript) {
      const warning = await res.text();
      if (!warning.includes('device_active')) throw new Error(`409: ${warning}`);
      if (!await AudioxUI.confirm(warning, {
        title: 'High-frequency activity hook', label: 'Acknowledge and save',
      })) return;
      res = await apiFetch('/api/scripts', {
        method, headers: { 'Content-Type': 'text/plain' },
        body: payload.replace('&activity_ack=0', '&activity_ack=1'),
      });
    }
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    if (method === 'POST' || editingMacroId === chord.id) resetMacroEditor();
    setStatus(method === 'DELETE' ? 'Script removed' : 'Script saved', 'ok', 'midi-chords');
    await loadMidiChords();
  } catch (err) {
    setStatus(`Script save failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = false;
  }
}

async function runMacro(id, button) {
  if (button.disabled) return;
  button.disabled = true;
  try {
    const res = await apiFetch('/api/scripts/run', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: `id=${id}`,
    });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    setStatus('Script queued (not confirmation of completion)', 'ok', 'midi-chords');
    await loadMidiChords();
  } catch (err) {
    setStatus(`Script run failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = false;
  }
}

function updateMacroBinding() {
  const enabled = document.getElementById('macro-midi-enabled').checked;
  if (!enabled) cancelChordLearn();
  document.getElementById('btn-chord-learn').disabled = !enabled;
  document.getElementById('macro-midi-binding').hidden = !enabled;
}

function resetMacroEditor() {
  cancelChordLearn();
  editingMacroId = null;
  document.getElementById('macro-name').value = '';
  document.getElementById('macro-midi-enabled').checked = false;
  document.getElementById('chord-channel').value = '0';
  document.getElementById('chord-note').value = '60';
  AudioxLuaEditor.setValue(document.getElementById('chord-script'), defaultScriptSource);
  document.getElementById('macro-config-write').checked = false;
  document.getElementById('chord-idle-velocity').value = '0';
  document.getElementById('chord-pressed-velocity').value = '0';
  document.getElementById('btn-chord-save').textContent = 'Save Script';
  AudioxUI.setUnsaved('macro-editor', false);
  updateMacroBinding();
}

function chordHexEncode(text) {
  return Array.from(new TextEncoder().encode(text), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function chordHexDecode(hex) {
  return new TextDecoder().decode(Uint8Array.from(hex.match(/../g) || [], (byte) => parseInt(byte, 16)));
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopMeterPolling();
  else if (state.activeTab === 'volume') startMeterPolling();
  if (document.hidden) {
    cancelChordLearn();
    effectsManager?.closeCustomUis();
  }
});

function cancelChordLearn() {
  if (state.mappingAssignTarget !== '__macro__') return;
  state.mappingAssignTarget = null;
  if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
  state.mappingPollTimer = null;
  document.getElementById('btn-chord-learn').setAttribute('aria-pressed', 'false');
  document.getElementById('chord-learn-status').textContent = 'MIDI learning stopped.';
}

document.getElementById('btn-chord-learn').addEventListener('click', async () => {
  if (!document.getElementById('macro-midi-enabled').checked) return;
  if (state.mappingAssignTarget === '__macro__') {
    cancelChordLearn();
    return;
  }
  const button = document.getElementById('btn-chord-learn');
  if (button.disabled) return;
  button.disabled = true;
  state.mappingAssignTarget = null;
  if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
  state.mappingPollTimer = null;
  try {
    const res = await apiFetch('/api/midi/last_note', { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const data = await res.json();
    if (state.activeTab !== 'scripts' || document.hidden ||
        !document.getElementById('macro-midi-enabled').checked) return;
    const seq = Number(data.last_seq);
    if (!Number.isInteger(seq) || seq < 0) throw new Error('device did not report a valid MIDI event sequence');
    state.mappingLastSeq = seq;
    state.mappingAssignTarget = '__macro__';
    if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
    button.setAttribute('aria-pressed', 'true');
    document.getElementById('chord-learn-status').textContent = 'Press a pad. Select Learn MIDI Pad again to cancel.';
  } catch (err) {
    setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = !document.getElementById('macro-midi-enabled').checked;
  }
});

document.getElementById('macro-midi-enabled').addEventListener('change', updateMacroBinding);
document.getElementById('btn-macro-new').addEventListener('click', async () => {
  if (AudioxUI.hasUnsavedChanges('macro-editor') && !await AudioxUI.confirm(
    'Discard unsaved script changes and start a new script?', {
      title: 'Discard script changes?', label: 'Discard and start new',
    })) return;
  resetMacroEditor();
});
document.getElementById('btn-chord-load').addEventListener('click', loadMidiChords);
document.getElementById('btn-chord-save').addEventListener('click', () => {
  const channel = Number(document.getElementById('chord-channel').value);
  const name = document.getElementById('macro-name').value.trim();
  const midi_enabled = document.getElementById('macro-midi-enabled').checked;
  const note = Number(document.getElementById('chord-note').value);
  const script = document.getElementById('chord-script').value;
  const idle = Number(document.getElementById('chord-idle-velocity').value);
  const pressed = Number(document.getElementById('chord-pressed-velocity').value);
  if (!name || name.includes('\0') || new TextEncoder().encode(name).length > 63 ||
      (!editingMacroId && macroCount >= 16) ||
      !Number.isInteger(channel) || channel < 0 || channel > 15 ||
      !Number.isInteger(note) || note < 0 || note > 127 ||
      !Number.isInteger(idle) || idle < 0 || idle > 127 ||
      !Number.isInteger(pressed) || pressed < 0 || pressed > 127 ||
      !script.trim() || script.includes('\0') ||
      new TextEncoder().encode(script).length > 17408) {
    setStatus('Enter a name (1-63 UTF-8 bytes), channel 0-15, note/LED values 0-127, and a Lua callback script (up to 17,408 UTF-8 bytes). Maximum 16 scripts.', 'warn', 'midi-chords');
    return;
  }
  return saveMidiChord('POST', { id: editingMacroId, name, midi_enabled, channel, note, idle_velocity: idle,
    pressed_velocity: pressed,
    config_write_allowed: script && document.getElementById('macro-config-write').checked,
    script_hex: chordHexEncode(script) }, document.getElementById('btn-chord-save'));
});

async function reloadConfig() {
  setStatus('reloading config...');
  try {
    const res = await apiFetch('/api/config/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    setStatus('config reloaded', 'ok');
    return true;
  } catch (err) {
    setStatus(`reload error: ${err}`, 'warn');
    return false;
  }
}

async function triggerSfx(fileName, action = 'trigger') {
  if (!fileName) {
    return;
  }

  const verb = action === 'press' ? 'pressing' : (action === 'release' ? 'releasing' : 'triggering');
  setStatus(`${verb} ${fileName}...`);
  try {
    const target = encodeURIComponent(fileName);
    const endpoint = (action === 'press')
      ? `/api/soundboard/press/${target}`
      : (action === 'release')
        ? `/api/soundboard/release/${target}`
        : `/api/soundboard/trigger/${target}`;
    const res = await apiFetch(endpoint, { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`error: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    if (action === 'release') {
      setStatus(`released ${fileName}`, 'ok');
    } else {
      setStatus(`triggered ${fileName}`, 'ok');
    }
  } catch (err) {
    setStatus(`request failed: ${err}`, 'warn');
  }
}

async function triggerStopAll() {
  setStatus('stopping all clips...');
  try {
    const res = await apiFetch('/api/soundboard/stop_all', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`error: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    setStatus('all clips stopped', 'ok');
  } catch (err) {
    setStatus(`request failed: ${err}`, 'warn');
  }
}

async function triggerSamplerToggle() {
  try {
    const res = await apiFetch('/api/midi/sampler_mode', { method: 'POST' });
    const text = await res.text();
    if (!res.ok) throw new Error(`${res.status} ${text.trim()}`);
    const data = JSON.parse(text);
    setStatus(`Sampler selection mode ${data.active ? 'on' : 'off'}.`, 'ok');
  } catch (error) {
    setStatus(`Sampler toggle failed: ${error}`, 'warn');
  }
}

function renderStopAllMidiStatus() {
  if (!sbStopAllMidiEl) {
    return;
  }
  sbStopAllMidiEl.textContent = (state.stopAllMidiNote === null || state.stopAllMidiNote === undefined)
    ? 'unassigned'
    : `note ${state.stopAllMidiNote}`;
}

function renderSamplerToggleMidiStatus() {
  if (!sbSamplerToggleMidiEl) {
    return;
  }
  sbSamplerToggleMidiEl.textContent = (state.samplerToggleMidiNote === null || state.samplerToggleMidiNote === undefined)
    ? 'unassigned'
    : `note ${state.samplerToggleMidiNote}`;
}

async function loadSoundboardModes() {
  try {
    const res = await apiFetch('/api/soundboard/modes', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      return;
    }
    const data = JSON.parse(txt);
    state.soundboardModesBySfx = {};
    const modes = Array.isArray(data?.modes) ? data.modes : [];
    for (const entry of modes) {
      const sfx = String(entry?.sfx || '');
      const mode = (entry?.mode === 'hold') ? 'hold' : 'play';
      if (sfx) {
        state.soundboardModesBySfx[sfx] = mode;
      }
    }
    renderSoundboardGrid();
  } catch (_) {}
}

function updateSoundboardLayoutState(data) {
  const rows = Number(data?.rows);
  const columns = Number(data?.columns);
  const corners = Array.isArray(data?.corners) && data.corners.length === 4
    ? data.corners.map((note) => Number.isInteger(note) ? note : null)
    : [null, null, null, null];
  state.soundboardLayout = {
    mode: data?.mode === 'grid' ? 'grid' : 'normal',
    rows: Number.isInteger(rows) && rows >= 1 && rows <= 8 ? rows : 4,
    columns: Number.isInteger(columns) && columns >= 1 && columns <= 8 ? columns : 4,
    corners,
    cells: Array.isArray(data?.cells) ? data.cells : [],
  };
  state.soundboardCellsBySfx = AudioxSoundboardGrid.cellsBySound(state.soundboardLayout,
    AudioxSoundboardGrid.elements(state.sfxFiles));
  const modeSelect = document.getElementById('sb-layout-mode');
  const rowsInput = document.getElementById('sb-grid-rows');
  const columnsInput = document.getElementById('sb-grid-columns');
  modeSelect.value = state.soundboardLayout.mode;
  rowsInput.value = String(state.soundboardLayout.rows);
  columnsInput.value = String(state.soundboardLayout.columns);
  document.getElementById('sb-grid-options').hidden = state.soundboardLayout.mode !== 'grid';
  document.querySelectorAll('.sb-grid-only').forEach((el) => { el.hidden = state.soundboardLayout.mode !== 'grid'; });
  const learned = state.soundboardLayout.corners
    .map((note, index) => Number.isInteger(note) ? `${['top-left', 'top-right', 'bottom-left', 'bottom-right'][index]} MIDI ${note}` : null)
    .filter(Boolean);
  document.getElementById('sb-grid-learn-status').textContent = learned.length
    ? `Learned: ${learned.join(', ')}.`
    : 'Learn the four physical pad corners to connect grid cells to MIDI notes.';
  document.getElementById('sb-order-status').textContent = state.soundboardLayout.mode === 'grid'
    ? 'Drag sounds to empty cells, onto other sounds to swap, or to Unassigned sounds. On touch, drag the sound name. The cell selector also supports keyboard placement. Learn MIDI corners to update controller pad mappings.'
    : 'Drag sound cards to reorder them (on touch, drag the sound name). With a keyboard, focus a name and press Space, use arrow keys, then Enter to save. Changes also update the device display.';
  renderSoundboardCellSelectors();
  renderSoundboardGrid();
}

async function loadSoundboardLayout() {
  try {
    const res = await apiFetch('/api/soundboard/layout', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      if (res.status === 404) {
        updateSoundboardLayoutState({ mode: 'normal', rows: 4, columns: 4 });
        return;
      }
      throw new Error(`Could not load soundboard layout: ${res.status} ${txt}`);
    }
    updateSoundboardLayoutState(JSON.parse(txt));
  } catch (error) {
    setStatus(error, 'warn', 'soundboard-layout');
  }
}

function renderSoundboardCellSelectors() {
  const layout = state.soundboardLayout;
  const uploadSelect = document.getElementById('sb-upload-cell');
  uploadSelect.replaceChildren(new Option('Unassigned', ''));
  if (layout.mode !== 'grid') return;
  const occupied = new Set(Object.values(state.soundboardCellsBySfx)
    .map((cell) => `${cell.row}:${cell.column}`));
  for (let row = 0; row < layout.rows; row += 1) {
    for (let column = 0; column < layout.columns; column += 1) {
      const option = new Option(`Row ${row + 1}, column ${column + 1}`, `${row}:${column}`);
      if (occupied.has(option.value)) {
        option.disabled = true;
        option.textContent += ' (occupied)';
      }
      uploadSelect.appendChild(option);
    }
  }
}

async function saveSoundboardLayout(fields, statusLabel = 'Soundboard layout saved.') {
  try {
    const res = await apiFetch('/api/soundboard/layout', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: new URLSearchParams(fields).toString(),
    });
    const txt = await res.text();
    if (!res.ok) throw new Error(`${res.status} ${txt.trim()}`);
    updateSoundboardLayoutState(JSON.parse(txt));
    setStatus(statusLabel, 'ok', 'soundboard-layout');
    return true;
  } catch (error) {
    setStatus(`Soundboard layout save failed: ${error}`, 'warn', 'soundboard-layout');
    await loadSoundboardLayout();
    return false;
  }
}

async function saveSoundboardGridCell(sfx, row, column) {
  if (state.soundboardGridSaving) return false;
  const current = state.soundboardCellsBySfx[sfx];
  if (current?.row === row && current?.column === column) return true;
  state.soundboardGridSaving = true;
  soundboardGridDrag.cancel();
  try {
    const action = AudioxSoundboardGrid.actionFor(sfx);
    const element = action ? { action: action.action } : { sfx };
    const label = action?.label || sfx;
    const fields = row === null ? { ...element, unassign: '1' } :
      { ...element, row: String(row), column: String(column) };
    const saved = await saveSoundboardLayout(fields, row === null ? `Unassigned ${label}.` :
      `Moved ${label} to row ${row + 1}, column ${column + 1}.`);
    if (saved) {
      await loadMappings();
      await loadMidiActions();
    }
    return saved;
  } finally {
    state.soundboardGridSaving = false;
    renderSoundboardGrid();
  }
}

async function beginSoundboardGridLearn(corner) {
  const target = `__grid_${corner}__`;
  if (state.mappingAssignTarget === target) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    document.getElementById('sb-grid-learn-status').textContent = 'Grid MIDI learn cancelled.';
    return;
  }
  try {
    const response = await apiFetch('/api/midi/last_note', { method: 'GET' });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${text.trim()}`);
    const data = JSON.parse(text);
    const sequence = Number(data.last_seq);
    if (!Number.isInteger(sequence) || sequence < 0) throw new Error('device did not report a valid MIDI event sequence');
    state.mappingAssignTarget = target;
    state.mappingLastSeq = sequence;
    if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
    document.getElementById('sb-grid-learn-status').textContent =
      `Press the ${corner.replace('_', ' ')} pad corner. Click its Learn button again to cancel.`;
  } catch (error) {
    setStatus(`Grid MIDI learn failed: ${error}`, 'warn', 'soundboard-layout');
  }
}

async function saveSoundboardGridCorner(corner, note) {
  const saved = await saveSoundboardLayout({ corner, note: String(note) },
    `Learned ${corner.replace('_', ' ')} as MIDI note ${note}.`);
  document.getElementById('sb-grid-learn-status').textContent = saved
    ? `Learned ${corner.replace('_', ' ')} as MIDI ${note}. Assigning a sound to a calibrated cell maps its pad automatically.`
    : 'Grid MIDI learn failed. Check that the four corner notes form a unique, evenly numbered grid.';
}

async function saveSoundboardModeForSfx(sfx, mode) {
  const normalizedMode = (mode === 'hold') ? 'hold' : 'play';
  setStatus(`saving mode for ${sfx}...`);
  try {
    const res = await apiFetch('/api/soundboard/mode/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `sfx=${encodeURIComponent(sfx)}&mode=${normalizedMode}`,
    });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`mode save failed: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    if (normalizedMode === 'hold') {
      state.soundboardModesBySfx[sfx] = 'hold';
    } else {
      delete state.soundboardModesBySfx[sfx];
    }
    renderSoundboardGrid();
    setStatus(`mode for ${sfx} set to ${normalizedMode}`, 'ok');
  } catch (err) {
    setStatus(`mode save error: ${err}`, 'warn');
  }
}

async function runSystemAction(path, label) {
  try {
    await AudioxUI.runTask({
      sync: 'Syncing filesystem', restart: 'Restarting AudioX', shutdown: 'Shutting down AudioX',
    }[label], async () => {
      AudioxUI.taskProgress(`${label} in progress...`);
      let res;
      try {
        res = await apiFetch(path, { method: 'POST', signal: AbortSignal.timeout(15000) });
      } catch (error) {
        if (label !== 'restart' || !(error instanceof TypeError || error.name === 'TimeoutError')) throw error;
        AudioxUI.taskProgress('Connection interrupted after the restart request. Checking whether AudioX restarts...');
        return;
      }
      const txt = (await res.text()).trim();
      if (!res.ok) throw new Error(`${label} failed: ${res.status} ${txt}`);
    }, { restart: label === 'restart' });
    if (label === 'shutdown') {
      stopConnectionMonitor();
      stopSystemPolling();
    }
    const messages = {
      sync: 'Filesystem synced. Saved files have been flushed to storage.',
      restart: 'AudioX restarted and reconnected.',
      shutdown: 'Shutdown requested. Wait for the device to power off before disconnecting it.',
    };
    setStatus(messages[label] || `${label} completed`, 'ok', `system-${label}`);
    return true;
  } catch (err) {
    setStatus(`${label} error: ${err}`, 'warn', `system-${label}`);
    return false;
  }
}

function sanitizeSfxName(name) {
  const raw = String(name || '').trim();
  if (!raw) return '';
  const base = raw.split('/').pop().split('\\').pop();
  let clean = base.replace(/[^a-zA-Z0-9._-]/g, '_');
  if (!clean) {
    return '';
  }
  if (!/\.wav$/i.test(clean)) {
    clean = `${clean}.wav`;
  }
  return clean;
}

async function listSfxFiles() {
  try {
    let res = await apiFetch('/api/soundboard/order', { method: 'GET' });
    state.soundboardOrderSupported = res.status !== 404;
    if (!state.soundboardOrderSupported) {
      setStatus('Shared sound ordering is unavailable on this firmware. The file list remains playable.', 'warn', 'soundboard-order-support');
      res = await apiFetch('/api/rootfs/sfx/', { method: 'GET' });
    } else {
      AudioxUI.resolve('soundboard-order-support');
    }
    const txt = await res.text();
    if (!res.ok) throw new Error(`Could not load sounds: ${res.status} ${txt}`);

    const files = [];
    for (const line of txt.split(/\r?\n/)) {
      const t = line.trim();
      if (!t || t.startsWith('rootfs listing:')) continue;
      if (t.endsWith('/')) continue;
      files.push(t);
    }

    if (!state.soundboardOrderSupported) files.sort();
    state.sfxFiles = files;
    state.soundboardCellsBySfx = AudioxSoundboardGrid.cellsBySound(state.soundboardLayout,
      AudioxSoundboardGrid.elements(files));
    renderSoundboardGrid();
    AudioxUI.resolve('soundboard-list');
    return files;
  } catch (err) {
    setStatus(err, 'warn', 'soundboard-list');
    return [];
  }
}

async function saveSoundboardOrder(files, file) {
  if (state.soundboardOrderSaving) return;
  if (!state.soundboardOrderSupported || files.length !== state.sfxFiles.length ||
      new Set(files).size !== files.length || files.some((name) => !state.sfxFiles.includes(name))) {
    setStatus('Refresh the soundboard before changing this sound position.', 'warn', 'soundboard-order');
    return;
  }
  state.soundboardOrderSaving = true;
  renderSoundboardGrid();
  try {
    await AudioxUI.runTask('Saving sound order', async () => {
      const res = await apiFetch('/api/soundboard/order', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: `${files.join('\n')}\n`,
        signal: AbortSignal.timeout(10000),
      });
      const txt = await res.text();
      if (!res.ok) throw new Error(`Could not save sound order: ${res.status} ${txt}`);
      const saved = txt.trim().split(/\r?\n/);
      if (saved.length !== files.length || saved.some((name, i) => name !== files[i])) {
        throw new Error('The device returned an unexpected sound order. Refresh the soundboard to check it.');
      }
      state.sfxFiles = saved;
      setStatus('Sound order saved for the website and device display.', 'ok', 'soundboard-order');
    });
  } catch (error) {
    setStatus(error, 'warn', 'soundboard-order');
  } finally {
    state.soundboardOrderSaving = false;
    renderSoundboardGrid();
    const card = Array.from(sbGridEl.children).find((element) => element.dataset.file === file);
    card?.querySelector('.sb-card-name')?.focus({ preventScroll: true });
  }
}

async function reloadSoundboardBank() {
  try {
    const res = await apiFetch('/api/soundboard/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`soundboard reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    try {
      const data = JSON.parse(txt);
      const loaded = Number(data?.loaded || 0);
      const max = Number(data?.max || 0);
      setStatus(`soundboard cache loaded ${loaded}/${max}`, 'ok');
    } catch (_) {
      setStatus('soundboard cache reloaded', 'ok');
    }
    return true;
  } catch (err) {
    setStatus(`soundboard reload error: ${err}`, 'warn');
    return false;
  }
}

function groupSoundboardTileSettings(actions, midi, file, label, isTarget) {
  const settings = document.createElement('details');
  settings.className = 'sb-tile-settings sb-edit-only';
  settings.open = state.soundboardOpenSettings.has(file) || isTarget;
  const summary = document.createElement('summary');
  summary.textContent = isTarget ? 'Waiting for MIDI\u2026' : 'Settings';
  summary.setAttribute('aria-label', `Settings for ${label}`);
  const controls = document.createElement('div');
  controls.className = 'sb-tile-controls';
  for (const control of [...actions.children]) {
    if (!control.classList.contains('sb-edit-only')) continue;
    if (control.tagName === 'SELECT') {
      const field = document.createElement('label');
      field.textContent = control.classList.contains('sb-grid-cell-select') ? 'Grid position' : 'Playback';
      field.appendChild(control);
      controls.appendChild(field);
    } else {
      controls.appendChild(control);
    }
  }
  const clear = midi.querySelector('.sb-midi-clear');
  if (clear) {
    clear.className = 'flat';
    clear.textContent = 'Clear MIDI mapping';
    controls.appendChild(clear);
  }
  settings.append(summary, controls);
  settings.addEventListener('toggle', () => {
    if (settings.open) state.soundboardOpenSettings.add(file);
    else state.soundboardOpenSettings.delete(file);
  });
  actions.appendChild(settings);
}

function renderSoundboardGrid() {
  soundboardOrder.cancel();
  soundboardGridDrag.cancel();
  const gridMode = state.soundboardLayout.mode === 'grid';
  const rows = state.soundboardLayout.rows;
  const columns = state.soundboardLayout.columns;
  state.soundboardCellsBySfx = AudioxSoundboardGrid.cellsBySound(state.soundboardLayout,
    AudioxSoundboardGrid.elements(state.sfxFiles));
  sbGridEl.classList.toggle('grid-mode', gridMode);
  sbGridEl.style.setProperty('--sb-columns', String(columns));
  sbGridEl.classList.toggle('can-reorder', !gridMode && state.soundboardEditing && state.soundboardOrderSupported &&
    !state.soundboardOrderSaving);
  sbGridEl.innerHTML = '';
  const unassignedList = document.getElementById('sb-unassigned-list');
  unassignedList.replaceChildren();
  document.getElementById('sb-unassigned').hidden = !gridMode;
  if (!state.sfxFiles.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = 'No sounds uploaded yet.';
    if (!gridMode) {
      sbGridEl.appendChild(empty);
      return;
    }
    empty.style.gridColumn = `1 / ${columns + 1}`;
    empty.style.gridRow = String(rows + 1);
    sbGridEl.appendChild(empty);
  }

  const occupiedCells = new Set();
  for (const file of gridMode ? AudioxSoundboardGrid.elements(state.sfxFiles) : state.sfxFiles) {
    const action = AudioxSoundboardGrid.actionFor(file);
    const label = action?.label || file;
    const mappedNote = action ? (action.action === 'stop_all' ? state.stopAllMidiNote : state.samplerToggleMidiNote)
      : state.midiMappingsBySfx[file];
    const hasMidi = mappedNote !== undefined && mappedNote !== null;
    const isTarget = state.mappingAssignTarget === file;
    const mode = (state.soundboardModesBySfx[file] === 'hold') ? 'hold' : 'play';

    const card = document.createElement('div');
    card.className = `sb-card${isTarget ? ' assign-target' : ''}`;
    card.dataset.file = file;
    if (gridMode) {
      const cell = state.soundboardCellsBySfx[file];
      if (cell) {
        card.dataset.gridRow = String(cell.row);
        card.dataset.gridColumn = String(cell.column);
        card.style.gridColumn = String(cell.column + 1);
        card.style.gridRow = String(cell.row + 1);
        occupiedCells.add(`${cell.row}:${cell.column}`);
      }
    }

    const nameEl = document.createElement('div');
    nameEl.className = 'sb-card-name';
    nameEl.title = label;
    nameEl.textContent = label;
    nameEl.tabIndex = state.soundboardEditing && !gridMode && state.soundboardOrderSupported ? 0 : -1;
    if (state.soundboardEditing && !gridMode && state.soundboardOrderSupported) {
      nameEl.setAttribute('role', 'button');
      nameEl.setAttribute('aria-label', `Reorder ${file}`);
      nameEl.setAttribute('aria-pressed', 'false');
    }
    nameEl.setAttribute('aria-describedby', 'sb-order-status');

    const midiEl = document.createElement('div');
    midiEl.className = `sb-card-midi${hasMidi ? ' mapped' : ''}`;
    if (hasMidi) {
      const noteSpan = document.createElement('span');
      noteSpan.textContent = `MIDI ${mappedNote}`;
      const clearBtn = document.createElement('button');
      clearBtn.className = 'sb-midi-clear sb-edit-only';
      clearBtn.textContent = '\u00d7';
      clearBtn.title = 'Remove MIDI mapping';
      clearBtn.addEventListener('click', (ev) => {
        ev.stopPropagation();
        if (action) {
          const remove = action.action === 'stop_all' ? deleteStopAllActionMapping : deleteSamplerToggleActionMapping;
          remove().then(renderSoundboardGrid);
        } else deleteMapping(mappedNote);
      });
      midiEl.appendChild(noteSpan);
      midiEl.appendChild(clearBtn);
    } else {
      midiEl.textContent = 'no MIDI';
    }

    const actionsEl = document.createElement('div');
    actionsEl.className = 'sb-card-actions';

    if (gridMode) {
      const cellSelect = document.createElement('select');
      cellSelect.className = 'sb-edit-only sb-grid-cell-select';
      cellSelect.setAttribute('aria-label', `Grid cell for ${label}`);
      cellSelect.appendChild(new Option('Unassigned', ''));
      cellSelect.disabled = state.soundboardGridSaving;
      for (let row = 0; row < rows; row += 1) {
        for (let column = 0; column < columns; column += 1) {
          const option = new Option(`Row ${row + 1}, column ${column + 1}`, `${row}:${column}`);
          const occupiedByOther = Object.entries(state.soundboardCellsBySfx)
            .some(([otherSfx, otherCell]) => otherSfx !== file &&
              otherCell.row === row && otherCell.column === column);
          if (occupiedByOther) {
            option.textContent += ' (swap)';
          }
          cellSelect.appendChild(option);
        }
      }
      const cell = state.soundboardCellsBySfx[file];
      if (cell) cellSelect.value = `${cell.row}:${cell.column}`;
      cellSelect.addEventListener('change', async () => {
        if (!cellSelect.value) {
          await saveSoundboardGridCell(file, null);
          return;
        }
        const [row, column] = cellSelect.value.split(':').map(Number);
        await saveSoundboardGridCell(file, row, column);
      });
      actionsEl.appendChild(cellSelect);
    }

    if (action) {
      const trigger = document.createElement('button');
      trigger.className = `sb-play ${action.action === 'stop_all' ? 'warn' : 'primary'}`;
      trigger.textContent = label;
      trigger.addEventListener('click', action.action === 'stop_all' ? triggerStopAll : triggerSamplerToggle);
      actionsEl.appendChild(trigger);
      groupSoundboardTileSettings(actionsEl, midiEl, file, label, isTarget);
      card.classList.add('sb-action-card');
      card.append(nameEl, midiEl, actionsEl);
      (state.soundboardCellsBySfx[file] ? sbGridEl : unassignedList).appendChild(card);
      continue;
    }

    const modeSelect = document.createElement('select');
    modeSelect.className = 'sb-edit-only';
    modeSelect.title = 'Playback mode for this sound';
    const optPlay = document.createElement('option');
    optPlay.value = 'play';
    optPlay.textContent = 'Play';
    const optHold = document.createElement('option');
    optHold.value = 'hold';
    optHold.textContent = 'Hold';
    modeSelect.appendChild(optPlay);
    modeSelect.appendChild(optHold);
    modeSelect.value = mode;
    modeSelect.addEventListener('change', () => {
      saveSoundboardModeForSfx(file, modeSelect.value);
    });

    const playBtn = document.createElement('button');
    playBtn.className = 'primary sb-play';
    playBtn.setAttribute('aria-label', `${mode === 'hold' ? 'Hold' : 'Play'} ${file}`);
    playBtn.textContent = (mode === 'hold') ? 'Hold' : '\u25b6 Play';

    let holdDown = false;
    const releaseHold = () => {
      if (!holdDown) {
        return;
      }
      holdDown = false;
      triggerSfx(file, 'release');
    };

    playBtn.addEventListener('pointerdown', (ev) => {
      if (mode !== 'hold') {
        return;
      }
      ev.preventDefault();
      holdDown = true;
      triggerSfx(file, 'press');
    });
    playBtn.addEventListener('pointerup', releaseHold);
    playBtn.addEventListener('pointercancel', releaseHold);
    playBtn.addEventListener('pointerleave', releaseHold);
    playBtn.addEventListener('keydown', (event) => {
      if (mode !== 'hold' || ![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      if (!holdDown) {
        holdDown = true;
        triggerSfx(file, 'press');
      }
    });
    playBtn.addEventListener('keyup', (event) => {
      if (mode !== 'hold' || ![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      releaseHold();
    });
    playBtn.addEventListener('blur', releaseHold);
    playBtn.addEventListener('click', (ev) => {
      if (mode === 'hold') {
        ev.preventDefault();
        return;
      }
      triggerSfx(file, 'trigger');
    });

    const assignBtn = document.createElement('button');
    assignBtn.className = 'sb-edit-only';
    assignBtn.textContent = isTarget ? 'Waiting\u2026' : 'Assign MIDI';
    if (isTarget) assignBtn.classList.add('primary');
    assignBtn.addEventListener('click', async () => {
      if (isTarget) {
        state.mappingAssignTarget = null;
        if (state.mappingPollTimer) {
          clearInterval(state.mappingPollTimer);
          state.mappingPollTimer = null;
        }
        sbMappingStatusEl.textContent = 'Assignment cancelled.';
      } else {
        // Snapshot the current seq so only presses AFTER this click are captured.
        try {
          const r = await apiFetch('/api/midi/last_note', { method: 'GET' });
          if (r.ok) {
            const d = JSON.parse(await r.text());
            state.mappingLastSeq = Number(d.last_seq || 0);
          }
        } catch (_) {}
        state.mappingAssignTarget = file;
        if (!state.mappingPollTimer) {
          state.mappingPollTimer = setInterval(pollMappingCapture, 220);
        }
        sbMappingStatusEl.textContent = `Press a MIDI button to assign to "${file}".`;
      }
      renderSoundboardGrid();
    });

    actionsEl.appendChild(modeSelect);
    actionsEl.appendChild(playBtn);
    actionsEl.appendChild(assignBtn);

    const delBtn = document.createElement('button');
    delBtn.className = 'warn sb-edit-only';
    delBtn.textContent = 'Remove';
    delBtn.addEventListener('click', () => deleteSfxFile(file));
    actionsEl.appendChild(delBtn);

    const replaceInput = document.createElement('input');
    replaceInput.type = 'file';
    replaceInput.accept = 'audio/*';
    replaceInput.style.display = 'none';
    replaceInput.addEventListener('change', async () => {
      const selected = replaceInput.files && replaceInput.files[0] ? replaceInput.files[0] : null;
      if (!selected) {
        return;
      }
      await runLoadingTask('Replacing sound', () => replaceSfxFile(file, selected));
      replaceInput.value = '';
    });

    const replaceBtn = document.createElement('button');
    replaceBtn.className = 'sb-edit-only';
    replaceBtn.textContent = 'Replace';
    replaceBtn.addEventListener('click', () => {
      replaceInput.click();
    });
    actionsEl.appendChild(replaceBtn);
    actionsEl.appendChild(replaceInput);
    groupSoundboardTileSettings(actionsEl, midiEl, file, label, isTarget);

    card.appendChild(nameEl);
    card.appendChild(midiEl);
    card.appendChild(actionsEl);
    (gridMode && !state.soundboardCellsBySfx[file] ? unassignedList : sbGridEl).appendChild(card);
  }

  if (gridMode) {
    for (let row = 0; row < rows; row += 1) {
      for (let column = 0; column < columns; column += 1) {
        if (occupiedCells.has(`${row}:${column}`)) continue;
        const placeholder = document.createElement('div');
        placeholder.className = 'sb-grid-empty';
        placeholder.dataset.gridRow = String(row);
        placeholder.dataset.gridColumn = String(column);
        placeholder.textContent = `Empty · ${row + 1}, ${column + 1}`;
        placeholder.setAttribute('aria-label', `Empty grid cell, row ${row + 1}, column ${column + 1}`);
        placeholder.style.gridColumn = String(column + 1);
        placeholder.style.gridRow = String(row + 1);
        sbGridEl.appendChild(placeholder);
      }
    }
    if (!unassignedList.children.length) {
      const empty = document.createElement('p');
      empty.className = 'small';
      empty.textContent = 'No unassigned sounds or actions. Drop a tile here to unassign it.';
      unassignedList.appendChild(empty);
    }
  }
}

async function deleteSfxFile(filename) {
  if (!await AudioxUI.confirm(`Delete "${filename}" from the soundboard? This cannot be undone.`, {
    title: 'Delete sound?', label: 'Delete sound',
  })) return;
  setStatus(`deleting ${filename}...`);
  try {
    const res = await apiFetch(`/api/rootfs/sfx/${encodeURIComponent(filename)}`, {
      method: 'DELETE',
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    await listSfxFiles();
    await reloadSoundboardBank();
    await loadMappings();
    setStatus(`deleted ${filename}`, 'ok');
  } catch (err) {
    setStatus(`delete error: ${err}`, 'warn');
  }
}

async function replaceSfxFile(filename, fileObj) {
  if (!filename || !fileObj) {
    return;
  }

  setStatus(`replacing ${filename}...`);
  try {
    const buf = await fileObj.arrayBuffer();
    const res = await apiFetch(`/api/rootfs/sfx/${encodeURIComponent(filename)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: buf,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`replace failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await reloadSoundboardBank();
    await listSfxFiles();
    setStatus(`replaced ${filename}`, 'ok');
  } catch (err) {
    setStatus(`replace error: ${err}`, 'warn');
  }
}

async function uploadSfxFile() {
  const file = document.getElementById('upload_file').files[0];
  const manualName = document.getElementById('upload_name').value;
  const selectedCell = document.getElementById('sb-upload-cell').value;
  if (!file) {
    setStatus('pick a file first', 'warn');
    return;
  }

  const targetName = sanitizeSfxName(manualName || file.name);
  if (!targetName) {
    setStatus('invalid target filename', 'warn');
    return;
  }

  setStatus(`uploading ${targetName} (streaming)...`);
  try {
    const buf = await file.arrayBuffer();
    const res = await apiFetch(`/api/rootfs/sfx/${targetName}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: buf,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`upload failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await listSfxFiles();
    await loadSoundboardLayout();
    let cellSaved = true;
    if (selectedCell && state.soundboardLayout.mode === 'grid') {
      const [row, column] = selectedCell.split(':').map(Number);
      cellSaved = await saveSoundboardGridCell(targetName, row, column);
    }
    await reloadSoundboardBank();
    AudioxUI.setUnsaved('sound-upload', false);
    setStatus(cellSaved ? `uploaded ${targetName}` :
      `uploaded ${targetName}, but the selected grid cell could not be assigned. Choose a free cell.`,
    cellSaved ? 'ok' : 'warn');
  } catch (err) {
    setStatus(`upload error: ${err}`, 'warn');
  }
}

async function loadMappings() {
  try {
    const res = await apiFetch('/api/midi/mappings', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) return;

    const data = JSON.parse(txt);
    const mappings = Array.isArray(data?.mappings) ? data.mappings : [];
    state.midiMappingsByNote = {};
    state.midiMappingsBySfx = {};

    for (const map of mappings) {
      state.midiMappingsByNote[Number(map.note)] = map.sfx;
      state.midiMappingsBySfx[map.sfx] = Number(map.note);
    }

    renderSoundboardGrid();
  } catch (err) {}
}

async function loadMidiActions() {
  try {
    const res = await apiFetch('/api/midi/actions', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      throw new Error(`${res.status} ${txt.trim()}`);
    }

    const data = JSON.parse(txt);
    state.stopAllMidiNote = null;
    state.samplerToggleMidiNote = null;
    state.passthroughToggleMidiNote = null;
    state.passthroughToggleMidiChannel = null;
    const actions = Array.isArray(data?.actions) ? data.actions : [];
    for (const item of actions) {
      const action = String(item?.action || '');
      if (action !== 'stop_all' && action !== 'sampler_toggle' && action !== 'passthrough_toggle') {
        continue;
      }
      const note = Number(item?.note);
      if (Number.isFinite(note) && note >= 0 && note <= 127) {
        if (action === 'stop_all' && state.stopAllMidiNote === null) {
          state.stopAllMidiNote = note;
        }
        if (action === 'sampler_toggle' && state.samplerToggleMidiNote === null) {
          state.samplerToggleMidiNote = note;
        }
        if (action === 'passthrough_toggle' && state.passthroughToggleMidiNote === null) {
          state.passthroughToggleMidiNote = note;
          state.passthroughToggleMidiChannel = item.channel;
        }
      }
    }
    renderStopAllMidiStatus();
    renderSamplerToggleMidiStatus();
    document.getElementById('passthrough-toggle-midi').textContent =
      state.passthroughToggleMidiNote === null ? 'unassigned' :
        `channel ${state.passthroughToggleMidiChannel}, note ${state.passthroughToggleMidiNote}`;
  } catch (error) {
    setStatus(`Could not load MIDI action mappings: ${error}`, 'warn');
  }
  renderSoundboardGrid();
}

let passthroughRequestPending = false;
async function loadPassthroughOnly(toggle = false) {
  if (passthroughRequestPending) return;
  passthroughRequestPending = true;
  const button = document.getElementById('btn-passthrough-only');
  button.disabled = true;
  try {
    const response = await apiFetch('/api/midi/passthrough_only', { method: toggle ? 'POST' : 'GET' });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${text.trim()}`);
    const { active } = JSON.parse(text);
    button.setAttribute('aria-pressed', String(active));
    button.textContent = active ? 'Restore MIDI Controls' : 'Enable Passthrough Only';
    document.getElementById('btn-assign-passthrough-toggle').disabled = !!active;
  } catch (error) {
    setStatus(`Passthrough-only mode: ${error}`, 'warn', 'midi-passthrough');
  } finally {
    button.disabled = false;
    passthroughRequestPending = false;
  }
}

async function saveActionMapping(note, actionName, channel) {
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }

  if (!actionName) {
    setStatus('invalid action mapping', 'warn');
    return;
  }

  try {
    const body = `note=${note}&action=${actionName}` +
      (actionName === 'passthrough_toggle' ? `&channel=${channel}` : '');
    const res = await apiFetch('/api/midi/action/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`action mapping save failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await loadMappings();
    await loadMidiActions();
    setStatus(`mapped note ${note} -> ${actionName}`, 'ok');
    return true;
  } catch (err) {
    setStatus(`action mapping save error: ${err}`, 'warn');
  }
}

async function saveStopAllActionMapping(note) {
  await saveActionMapping(note, 'stop_all');
}

async function saveSamplerToggleActionMapping(note) {
  await saveActionMapping(note, 'sampler_toggle');
}

async function deleteStopAllActionMapping() {
  if (!Number.isFinite(state.stopAllMidiNote)) {
    setStatus('no stop-all MIDI mapping to remove', 'warn');
    return;
  }
  if (!await AudioxUI.confirm('Remove the MIDI mapping for Stop All?', {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;

  try {
    const res = await apiFetch('/api/midi/action/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `note=${state.stopAllMidiNote}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`action mapping delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await loadMidiActions();
    setStatus('stop-all MIDI mapping removed', 'ok');
  } catch (err) {
    setStatus(`action mapping delete error: ${err}`, 'warn');
  }
}

async function deleteSamplerToggleActionMapping() {
  if (!Number.isFinite(state.samplerToggleMidiNote)) {
    setStatus('no sampler-toggle MIDI mapping to remove', 'warn');
    return;
  }
  if (!await AudioxUI.confirm('Remove the MIDI mapping for Sampler Toggle?', {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;

  try {
    const res = await apiFetch('/api/midi/action/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `note=${state.samplerToggleMidiNote}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`action mapping delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await loadMidiActions();
    setStatus('sampler-toggle MIDI mapping removed', 'ok');
  } catch (err) {
    setStatus(`action mapping delete error: ${err}`, 'warn');
  }
}

async function saveMapping(note, sfx) {
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }
  if (!sfx) {
    setStatus('no target sound selected', 'warn');
    return;
  }

  try {
    const body = `note=${note}&sfx=${sfx}`;
    const res = await apiFetch('/api/midi/mapping/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`mapping save failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await loadMappings();
    setStatus(`mapped note ${note} \u2192 ${sfx}`, 'ok');
  } catch (err) {
    setStatus(`mapping save error: ${err}`, 'warn');
  }
}

async function deleteMapping(note) {
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note', 'warn');
    return;
  }
  if (!await AudioxUI.confirm(`Remove the soundboard MIDI mapping for note ${note}?`, {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;

  try {
    const res = await apiFetch('/api/midi/mapping/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `note=${note}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`mapping delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await loadMappings();
    setStatus(`mapping removed for note ${note}`, 'ok');
  } catch (err) {
    setStatus(`mapping delete error: ${err}`, 'warn');
  }
}

async function pollMappingCapture() {
  if (!state.mappingAssignTarget) {
    if (state.mappingPollTimer) {
      clearInterval(state.mappingPollTimer);
      state.mappingPollTimer = null;
    }
    return;
  }

  const targetAtStart = state.mappingAssignTarget;
  try {
    const res = await apiFetch('/api/midi/last_note', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      if (targetAtStart === '__macro__' || targetAtStart === '__action_passthrough_toggle__' ||
          String(targetAtStart).startsWith('__grid_'))
        throw new Error(`${res.status}: ${txt}`);
      return;
    }
    if (targetAtStart !== state.mappingAssignTarget) return;

    const data = JSON.parse(txt);
    const connected = !!data.connected;
    const seq = Number(data.last_seq || 0);
    const note = Number(data.last_note);

    const isMacroTarget = state.mappingAssignTarget === '__macro__';
    const isGridTarget = state.mappingAssignTarget.startsWith('__grid_');
    const isSbTarget = !isMacroTarget && state.mappingAssignTarget !== '__action_stop_all__' &&
                       state.mappingAssignTarget !== '__action_sampler_toggle__' &&
                       state.mappingAssignTarget !== '__action_passthrough_toggle__' && !isGridTarget;
    const statusEl2 = isMacroTarget ? document.getElementById('chord-learn-status') :
      (isGridTarget ? document.getElementById('sb-grid-learn-status') :
        (isSbTarget ? sbMappingStatusEl : midiActionCaptureStatusEl));

    if (!connected) {
      if (statusEl2) statusEl2.textContent = 'Waiting for MIDI device\u2026';
      return;
    }

    if (seq > state.mappingLastSeq && note >= 0 && note <= 127) {
      state.mappingLastSeq = seq;
      const target = state.mappingAssignTarget;
      state.mappingAssignTarget = null;
      if (state.mappingPollTimer) {
        clearInterval(state.mappingPollTimer);
        state.mappingPollTimer = null;
      }
      if (target === '__macro__') {
        document.getElementById('btn-chord-learn').setAttribute('aria-pressed', 'false');
        if (!Number.isInteger(data.last_channel) || data.last_channel < 0 || data.last_channel > 15) {
          throw new Error('device did not report a valid MIDI channel');
        }
        document.getElementById('chord-channel').value = data.last_channel;
        document.getElementById('chord-note').value = note;
        AudioxUI.setUnsaved('macro-editor');
        statusEl2.textContent = `Channel ${data.last_channel}, note ${note} captured. Save Script to apply the binding.`;
      } else if (target === '__action_stop_all__') {
        await saveStopAllActionMapping(note);
        if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = `Assigned note ${note} to stop all.`;
      } else if (target === '__action_sampler_toggle__') {
        await saveSamplerToggleActionMapping(note);
        if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = `Assigned note ${note} to sampler toggle.`;
      } else if (target === '__action_passthrough_toggle__') {
        if (!Number.isInteger(data.last_channel) || data.last_channel < 0 || data.last_channel > 15)
          throw new Error('device did not report a valid MIDI channel');
        if (await saveActionMapping(note, 'passthrough_toggle', data.last_channel) && midiActionCaptureStatusEl)
          midiActionCaptureStatusEl.textContent =
            `Assigned channel ${data.last_channel}, note ${note} to passthrough toggle.`;
      } else if (target.startsWith('__grid_')) {
        const corner = target.slice('__grid_'.length, -2);
        await saveSoundboardGridCorner(corner, note);
      } else {
        await saveMapping(note, target);
        if (sbMappingStatusEl) sbMappingStatusEl.textContent = `Assigned note ${note} to "${target}".`;
      }
      renderSoundboardGrid();
    }
  } catch (err) {
    if (targetAtStart === '__macro__') {
      cancelChordLearn();
      setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-chords');
    } else if (targetAtStart === '__action_passthrough_toggle__') {
      state.mappingAssignTarget = null;
      clearInterval(state.mappingPollTimer);
      state.mappingPollTimer = null;
      setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-passthrough');
    } else if (String(targetAtStart).startsWith('__grid_')) {
      document.getElementById('sb-grid-learn-status').textContent = `Grid MIDI learn failed: ${err}`;
      setStatus(`Grid MIDI learn failed: ${err}`, 'warn', 'soundboard-layout');
    }
  }
}

async function loadSamplerConfig() {
  try {
    const res = await apiFetch('/api/midi/sampler_config', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      return;
    }
    const data = JSON.parse(txt);
    state.samplerKeyboardEnabled = !!data.keyboard_enabled;
    state.samplerKeyboardChannel = Number(data.keyboard_channel ?? 0);
    state.samplerRootNote = Number(data.root_note ?? 60);

    const enabledEl = document.getElementById('sampler_keyboard_enabled');
    const channelEl = document.getElementById('sampler_keyboard_channel');
    const rootEl = document.getElementById('sampler_root_note');
    if (enabledEl) enabledEl.value = state.samplerKeyboardEnabled ? '1' : '0';
    if (channelEl) channelEl.value = String(state.samplerKeyboardChannel);
    if (rootEl) rootEl.value = String(state.samplerRootNote);
    AudioxUI.setUnsaved('sampler-config', false);
  } catch (_) {
  }
}

async function saveSamplerConfig() {
  const enabled = document.getElementById('sampler_keyboard_enabled')?.value === '1' ? 1 : 0;
  const channel = Number(document.getElementById('sampler_keyboard_channel')?.value ?? 0);
  const rootNote = Number(document.getElementById('sampler_root_note')?.value ?? 60);

  if (!Number.isFinite(channel) || channel < 0 || channel > 15) {
    setStatus('invalid sampler keyboard channel, expected 0-15', 'warn');
    return;
  }
  if (!Number.isFinite(rootNote) || rootNote < 0 || rootNote > 127) {
    setStatus('invalid sampler root note, expected 0-127', 'warn');
    return;
  }

  try {
    const body = `keyboard_enabled=${enabled}&keyboard_channel=${Math.trunc(channel)}&root_note=${Math.trunc(rootNote)}`;
    const res = await apiFetch('/api/midi/sampler_config', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`sampler config save failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    await loadSamplerConfig();
    AudioxUI.setUnsaved('sampler-config', false);
    setStatus('sampler config saved', 'ok');
  } catch (err) {
    setStatus(`sampler config save error: ${err}`, 'warn');
  }
}

async function initializeGraphFromConfig() {
  ensureRoutingGraph();
  await loadConfig();
  await loadLegacyConfigStatus();
  await loadFramebufferView();
  await loadSoundboardModes();
  await listSfxFiles();
  await loadSoundboardLayout();
  await loadMappings();
  await loadMidiActions();
  await loadPassthroughOnly();
  await loadSamplerConfig();
  await loadRoutingThings();
  await loadPluginCatalog();
  if (effectsManager) {
    try {
      await effectsManager.loadEffects();
    } catch (err) {
      setStatus(String(err), 'warn', 'effects-load');
    }
  }
  await loadRoutingFile();
  arrangeRoutingGraph();
  state.edgeDeleteEnabled = true;
  updateZoomLabel();
  startRoutingThingsPolling();
}

// ---- Volume page ----

const volumeMeters = new Map();
let meterPollTimer = null;
let meterPollBusy = false;
let meterTopology = null;
const meterLevels = new WeakMap();
const meterReducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
let meterAnimationFrame = null;
let meterAnimationTime = 0;

function resetVolumeMeters() {
  if (meterAnimationFrame !== null) cancelAnimationFrame(meterAnimationFrame);
  meterAnimationFrame = null;
  for (const [id, group] of volumeMeters) {
    updateVolumeMeter(id, Array(group.children[0]?.children.length || 0).fill(0), true);
  }
}

function animateVolumeMeters(now) {
  meterAnimationFrame = null;
  if (state.activeTab !== 'volume' || document.hidden) {
    resetVolumeMeters();
    return;
  }
  const elapsed = Math.max(0, now - meterAnimationTime);
  meterAnimationTime = now;
  let moving = false;
  for (const group of volumeMeters.values()) {
    for (const row of group.children[0]?.children || []) {
      const meter = row.children[0];
      const level = meterLevels.get(meter);
      if (!level) continue;
      // Smooth in dB, with a fast attack (35 ms) and gentler release (300 ms).
      const duration = level.target > level.current ? 35 : 300;
      level.current += (level.target - level.current) * -Math.expm1(-elapsed / duration);
      if (meterReducedMotion.matches || Math.abs(level.target - level.current) < 0.05) {
        level.current = level.target;
      } else {
        moving = true;
      }
      meter.value = (level.current + 60) / 60;
      row.style.setProperty('--meter-level', `${meter.value * 100}%`);
    }
  }
  if (moving) meterAnimationFrame = requestAnimationFrame(animateVolumeMeters);
}

function stopMeterPolling() {
  if (meterPollTimer !== null) clearInterval(meterPollTimer);
  meterPollTimer = null;
  resetVolumeMeters();
}

function startMeterPolling() {
  stopMeterPolling();
  if (document.hidden) return;
  pollMeters();
  meterPollTimer = setInterval(pollMeters, 250);
}

function updateVolumeMeter(id, levels, immediate = false) {
  const group = volumeMeters.get(id);
  if (!group) return;
  if (!levels.length) {
    group.replaceChildren();
    return;
  }
  if (group.children[0]?.children.length !== levels.length) {
    group.replaceChildren();
    const bars = document.createElement('div');
    bars.className = 'vol-meter-bars';
    const name = state.volumes?.find(vol => vol.id === id)?.name || id;
    levels.forEach((_, channel) => {
      const row = document.createElement('div');
      row.className = 'vol-meter-row';
      const meter = document.createElement('meter');
      meter.min = 0;
      meter.max = 1;
      meter.high = 0.95;
      meter.optimum = 0;
      meter.value = 0;
      row.style.setProperty('--meter-level', '0%');
      meterLevels.set(meter, { current: -60, target: -60 });
      meter.setAttribute('aria-label', `${name} channel ${channel + 1} signal level`);
      row.appendChild(meter);
      bars.appendChild(row);
    });
    const value = document.createElement('span');
    value.className = 'vol-meter-reading';
    group.append(bars, value);
  }
  levels.forEach((raw, channel) => {
    const level = Number.isFinite(raw) ? Math.max(0, Math.min(1, raw)) : 0;
    const db = level > 0 ? 20 * Math.log10(level) : -Infinity;
    const row = group.children[0].children[channel];
    const meter = row.children[0];
    const display = meterLevels.get(meter);
    display.target = Math.max(-60, db);
    if (immediate || meterReducedMotion.matches) {
      display.current = display.target;
      meter.value = (display.current + 60) / 60;
      row.style.setProperty('--meter-level', `${meter.value * 100}%`);
    }
    meter.setAttribute('aria-valuetext', `${db > -60 ? db.toFixed(1) : '-inf'} dBFS`);
  });
  const level = levels.reduce((sum, raw) =>
    sum + (Number.isFinite(raw) ? Math.max(0, Math.min(1, raw)) : 0), 0) / levels.length;
  const db = level > 0 ? 20 * Math.log10(level) : -Infinity;
  group.children[1].textContent = `${db > -60 ? db.toFixed(1) : '-inf'} dB`;
  group.children[1].title = `Average channel peak: ${db > -60 ? db.toFixed(1) : '-inf'} dBFS`;
  if (!immediate && !meterReducedMotion.matches && meterAnimationFrame === null && levels.length) {
    meterAnimationTime = performance.now();
    meterAnimationFrame = requestAnimationFrame(animateVolumeMeters);
  }
}

async function pollMeters() {
  if (meterPollBusy || state.activeTab !== 'volume' || document.hidden) return;
  meterPollBusy = true;
  const status = document.getElementById('vol-meter-status');
  try {
    const res = await apiFetch('/api/audio/meters', { cache: 'no-store', signal: AbortSignal.timeout(2000) });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    if (!Array.isArray(data.meters) || data.meters.some(meter =>
      typeof meter.id !== 'string' || !Array.isArray(meter.levels) ||
      meter.levels.length > 16 || meter.levels.some(level => !Number.isFinite(level)))) {
      throw new Error('Invalid meter data');
    }
    if (state.activeTab !== 'volume' || document.hidden) return;
    const topology = JSON.stringify(data.meters.map(meter => [meter.id, meter.levels.length]));
    if (meterTopology !== null && topology !== meterTopology && !(await loadVolumes())) {
      throw new Error('Could not refresh meter channels');
    }
    meterTopology = topology;
    if (state.activeTab !== 'volume' || document.hidden) return;
    const liveIds = new Set(data.meters.map(meter => meter.id));
    for (const [id, group] of volumeMeters) {
      if (!liveIds.has(id)) group.replaceChildren();
    }
    data.meters.forEach(meter => updateVolumeMeter(meter.id, meter.levels));
    if (status) {
      status.textContent = '';
      status.hidden = true;
    }
  } catch (err) {
    if (state.activeTab !== 'volume' || document.hidden) return;
    resetVolumeMeters();
    const message = `Meters unavailable: ${AudioxUI.formatMessage(err)}`;
    if (status) {
      status.textContent = message;
      status.hidden = false;
    }
  } finally {
    meterPollBusy = false;
  }
}

async function loadVolumes() {
  try {
    const [vRes, ccRes] = await Promise.all([
      apiFetch('/api/audio/volumes', { method: 'GET' }),
      apiFetch('/api/midi/cc_volumes', { method: 'GET' }),
    ]);
    if (!vRes.ok || !ccRes.ok) {
      throw new Error(`Volume data load failed (${!vRes.ok ? vRes.status : ccRes.status}). Try refreshing the Volume page.`);
    }
    const volumes = await vRes.json();
    const mappings = await ccRes.json();
    state.volumes = Array.isArray(volumes?.volumes) ? volumes.volumes : [];
    state.ccVolumes = Array.isArray(mappings?.mappings) ? mappings.mappings : [];
    renderVolGrid();
    AudioxUI.resolve('volumes-load');
    return true;
  } catch (err) {
    setStatus(`volume load error: ${err}`, 'warn', 'volumes-load');
    return false;
  }
}

function ccForThing(thingId) {
  const m = state.ccVolumes.find((e) => e.id === thingId);
  return m ? Number(m.cc) : null;
}

function gainToPercent(gain) {
  return Math.round(Number(gain) * 100);
}

function percentToGain(pct) {
  return Number(pct) / 100;
}

function renderVolGrid() {
  if (!volGridEl) return;
  volumeMeters.clear();
  volGridEl.innerHTML = '';
  const audioVolumes = state.volumes.filter(vol => vol.id !== 'routing_helper');
  const effects = audioVolumes.filter(vol => isEffectThingId(vol.id));
  const visibleVolumes = audioVolumes.filter(vol => state.volShowEffects || !isEffectThingId(vol.id));
  const effectSummary = document.getElementById('vol-effect-summary');
  if (effectSummary) {
    const adjusted = effects.filter(vol => vol.gain !== 1).length;
    effectSummary.textContent = !state.volShowEffects && effects.length
      ? `${effects.length} effect ${effects.length === 1 ? 'channel' : 'channels'} hidden${adjusted ? `; ${adjusted} with adjusted gain` : ''}. Saved gain, MIDI mappings, and script control remain active.`
      : '';
    effectSummary.hidden = !effectSummary.textContent;
  }

  if (!visibleVolumes.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = audioVolumes.length
      ? 'Only effect channels are available. Enable Show effect channels to see them.'
      : 'No active devices. Open the Routing tab first.';
    volGridEl.appendChild(empty);
    return;
  }

  for (const vol of visibleVolumes) {
    const { id, name, gain } = vol;
    const cc = ccForThing(id);
    const isTarget = state.volAssignTarget === id;

    const card = document.createElement('div');
    card.className = `vol-card${isTarget ? ' cc-target' : ''}`;

    const nameEl = document.createElement('div');
    nameEl.className = 'vol-card-name';
    nameEl.title = name;
    nameEl.textContent = name;

    const idEl = document.createElement('details');
    idEl.className = 'vol-card-id';
    const idSummary = document.createElement('summary');
    idSummary.textContent = 'Advanced';
    const idText = document.createElement('span');
    idText.textContent = `Script / routing ID: ${id}`;
    idEl.appendChild(idSummary);
    idEl.appendChild(idText);

    const sliderRow = document.createElement('div');
    sliderRow.className = 'vol-slider-row';

    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = '0';
    slider.max = '200';
    slider.step = '1';
    slider.value = String(Math.max(0, Math.min(200, gainToPercent(gain))));
    slider.setAttribute('aria-label', `${name} outgoing gain`);
    slider.setAttribute('aria-orientation', 'vertical');

    const pctEl = document.createElement('div');
    pctEl.className = 'vol-pct';
    pctEl.textContent = `${gainToPercent(gain)}%`;
    slider.setAttribute('aria-valuetext', pctEl.textContent);

    let saveTimer = null;
    const showGain = value => {
      slider.value = String(gainToPercent(value));
      pctEl.textContent = `${gainToPercent(value)}%`;
      slider.setAttribute('aria-valuetext', pctEl.textContent);
    };
    slider.addEventListener('input', () => {
      pctEl.textContent = `${slider.value}%`;
      slider.setAttribute('aria-valuetext', pctEl.textContent);
      clearTimeout(saveTimer);
      saveTimer = setTimeout(async () => {
        const requested = percentToGain(slider.value);
        if (await setVolumeGain(id, requested) === false && percentToGain(slider.value) === requested) {
          showGain(vol.gain);
        }
      }, 120);
    });

    sliderRow.appendChild(slider);
    sliderRow.appendChild(pctEl);
    const scale = document.createElement('div');
    scale.className = 'vol-fader-scale';
    scale.setAttribute('aria-hidden', 'true');
    for (const tick of ['200', '100', '0']) {
      const mark = document.createElement('span');
      mark.textContent = tick;
      scale.appendChild(mark);
    }
    sliderRow.appendChild(scale);
    const unityBtn = document.createElement('button');
    unityBtn.className = 'flat vol-unity';
    unityBtn.textContent = 'Unity';
    unityBtn.title = 'Reset outgoing gain to 100% (0 dB)';
    unityBtn.setAttribute('aria-label', `Reset ${name} to unity gain`);
    unityBtn.addEventListener('click', async () => {
      clearTimeout(saveTimer);
      showGain(1);
      if (await setVolumeGain(id, 1) === false && slider.value === '100') showGain(vol.gain);
    });

    const ccRow = document.createElement('div');
    ccRow.className = 'vol-cc-row';

    const ccBadge = document.createElement('span');
    ccBadge.className = `vol-cc-badge${cc !== null ? '' : ' unset'}`;
    ccBadge.textContent = cc !== null ? `CC ${cc}` : 'no MIDI CC';

    const assignBtn = document.createElement('button');
    assignBtn.textContent = isTarget ? 'Waiting\u2026' : 'Assign CC';
    if (isTarget) assignBtn.classList.add('primary');
    assignBtn.addEventListener('click', async () => {
      if (isTarget) {
        state.volAssignTarget = null;
        if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
        if (volCcStatusEl) volCcStatusEl.textContent = 'Cancelled.';
        renderVolGrid();
        return;
      }
      try {
        const r = await apiFetch('/api/midi/last_cc', { method: 'GET' });
        if (r.ok) {
          const d = JSON.parse(await r.text());
          state.volCcLastSeq = Number(d.last_seq || 0);
        }
      } catch (_) {}
      state.volAssignTarget = id;
      if (!state.volCcPollTimer) {
        state.volCcPollTimer = setInterval(pollVolCcCapture, 220);
      }
      if (volCcStatusEl) volCcStatusEl.textContent = `Move a MIDI knob or slider to assign to "${name}".`;
      renderVolGrid();
    });

    const clearBtn = document.createElement('button');
    clearBtn.className = 'flat';
    clearBtn.textContent = 'Clear CC';
    clearBtn.disabled = cc === null;
    clearBtn.addEventListener('click', async () => {
      if (cc === null) return;
      await deleteCcVolumeMapping(cc);
    });

    if (cc !== null) ccRow.appendChild(ccBadge);
    ccRow.appendChild(assignBtn);
    if (cc !== null) ccRow.appendChild(clearBtn);
    if (cc === null) ccRow.appendChild(ccBadge);

    card.appendChild(nameEl);
    const meters = document.createElement('div');
    meters.className = 'vol-meters';
    volumeMeters.set(id, meters);
    const controls = document.createElement('div');
    controls.className = 'vol-channel-controls';
    controls.appendChild(sliderRow);
    controls.appendChild(meters);
    card.appendChild(controls);
    card.appendChild(unityBtn);
    card.appendChild(ccRow);
    card.appendChild(idEl);
    volGridEl.appendChild(card);
  }
}

async function setVolumeGain(thingId, gain) {
  try {
    const res = await apiFetch('/api/audio/volume/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `id=${encodeURIComponent(thingId)}&gain=${gain.toFixed(4)}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`volume set failed: ${res.status} ${txt}`, 'warn');
      return false;
    }
    // Update local state
    const v = state.volumes.find((e) => e.id === thingId);
    if (v) v.gain = gain;
    setStatus(`${v?.name || thingId} volume set to ${gainToPercent(gain)}%`, 'ok');
    return true;
  } catch (err) {
    setStatus(`volume set error: ${err}`, 'warn');
    return false;
  }
}

async function pollVolCcCapture() {
  if (!state.volAssignTarget) {
    if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
    return;
  }
  try {
    const res = await apiFetch('/api/midi/last_cc', { method: 'GET' });
    if (!res.ok) return;
    const data = JSON.parse(await res.text());
    const seq = Number(data.last_seq || 0);
    const cc = Number(data.last_cc);

    if (!data.connected) {
      if (volCcStatusEl) volCcStatusEl.textContent = 'Waiting for MIDI device\u2026';
      return;
    }

    if (seq > state.volCcLastSeq && cc >= 0 && cc <= 127) {
      state.volCcLastSeq = seq;
      const target = state.volAssignTarget;
      state.volAssignTarget = null;
      if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
      await setCcVolumeMapping(cc, target);
      if (volCcStatusEl) volCcStatusEl.textContent = `Assigned CC ${cc} to "${target}".`;
    }
  } catch (_) {}
}

async function setCcVolumeMapping(cc, thingId) {
  try {
    const res = await apiFetch('/api/midi/cc_volume/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `cc=${cc}&id=${encodeURIComponent(thingId)}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`CC volume set failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    // Update local CC map
    state.ccVolumes = state.ccVolumes.filter((e) => e.cc !== cc && e.id !== thingId);
    state.ccVolumes.push({ cc, id: thingId });
    renderVolGrid();
    setStatus(`CC ${cc} \u2192 ${thingId} volume`, 'ok');
  } catch (err) {
    setStatus(`CC volume set error: ${err}`, 'warn');
  }
}

async function deleteCcVolumeMapping(cc) {
  if (!await AudioxUI.confirm(`Remove the volume control mapping for MIDI CC ${cc}?`, {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;
  try {
    const res = await apiFetch('/api/midi/cc_volume/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `cc=${cc}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`CC volume delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    state.ccVolumes = state.ccVolumes.filter((e) => e.cc !== cc);
    renderVolGrid();
    setStatus(`CC ${cc} volume mapping removed`, 'ok');
  } catch (err) {
    setStatus(`CC volume delete error: ${err}`, 'warn');
  }
}

tabsEl.querySelectorAll('.tab-btn').forEach((btn) => {
  btn.addEventListener('click', async () => {
    if (state.activeTab === 'routing' && btn.dataset.tab !== 'routing'
        && AudioxUI.hasUnsavedChanges('routing')
        && !await AudioxUI.confirm('Routing has unsaved wire edits. They will remain pending until you save or undo them.', {
          title: 'Leave unsaved routing?', label: 'Leave with edits',
        })) return;
    setActiveTab(btn.dataset.tab);
    if (btn.dataset.tab === 'routing') {
      pollRoutingThingsIfNeeded();
    }
  });
});

document.getElementById('btn-load').addEventListener('click', () => runLoadingTask('Loading configuration', async () => {
  const cfg = await loadConfig();
  if (cfg) {
    await loadRoutingThings();
    await loadRoutingFile();
  }
}));

document.getElementById('btn-save').addEventListener('click', () => runLoadingTask('Saving configuration', async () => {
  const ok = await saveConfig();
  if (ok) {
    await loadRoutingThings();
    await loadRoutingFile();
  }
}));

document.getElementById('btn-reload').addEventListener('click', async () => {
  const choice = await AudioxUI.confirm('Reload Config is unreliable and may cause instability or leave the system unresponsive until rebooted. USB audio, MIDI, keyboard, and networking will disconnect and may not recover. Save Config, then use System > Restart Device instead (recommended). Save any pending changes before continuing. Reboot Instead stops all audio while the device restarts and applies saved USB changes. Reload anyway?', {
    title: 'Warning: USB reload may cause instability', label: 'Reload anyway', alternateLabel: 'Reboot Instead',
  });
  if (choice === 'alternate') {
    await runSystemAction('/api/system/restart', 'restart');
    return;
  }
  if (choice !== true) return;
  return runLoadingTask('Reloading configuration', async () => {
    const ok = await reloadConfig();
    if (ok) {
      await loadRoutingThings();
      await loadRoutingFile();
    }
  });
});

document.getElementById('btn-route-save').addEventListener('click', () => runLoadingTask('Saving routing', saveRouting));
document.getElementById('routing-advanced').checked = state.advancedRouting;
document.getElementById('routing-advanced').addEventListener('change', (ev) => {
  setRoutingAdvanced(ev.target.checked);
  setStatus(state.advancedRouting ? 'Advanced routing: every channel has its own socket' :
    'Simple routing: stereo pairs share one wire', 'ok');
});
document.getElementById('btn-route-arrange').addEventListener('click', () => {
  state.autoArrange = true;
  state.positions = {};
  arrangeRoutingGraph();
});
document.getElementById('btn-zoom-fit').addEventListener('click', fitRoutingGraph);

const graphShellEl = document.querySelector('.graph-shell');
const fullscreenButton = document.getElementById('btn-graph-fullscreen');
let graphNativeFullscreen = false;
function updateGraphFullscreen() {
  if (graphNativeFullscreen && !document.fullscreenElement) {
    graphShellEl.classList.remove('expanded');
    graphNativeFullscreen = false;
  }
  const expanded = graphShellEl.classList.contains('expanded');
  fullscreenButton.textContent = expanded ? 'Exit fullscreen' : 'Fullscreen';
  fullscreenButton.setAttribute('aria-pressed', String(expanded));
  requestAnimationFrame(() => { resizeRoutingCanvas(); fitRoutingGraph(); });
}
fullscreenButton.addEventListener('click', async () => {
  try {
    if (graphShellEl.classList.contains('expanded')) {
      graphShellEl.classList.remove('expanded');
      if (graphNativeFullscreen) await document.exitFullscreen();
    } else {
      if (document.fullscreenEnabled) {
        // Fullscreen the document so notifications and modal dialogs remain visible.
        await document.documentElement.requestFullscreen();
        graphNativeFullscreen = true;
      }
      graphShellEl.classList.add('expanded');
    }
    updateGraphFullscreen();
  } catch (error) {
    graphShellEl.classList.add('expanded');
    updateGraphFullscreen();
    setStatus(`Browser fullscreen unavailable: ${error.message}. Using the expanded graph view instead.`, 'warn');
  }
});
document.addEventListener('fullscreenchange', updateGraphFullscreen);
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !graphNativeFullscreen && graphShellEl.classList.contains('expanded')) {
    graphShellEl.classList.remove('expanded');
    updateGraphFullscreen();
    fullscreenButton.focus();
  }
}, true);
document.getElementById('btn-bluetooth-scan').addEventListener('click', scanBluetoothDevices);

document.getElementById('btn-zoom-in').addEventListener('click', () => {
  const scale = state.graphCanvas ? state.graphCanvas.ds.scale : 1;
  setRoutingZoom(scale * 1.15);
});

document.getElementById('btn-zoom-out').addEventListener('click', () => {
  const scale = state.graphCanvas ? state.graphCanvas.ds.scale : 1;
  setRoutingZoom(scale * 0.87);
});

document.getElementById('btn-zoom-reset').addEventListener('click', () => {
  setRoutingZoom(1);
});

graphWrapEl.addEventListener('wheel', () => {
  requestAnimationFrame(updateZoomLabel);
}, { passive: true });

window.addEventListener('resize', () => {
  if (state.graphCanvas) {
    resizeRoutingCanvas();
    updateZoomLabel();
  }
});

function watchRoutingPixelRatio() {
  const query = window.matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
  query.addEventListener('change', () => {
    resizeRoutingCanvas();
    watchRoutingPixelRatio();
  }, { once: true });
}
watchRoutingPixelRatio();

if (typeof ResizeObserver === 'function') {
  const ro = new ResizeObserver(() => {
    if (!state.graphCanvas) {
      return;
    }
    resizeRoutingCanvas();
    if (state.autoArrange && state.activeTab === 'routing') fitRoutingGraph();
    updateZoomLabel();
  });
  ro.observe(graphWrapEl);
}

document.getElementById('btn-route-undo').addEventListener('click', () => routingHistory.undo());
document.getElementById('btn-route-redo').addEventListener('click', () => routingHistory.redo());
window.addEventListener('audiox-scene-conflict', () => {
  state.sceneRoutingConflict = true;
  document.getElementById('scene-routing-conflict').hidden = false;
  document.getElementById('btn-route-draft').hidden = false;
});

async function loadActiveRouting() {
  if (AudioxUI.hasUnsavedChanges('routing') && !await AudioxUI.confirm(
    'Discard pending wire edits and load the active setup? Download the wire draft first if you want to keep it.', {
      title: 'Load active routing?', label: 'Discard draft and load',
    })) return;
  const response = await apiFetch(routingPath, { cache: 'no-store' });
  if (!response.ok) throw new Error(`Could not read active routing (${response.status}). Draft retained.`);
  const activeEdges = parseRoutingText(await response.text());
  const loaded = await loadRoutingThingsWithOptions({ silent: false, force: true });
  if (!loaded) return;
  state.edges = activeEdges;
  state.dormantEdges = [];
  state.virtuals = [];
  state.selectedEdgeIdx = -1;
  rebuildRoutingGraph();
  routingHistory.reset([...state.edges, ...state.dormantEdges]);
  state.sceneRoutingConflict = false;
  document.getElementById('scene-routing-conflict').hidden = true;
  document.getElementById('btn-route-draft').hidden = true;
  await effectsManager?.loadEffects();
  await loadVolumes();
}

window.addEventListener('audiox-scene-recalled', () => {
  if (AudioxUI.hasUnsavedChanges('routing')) {
    window.dispatchEvent(new CustomEvent('audiox-scene-conflict'));
    return;
  }
  loadActiveRouting().catch((error) => setStatus(`scene refresh failed: ${error}`, 'warn', 'scenes'));
});
document.getElementById('btn-route-reload').addEventListener('click', () => {
  loadActiveRouting().catch((error) => setStatus(`routing refresh failed: ${error}`, 'warn'));
});
document.getElementById('btn-route-draft').addEventListener('click', () => {
  const url = URL.createObjectURL(new Blob([formatRoutingText()], { type: 'text/plain' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = 'audiox-wire-draft.txt';
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

document.addEventListener('keydown', async (ev) => {
  if (ev.target?.closest?.('input, textarea, select, [contenteditable], dialog')
      || AudioxUI.isBusy()) return;
  if (state.activeTab === 'routing' && (ev.ctrlKey || ev.metaKey)
      && (ev.key.toLowerCase() === 'z' || ev.key.toLowerCase() === 'y')) {
    ev.preventDefault();
    if (ev.shiftKey || ev.key.toLowerCase() === 'y') routingHistory.redo();
    else routingHistory.undo();
    return;
  }
  if (ev.key === 'Delete') {
    if (state.activeTab !== 'routing') {
      return;
    }

    ev.preventDefault();
    if (state.selectedEdgeIdx >= 0) {
      deleteSelectedEdge();
      return;
    }

    const selected = state.graphCanvas ? Object.values(state.graphCanvas.selected_nodes || {}) : [];
    const node = selected.length ? selected[0] : null;
    if (!node) {
      return;
    }
    if (isProtectedNode(node)) {
      setStatus('device nodes cannot be deleted', 'warn');
      return;
    }
    if (isEffectNode(node)) {
      const thingId = normalizeThingId(node?.properties?.thingId);
      if (thingId) {
        deleteEffectThing(thingId);
      }
      return;
    }
    if (!await AudioxUI.confirm('Remove this node from the routing graph?', {
      title: 'Remove routing node?', label: 'Remove node',
    })) return;
    state.graph.remove(node);
    syncEdgesFromGraph();
    state.graphCanvas.setDirty(true, true);
    setStatus('node removed', 'ok');
  }
});

document.getElementById('btn-upload-real').addEventListener('click', () => runLoadingTask('Uploading sound', uploadSfxFile));

function setSoundboardEditing(editing) {
  soundboardOrder.cancel();
  soundboardGridDrag.cancel();
  state.soundboardEditing = editing;
  document.getElementById('soundboard-layout').classList.toggle('play-mode', !editing);
  sbGridEl.querySelectorAll('.sb-card-name').forEach((name) => {
    name.tabIndex = editing && state.soundboardLayout.mode !== 'grid' && state.soundboardOrderSupported ? 0 : -1;
    if (editing && state.soundboardLayout.mode !== 'grid' && state.soundboardOrderSupported) {
      name.setAttribute('role', 'button');
      name.setAttribute('aria-label', `Reorder ${name.textContent}`);
      name.setAttribute('aria-pressed', 'false');
    } else {
      name.removeAttribute('role');
      name.removeAttribute('aria-label');
      name.removeAttribute('aria-pressed');
    }
  });
  sbGridEl.classList.toggle('can-reorder', editing && state.soundboardLayout.mode !== 'grid' &&
    state.soundboardOrderSupported);
  for (const [id, active] of [['btn-sb-play-mode', !editing], ['btn-sb-edit-mode', editing]]) {
    const button = document.getElementById(id);
    button.setAttribute('aria-pressed', String(active));
    button.classList.toggle('primary', active);
    button.classList.toggle('flat', !active);
  }
  if (!editing && state.sfxFiles.includes(state.mappingAssignTarget)) {
    state.mappingAssignTarget = null;
    clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    renderSoundboardGrid();
  }
  if (!editing && String(state.mappingAssignTarget || '').startsWith('__grid_')) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    document.getElementById('sb-grid-learn-status').textContent = 'Grid MIDI learn cancelled.';
  }
  sbMappingStatusEl.textContent = editing
    ? (state.soundboardLayout.mode === 'grid'
      ? 'Manage sounds, grid positions, playback modes, and MIDI assignments.'
      : 'Manage sounds, playback modes, and MIDI assignments.')
    : 'Tap a sound to play. Switch to Edit to manage sounds and MIDI.';
}
document.getElementById('btn-sb-play-mode').addEventListener('click', () => setSoundboardEditing(false));
document.getElementById('btn-sb-edit-mode').addEventListener('click', () => setSoundboardEditing(true));
document.getElementById('sb-layout-mode').addEventListener('change', async (event) => {
  const { rows, columns } = state.soundboardLayout;
  const saved = await saveSoundboardLayout({
    mode: event.currentTarget.value,
    rows: String(rows),
    columns: String(columns),
  }, `Soundboard switched to ${event.currentTarget.value} layout.`);
  if (!saved) event.currentTarget.value = state.soundboardLayout.mode;
});
for (const id of ['sb-grid-rows', 'sb-grid-columns']) {
  document.getElementById(id).addEventListener('change', async () => {
    const rows = document.getElementById('sb-grid-rows').value;
    const columns = document.getElementById('sb-grid-columns').value;
    await saveSoundboardLayout({
      mode: state.soundboardLayout.mode,
      rows,
      columns,
    }, `Soundboard grid set to ${rows} rows by ${columns} columns.`);
  });
}
document.querySelectorAll('.sb-grid-learn').forEach((button) => {
  button.addEventListener('click', () => beginSoundboardGridLearn(button.dataset.corner));
});

document.getElementById('btn-upload-clear').addEventListener('click', () => {
  document.getElementById('upload_name').value = '';
  document.getElementById('upload_file').value = '';
  AudioxUI.setUnsaved('sound-upload', false);
  setStatus('upload form cleared');
});

document.getElementById('btn-sb-refresh').addEventListener('click', async () => {
  await reloadSoundboardBank();
  await listSfxFiles();
  await loadSoundboardLayout();
  await loadMappings();
  await loadSoundboardModes();
  setStatus('soundboard refreshed', 'ok');
});

document.getElementById('btn-sb-stop-all').addEventListener('click', triggerStopAll);
document.getElementById('btn-fb-view').addEventListener('click', toggleFramebufferView);

document.getElementById('btn-sb-assign-stop-all').addEventListener('click', async () => {
  cancelChordLearn();
  const isTarget = state.mappingAssignTarget === '__action_stop_all__';
  if (isTarget) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) { clearInterval(state.mappingPollTimer); state.mappingPollTimer = null; }
    if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = 'Cancelled.';
    return;
  }
  try {
    const r = await apiFetch('/api/midi/last_note', { method: 'GET' });
    if (r.ok) { const d = JSON.parse(await r.text()); state.mappingLastSeq = Number(d.last_seq || 0); }
  } catch (_) {}
  state.mappingAssignTarget = '__action_stop_all__';
  if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
  if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = 'Press a MIDI button to map stop all.';
});

document.getElementById('btn-sb-clear-stop-all').addEventListener('click', async () => {
  await deleteStopAllActionMapping();
});

document.getElementById('btn-sb-assign-sampler-toggle').addEventListener('click', async () => {
  cancelChordLearn();
  const isTarget = state.mappingAssignTarget === '__action_sampler_toggle__';
  if (isTarget) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) { clearInterval(state.mappingPollTimer); state.mappingPollTimer = null; }
    if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = 'Cancelled.';
    return;
  }
  try {
    const r = await apiFetch('/api/midi/last_note', { method: 'GET' });
    if (r.ok) { const d = JSON.parse(await r.text()); state.mappingLastSeq = Number(d.last_seq || 0); }
  } catch (_) {}
  state.mappingAssignTarget = '__action_sampler_toggle__';
  if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
  if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = 'Press a MIDI button to map sampler toggle.';
});

document.getElementById('btn-sb-clear-sampler-toggle').addEventListener('click', async () => {
  await deleteSamplerToggleActionMapping();
});

document.getElementById('btn-passthrough-only').addEventListener('click', () => loadPassthroughOnly(true));
document.getElementById('btn-assign-passthrough-toggle').addEventListener('click', async () => {
  cancelChordLearn();
  const target = '__action_passthrough_toggle__';
  if (state.mappingAssignTarget === target) {
    state.mappingAssignTarget = null;
    clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    midiActionCaptureStatusEl.textContent = 'Cancelled.';
    return;
  }
  try {
    const response = await apiFetch('/api/midi/last_note', { method: 'GET' });
    if (!response.ok) throw new Error(await response.text());
    state.mappingLastSeq = Number(JSON.parse(await response.text()).last_seq || 0);
    state.mappingAssignTarget = target;
    if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
    midiActionCaptureStatusEl.textContent = 'Press a MIDI button to map passthrough-only toggle. Exit passthrough-only mode before learning.';
  } catch (error) {
    setStatus(`MIDI learn failed: ${error}`, 'warn');
  }
});
document.getElementById('btn-clear-passthrough-toggle').addEventListener('click', async () => {
  if (!Number.isInteger(state.passthroughToggleMidiNote)) return;
  try {
    const response = await apiFetch('/api/midi/action/delete', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' },
      body: `note=${state.passthroughToggleMidiNote}`,
    });
    if (!response.ok) throw new Error(await response.text());
    await loadMidiActions();
  } catch (error) {
    setStatus(`Could not clear passthrough toggle: ${error}`, 'warn');
  }
});

document.getElementById('btn-vol-refresh').addEventListener('click', loadVolumes);
document.getElementById('vol-show-effects').addEventListener('change', (event) => {
  state.volShowEffects = event.currentTarget.checked;
  if (!state.volShowEffects && isEffectThingId(state.volAssignTarget)) {
    state.volAssignTarget = null;
    if (state.volCcPollTimer) clearInterval(state.volCcPollTimer);
    state.volCcPollTimer = null;
    if (volCcStatusEl) volCcStatusEl.textContent = 'Effect channel MIDI assignment cancelled.';
  }
  renderVolGrid();
});

document.getElementById('btn-delete-legacy-config').addEventListener('click', deleteLegacyConfig);
document.getElementById('btn-sampler-config-save').addEventListener('click', saveSamplerConfig);
document.getElementById('btn-sampler-config-load').addEventListener('click', loadSamplerConfig);

document.getElementById('btn-system-sync').addEventListener('click', async () => {
  await runSystemAction('/api/system/sync', 'sync');
});

const pi5FanEnabled = document.getElementById('pi5-fan-enabled');
const pi5FanStatus = document.getElementById('pi5-fan-status');
const pi5FanGraph = document.getElementById('pi5-fan-graph');
document.getElementById('btn-pi5-fan-open').addEventListener('click', openPi5FanDialog);
document.getElementById('btn-pi5-fan-close').addEventListener('click', () => {
  document.getElementById('pi5-fan-dialog').close();
});
pi5FanGraph.addEventListener('pointerdown', (event) => {
  const point = event.target.closest?.('[data-step]');
  if (!point || !state.pi5FanEditor.enabled || (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded)) return;
  state.pi5FanDragging = Number(point.getAttribute('data-step'));
  pi5FanGraph.setPointerCapture(event.pointerId);
  event.preventDefault();
});
pi5FanGraph.addEventListener('pointermove', (event) => {
  if (state.pi5FanDragging !== undefined) updatePi5FanPointFromPointer(event, state.pi5FanDragging);
});
const finishFanDrag = (event) => {
  if (state.pi5FanDragging === undefined) return;
  state.pi5FanDragging = undefined;
  if (pi5FanGraph.hasPointerCapture(event.pointerId)) pi5FanGraph.releasePointerCapture(event.pointerId);
};
pi5FanGraph.addEventListener('pointerup', finishFanDrag);
pi5FanGraph.addEventListener('pointercancel', finishFanDrag);
pi5FanGraph.addEventListener('keydown', (event) => {
  const point = event.target.closest?.('[data-step]');
  if (!point) return;
  const step = Number(point.getAttribute('data-step'));
  const precision = event.shiftKey ? 1 : 5;
  if (event.key === 'ArrowLeft') editPi5FanPoint(step, -1, 0, true);
  else if (event.key === 'ArrowRight') editPi5FanPoint(step, 1, 0, true);
  else if (event.key === 'ArrowUp') editPi5FanPoint(step, 0, precision, true);
  else if (event.key === 'ArrowDown') editPi5FanPoint(step, 0, -precision, true);
  else return;
  event.preventDefault();
});
pi5FanEnabled.addEventListener('change', () => {
  state.pi5FanEditor.enabled = pi5FanEnabled.checked;
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
});
document.getElementById('pi5-fan-hysteresis').addEventListener('input', (event) => {
  state.pi5FanEditor.hysteresis = Number(event.currentTarget.value);
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
});
document.getElementById('btn-pi5-fan-save').addEventListener('click', async () => {
  if (!state.pi5FanIsPi5 || !state.pi5FanSettingsLoaded) {
    pi5FanStatus.textContent = 'Fan curves can only be saved from a Raspberry Pi 5.';
    return;
  }
  const curve = state.pi5FanEditor;
  if (!validFanCurve(curve)) {
    pi5FanStatus.textContent = 'Invalid curve. Temperatures and speeds must increase at every step.';
    return;
  }
  const settings = { enabled: curve.enabled ? 1 : 0 };
  for (let i = 0; i < 4; i += 1) {
    settings[`temperature${i}`] = curve.temperatures[i];
    settings[`speed${i}`] = curve.speeds[i];
  }
  settings.hysteresis = curve.hysteresis;
  try {
    const response = await apiFetch('/api/system/fan', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams(settings).toString(),
    });
    const text = (await response.text()).trim();
    if (!response.ok) throw new Error(`${response.status} ${text}`);
    const saved = JSON.parse(text);
    if (!validFanCurve(saved)) throw new Error('device returned an invalid fan curve');
    state.pi5FanEditor = {
      enabled: saved.enabled,
      temperatures: saved.temperatures,
      speeds: saved.speeds,
      hysteresis: saved.hysteresis,
    };
    state.pi5FanSettingsLoaded = true;
    AudioxUI.setUnsaved('pi5-fan', false);
    renderPi5FanCurve();
    pi5FanStatus.textContent = saved.enabled
      ? 'Fan curve saved to boot config. Restart the device to apply it.'
      : 'AudioX curve removed. Firmware defaults or other boot-config fan settings will apply after restart.';
  } catch (error) {
    pi5FanStatus.textContent = `Fan curve save failed: ${error.message}`;
  }
});

document.getElementById('btn-system-restart').addEventListener('click', async () => {
  if (!await AudioxUI.confirm('Restart AudioX? All audio will stop while the device reboots. Save any pending changes first.', {
    title: 'Restart device?', label: 'Restart device',
  })) return;
  await runSystemAction('/api/system/restart', 'restart');
});

document.getElementById('btn-system-shutdown').addEventListener('click', async () => {
  if (!await AudioxUI.confirm('Shut down AudioX? All audio will stop. You will need physical access to power the device back on.', {
    title: 'Shut down device?', label: 'Shut down',
  })) return;
  await runSystemAction('/api/system/shutdown', 'shutdown');
});

async function requestAudioStress(mode) {
  const duration = Number(document.getElementById('audio-stress-duration').value);
  const workMs = Number(document.getElementById('audio-stress-work').value);
  if (mode !== 'stop' && !await AudioxUI.confirm(
    mode === 'audio'
      ? 'This intentionally overloads the audio thread and can cause audible dropouts. Start the bounded test?'
      : 'This runs CPU workers on all available cores (up to 8) and can cause system-wide audio dropouts. Start the bounded test?',
    { title: 'Run audio stress test?', label: 'Start stress test' })) return;

  const fields = { mode };
  if (mode !== 'stop') fields.duration_secs = String(duration);
  if (mode === 'audio') fields.work_ms = String(workMs);
  try {
    const response = await apiFetch('/api/system/audio-stress', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: new URLSearchParams(fields).toString(),
      signal: AbortSignal.timeout(10000),
    });
    if (!response.ok) throw new Error((await response.text()).trim() || `HTTP ${response.status}`);
    setStatus(mode === 'stop' ? 'Audio stress test stopped.' : 'Audio stress test started.', 'warn', 'audio-stress');
    await loadSystemInfo();
  } catch (error) {
    setStatus(`audio stress test failed: ${error.message}`, 'warn', 'audio-stress');
  }
}

document.getElementById('btn-audio-stress-thread').addEventListener('click', () => requestAudioStress('audio'));
document.getElementById('btn-audio-stress-cpu').addEventListener('click', () => requestAudioStress('cpu'));
document.getElementById('btn-audio-stress-stop').addEventListener('click', () => requestAudioStress('stop'));

AudioxUpdates.init({
  setStatus,
  ui: AudioxUI,
  showUpdates: () => {
    setActiveTab('system');
    document.getElementById('update-card')?.scrollIntoView({ behavior: 'smooth', block: 'start' });
  },
});
AudioxBackup.init({ setStatus, ui: AudioxUI });
AudioxTelemetry.init();


const lightDimButton = document.getElementById('btn-light-dim');
let lightDimBusy = false;

function renderLightDim(dimmed) {
  lightDimButton.textContent = dimmed ? 'Restore lights' : 'Dim lights';
  lightDimButton.setAttribute('aria-pressed', String(dimmed));
  lightDimButton.disabled = false;
}

async function requestLightDim(dimmed) {
  const res = await apiFetch('/api/midi/light_dim', dimmed === undefined
    ? { cache: 'no-store', signal: AbortSignal.timeout(10000) }
    : {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `dimmed=${dimmed ? 1 : 0}`,
      signal: AbortSignal.timeout(10000),
    });
  if (!res.ok) throw new Error(`MIDI lights request failed: ${res.status} ${await res.text()}`);
  const data = await res.json();
  if (data.ok !== true || typeof data.dimmed !== 'boolean') {
    throw new Error('MIDI lights returned an unexpected response. Try again.');
  }
  renderLightDim(data.dimmed);
  return data.dimmed;
}

async function loadLightDim() {
  if (lightDimBusy) return;
  lightDimBusy = true;
  lightDimButton.disabled = true;
  try {
    await requestLightDim();
    AudioxUI.resolve('midi-light-dim');
  } catch (err) {
    setStatus(`MIDI lights load failed: ${err}`, 'warn', 'midi-light-dim');
  } finally {
    lightDimBusy = false;
  }
}

lightDimButton.addEventListener('click', async () => {
  if (lightDimBusy) return;
  const dimmed = lightDimButton.getAttribute('aria-pressed') !== 'true';
  lightDimBusy = true;
  lightDimButton.disabled = true;
  try {
    const result = await requestLightDim(dimmed);
    setStatus(result ? 'MIDI lights dimmed until restored or rebooted.' : 'MIDI lights restored.', 'ok', 'midi-light-dim');
  } catch (err) {
    setStatus(`MIDI lights change failed: ${err}`, 'warn', 'midi-light-dim');
  } finally {
    lightDimButton.disabled = false;
    lightDimBusy = false;
  }
});

async function loadLightConfig() {
  try {
    const res = await apiFetch('/api/midi/light_config', { method: 'GET' });
    if (!res.ok) return;
    const data = JSON.parse(await res.text());
    document.getElementById('light_channel').value = String(data.channel ?? 0);
    document.getElementById('light_mapped_vel').value = String(data.mapped_vel ?? 0);
    document.getElementById('light_stop_all_vel').value = String(data.stop_all_vel ?? 0);
    document.getElementById('light_playing_vel').value = String(data.playing_vel ?? 0);
    AudioxUI.setUnsaved('light-config', false);
  } catch (err) {}
}

async function saveLightConfig() {
  const channel = Number(document.getElementById('light_channel').value) || 0;
  const mappedVel = Number(document.getElementById('light_mapped_vel').value) || 0;
  const stopAllVel = Number(document.getElementById('light_stop_all_vel').value) || 0;
  const playingVel = Number(document.getElementById('light_playing_vel').value) || 0;

  setStatus('saving lighting config...');
  try {
    const body = `channel=${channel}&mapped_vel=${mappedVel}&playing_vel=${playingVel}&stop_all_vel=${stopAllVel}`;
    const res = await apiFetch('/api/midi/light_config', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`lighting save failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    setStatus('lighting config saved', 'ok');
    AudioxUI.setUnsaved('light-config', false);
  } catch (err) {
    setStatus(`lighting save error: ${err}`, 'warn');
  }
}

document.getElementById('btn-light-save').addEventListener('click', saveLightConfig);

document.getElementById('btn-light-refresh').addEventListener('click', async () => {
  setStatus('refreshing LEDs...');
  try {
    const res = await apiFetch('/api/midi/light_refresh', { method: 'POST' });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`LED refresh failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    setStatus('LEDs refreshed', 'ok');
  } catch (err) {
    setStatus(`LED refresh error: ${err}`, 'warn');
  }
});

async function loadBootFailure() {
  const bannerEl = document.getElementById('boot-failure-banner');
  try {
    const res = await apiFetch('/api/system/boot-failure', { cache: 'no-store' });
    if (res.status !== 200) {
      bannerEl.hidden = true;
      return;
    }
    const report = await res.text();
    const field = (name) => {
      const match = report.match(new RegExp(`^${name}=(.*)$`, 'm'));
      return match ? match[1] : 'unknown';
    };
    const reverted = field('reverted') === 'yes';
    const summary = reverted
      ? `${field('failed_version')} failed ${field('attempts')} boot attempt(s); restored ${field('restored_version')}.`
      : `${field('failed_version')} failed ${field('attempts')} boot attempt(s); no backup was available.`;
    document.getElementById('boot-failure-title').textContent = reverted
      ? 'Last update failed to boot and was reverted.'
      : 'Last update failed to boot.';
    document.getElementById('boot-failure-summary').textContent = summary;
    document.getElementById('boot-failure-report').textContent = report;
    bannerEl.hidden = false;
  } catch (err) {
    bannerEl.hidden = true;
  }
}

document.getElementById('btn-boot-failure-view').addEventListener('click', () => {
  const reportEl = document.getElementById('boot-failure-report');
  reportEl.hidden = !reportEl.hidden;
  document.getElementById('btn-boot-failure-view').textContent = reportEl.hidden ? 'View report' : 'Hide report';
});

document.getElementById('btn-boot-failure-dismiss').addEventListener('click', async () => {
  try {
    const res = await apiFetch('/api/system/boot-failure', { method: 'DELETE' });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`dismiss failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    document.getElementById('boot-failure-banner').hidden = true;
    setStatus('boot failure report dismissed', 'ok');
  } catch (err) {
    setStatus(`dismiss error: ${err}`, 'warn');
  }
});

initializeGraphFromConfig();
const stopConnectionMonitor = AudioxUI.startConnectionMonitor(() => {
  loadLightDim();
  if (state.activeTab === 'bluetooth') loadBluetoothDevices();
  if (state.activeTab === 'system') pollSystemIfNeeded();
}, showAudioHealth);
loadVersion();
loadBootFailure();
loadLightConfig();
loadLightDim();
loadSamplerConfig();
