const edgeListEl = document.getElementById('edge-list');
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
const inspectorEl = document.getElementById('route-inspector');
const nodeCountEl = document.getElementById('metric-node-count');
const routeCountEl = document.getElementById('metric-route-count');
const selectedNodeEl = document.getElementById('metric-selected-node');
const selectedRouteEl = document.getElementById('metric-selected-route');
const logsSourceEl = document.getElementById('logs-source');
const logsLevelEl = document.getElementById('logs-level');
const logsTextEl = document.getElementById('logs-text');
const logsLimitEl = document.getElementById('logs-limit');
const logsSummaryEl = document.getElementById('logs-summary');
const logsViewEl = document.getElementById('logs-view');
const bluetoothStatusEl = document.getElementById('bluetooth-status');
const bluetoothDevicesEl = document.getElementById('bluetooth-devices');

const configPath = '/api/rootfs/config.staging.txt';
const realConfigPath = '/api/rootfs/config.txt';
const routingPath = '/api/rootfs/routing.txt';

const state = {
  nodes: [],
  edges: [],
  // Routes to Bluetooth devices that are not currently paired. They stay in
  // routing.txt untouched so the routing comes back when the device returns.
  dormantEdges: [],
  positions: {},
  selectedNodeId: null,
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
  nodeByThingId: new Map(),
  suspendGraphSync: false,
  internalGraphMutation: false,
  thingsPollTimer: null,
  thingsPollInFlight: false,
  thingsSignature: '',
  effectsPollInFlight: false,
  soundboardModesBySfx: {},
  framebufferView: 'meters',
  stopAllMidiNote: null,
  samplerToggleMidiNote: null,
  samplerKeyboardEnabled: true,
  samplerKeyboardChannel: 0,
  samplerRootNote: 60,
  // Volume page
  volumes: [],         // [{id, name, gain}] from /api/audio/volumes
  ccVolumes: [],       // [{cc, id}] from /api/midi/cc_volumes
  volAssignTarget: null,
  volCcPollTimer: null,
  volCcLastSeq: 0,
  systemPollTimer: null,
  systemPollInFlight: false,
};

const soundboardOrder = AudioxSoundboardOrder.attach(sbGridEl, {
  enabled: () => state.soundboardEditing && state.soundboardOrderSupported &&
    !state.soundboardOrderSaving && !AudioxUI.isBusy(),
  save: saveSoundboardOrder,
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
const loadLogs = () => logViewer.load();
const showAudioHealth = AudioxLogs.createHealthNotifier(setStatus);

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

function setActiveTab(tab) {
  if (tab !== 'macros') {
    stopChordRecording();
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
  if (tab === 'macros') {
    loadMidiChords();
  }
  if (tab === 'midi') {
    loadLightDim();
    renderStopAllMidiStatus();
    renderSamplerToggleMidiStatus();
  }
  if (tab === 'effects' && effectsManager) {
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
      ? 'Connected as audio output; paused while idle' : 'Connected as audio output');
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
    const res = await fetch('/api/bluetooth/devices', { method: 'GET', signal: AbortSignal.timeout(10000) });
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
  bluetoothStatusEl.textContent = 'Working on your device request...';
  try {
    const res = await fetch(path, { method: 'POST', body });
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
  const center = [graphCanvasEl.width * 0.5, graphCanvasEl.height * 0.5];
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

function renderInspector() {
  nodeCountEl.textContent = String(state.nodes.length);
  routeCountEl.textContent = String(state.edges.length);
  selectedNodeEl.textContent = state.selectedNodeId || '-';

  if (state.selectedEdgeIdx >= 0 && state.selectedEdgeIdx < state.edges.length) {
    const edge = state.edges[state.selectedEdgeIdx];
    selectedRouteEl.textContent = `${edge.src}[${edge.srcChannel}] -> ${edge.dst}[${edge.dstChannel}]`;
    inspectorEl.textContent = `Selected route: ${edge.src}[${edge.srcChannel}] -> ${edge.dst}[${edge.dstChannel}]`;
    return;
  }

  selectedRouteEl.textContent = '-';
  if (state.selectedNodeId) {
    inspectorEl.textContent = `Selected node: ${state.selectedNodeId}.`;
    return;
  }

  inspectorEl.textContent = '';
}

function renderEdgesList() {
  edgeListEl.innerHTML = '';
  if (!state.edges.length) {
    edgeListEl.textContent = 'no routes';
    renderInspector();
    return;
  }

  state.edges.forEach((edge, idx) => {
    const row = document.createElement('div');
    row.className = `edge-item ${idx === state.selectedEdgeIdx ? 'selected' : ''}`.trim();

    const txt = document.createElement('div');
    txt.textContent = `${edge.src}[${edge.srcChannel}] -> ${edge.dst}[${edge.dstChannel}]`;

    const btn = document.createElement('button');
    btn.textContent = 'remove';
    btn.addEventListener('click', (ev) => {
      ev.stopPropagation();
      removeEdgeByIndex(idx);
    });

    row.appendChild(txt);
    row.appendChild(btn);
    row.addEventListener('click', () => {
      state.selectedEdgeIdx = (state.selectedEdgeIdx === idx) ? -1 : idx;
      renderEdgesList();
      renderInspector();
    });
    edgeListEl.appendChild(row);
  });

  renderInspector();
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
  const res = await fetch('/api/routing/helper/create', { method: 'POST' });
  const txt = await res.text();
  if (!res.ok) {
    throw new Error(`routing helper create failed: ${res.status} ${txt.trim()}`);
  }
}

async function deleteRoutingHelper() {
  const res = await fetch('/api/routing/helper/delete', { method: 'POST' });
  const txt = await res.text();
  if (!res.ok) {
    throw new Error(`routing helper delete failed: ${res.status} ${txt.trim()}`);
  }
}

async function createEffectFromTemplate(type) {
  const allowed = new Set(['gain', 'distortion', 'pitch', 'reverb', 'gate', 'cut']);
  const safeType = allowed.has(String(type || '').trim()) ? String(type).trim() : 'gain';
  const res = await fetch('/api/effect/create', {
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
  if (!await AudioxUI.confirm(`Delete effect ${id}? Its settings and routing connections will be removed.`, {
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
    setStatus(`effect ${id} deleted`, 'ok');
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

function getInputChannelFromSlot(node, slot) {
  const map = node?._inputChannelBySlot;
  if (!Array.isArray(map)) {
    return Number(slot);
  }
  const channel = Number(map[slot]);
  if (Number.isFinite(channel) && channel >= 0) {
    return channel;
  }
  return Number(slot);
}

function getOutputChannelCount(node) {
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

  renderEdgesList();
  state.graphCanvas.setDirty(true, true);
}

function addEdgeFromConnection(srcNode, srcSlot, dstNode, dstSlot) {
  const src = normalizeThingId(srcNode?.properties?.thingId);
  const dst = normalizeThingId(dstNode?.properties?.thingId);
  const srcChannel = Number(srcSlot);
  const dstChannel = Number(getInputChannelFromSlot(dstNode, dstSlot));

  if (!src || !dst || !Number.isFinite(srcChannel) || !Number.isFinite(dstChannel)) {
    return false;
  }

  const edge = { src, dst, srcChannel, dstChannel };
  if (findEdgeIndex(edge) >= 0) {
    setStatus('route already exists', 'warn');
    return false;
  }

  state.edges.push(edge);
  AudioxUI.setUnsaved('routing');
  syncEdgesFromGraph();
  renderEdgesList();
  setStatus('route added (not saved yet)', 'ok');
  return true;
}

function getViewCenterGraphCoords() {
  if (!state.graphCanvas) {
    return [0, 0];
  }

  const area = state.graphCanvas.visible_area;
  if (Array.isArray(area) && area.length >= 4) {
    return [area[0] + (area[2] * 0.5), area[1] + (area[3] * 0.5)];
  }

  const scale = state.graphCanvas.ds.scale;
  const offset = state.graphCanvas.ds.offset;
  return [
    ((graphCanvasEl.width * 0.5) - offset[0]) / scale,
    ((graphCanvasEl.height * 0.5) - offset[1]) / scale,
  ];
}

function resizeRoutingCanvas() {
  if (!state.graphCanvas || !graphWrapEl) {
    return;
  }

  const rect = graphWrapEl.getBoundingClientRect();
  const w = Math.max(1, Math.floor(rect.width));
  const h = Math.max(1, Math.floor(rect.height));

  // Keep canvas backing store and CSS size in lockstep to avoid pointer offset drift.
  if (graphCanvasEl.width !== w) {
    graphCanvasEl.width = w;
  }
  if (graphCanvasEl.height !== h) {
    graphCanvasEl.height = h;
  }
  graphCanvasEl.style.width = `${w}px`;
  graphCanvasEl.style.height = `${h}px`;

  if (state.graphCanvas.bgcanvas) {
    if (state.graphCanvas.bgcanvas.width !== w) {
      state.graphCanvas.bgcanvas.width = w;
    }
    if (state.graphCanvas.bgcanvas.height !== h) {
      state.graphCanvas.bgcanvas.height = h;
    }
    state.graphCanvas.bgcanvas.style.width = `${w}px`;
    state.graphCanvas.bgcanvas.style.height = `${h}px`;
  }

  state.graphCanvas.resize(w, h);
  state.graphCanvas.setDirty(true, true);
}

function bezierPoint(a, b, t) {
  const c = Math.max(40, Math.min(220, Math.abs(b[0] - a[0]) * 0.45));
  const p0x = a[0];
  const p0y = a[1];
  const p1x = a[0] + c;
  const p1y = a[1];
  const p2x = b[0] - c;
  const p2y = b[1];
  const p3x = b[0];
  const p3y = b[1];
  const it = 1 - t;

  return [
    (it * it * it * p0x) + (3 * it * it * t * p1x) + (3 * it * t * t * p2x) + (t * t * t * p3x),
    (it * it * it * p0y) + (3 * it * it * t * p1y) + (3 * it * t * t * p2y) + (t * t * t * p3y),
  ];
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

function linkDistance(link, x, y) {
  const srcNode = state.nodeByThingId.get(link.src);
  const dstNode = state.nodeByThingId.get(link.dst);
  if (!srcNode || !dstNode) {
    return Number.POSITIVE_INFINITY;
  }

  const a = srcNode.getConnectionPos(false, Number(link.srcChannel));
  const b = dstNode.getConnectionPos(true, Number(link.dstChannel));
  if (!a || !b) {
    return Number.POSITIVE_INFINITY;
  }

  let best = Number.POSITIVE_INFINITY;
  let prev = bezierPoint(a, b, 0);
  for (let i = 1; i <= 24; i += 1) {
    const t = i / 24;
    const next = bezierPoint(a, b, t);
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
  let bestIdx = -1;
  let bestDist = Number.POSITIVE_INFINITY;

  for (let i = 0; i < state.edges.length; i += 1) {
    const d = linkDistance(state.edges[i], x, y);
    if (d < bestDist) {
      bestDist = d;
      bestIdx = i;
    }
  }

  if (bestIdx < 0 || bestDist > threshold) {
    return false;
  }

  state.edges.splice(bestIdx, 1);
  AudioxUI.setUnsaved('routing');
  syncEdgesFromGraph();
  setStatus('route removed (not saved yet)', 'ok');
  return true;
}

function removeEdgeByIndex(index) {
  if (index < 0 || index >= state.edges.length) {
    return;
  }

  state.edges.splice(index, 1);

  AudioxUI.setUnsaved('routing');
  syncEdgesFromGraph();
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

    graphRemove(node);
  };
  state.graphCanvas = new window.LGraphCanvas(graphCanvasEl, state.graph, { autoresize: false });

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

  if (!window.LiteGraph.registered_node_types['audiox/effect_gain']) {
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
    window.LiteGraph.registerNodeType('audiox/effect_gain', AudioxEffectGainNode);
  }

  if (!window.LiteGraph.registered_node_types['audiox/routing_helper']) {
    function AudioxRoutingHelperNode() {
      this.size = [220, 80];
      this.properties = { routingHelperTemplate: true, nodeKind: 'template' };
      this.title = 'Add Routing Helper';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxRoutingHelperNode.title = 'Routing Helper';
    AudioxRoutingHelperNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('audiox/routing_helper', AudioxRoutingHelperNode);
  }

  if (!window.LiteGraph.registered_node_types['audiox/effect_distortion']) {
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
    window.LiteGraph.registerNodeType('audiox/effect_distortion', AudioxEffectDistortionNode);
  }

  if (!window.LiteGraph.registered_node_types['audiox/effect_pitch']) {
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
    window.LiteGraph.registerNodeType('audiox/effect_pitch', AudioxEffectPitchNode);
  }

  if (!window.LiteGraph.registered_node_types['audiox/effect_reverb']) {
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
    window.LiteGraph.registerNodeType('audiox/effect_reverb', AudioxEffectReverbNode);
  }

  if (!window.LiteGraph.registered_node_types['audiox/effect_gate']) {
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
    window.LiteGraph.registerNodeType('audiox/effect_gate', AudioxEffectGateNode);
  }

  if (!window.LiteGraph.registered_node_types['audiox/effect_cut']) {
    function AudioxEffectCutNode() {
      this.size = [220, 80];
      this.properties = { effectTypeTemplate: 'cut', nodeKind: 'template' };
      this.title = 'Add Cut';
      this.addInput('in 1', 'audio');
      this.addInput('in 2', 'audio');
      this.addOutput('out 1', 'audio');
      this.addOutput('out 2', 'audio');
    }
    AudioxEffectCutNode.title = 'Cut';
    AudioxEffectCutNode.filter = 'audiox';
    window.LiteGraph.registerNodeType('audiox/effect_cut', AudioxEffectCutNode);
  }
  state.graphCanvas.background_image = null;
  window.LiteGraph.NODE_DEFAULT_COLOR = '#1f4a62';
  window.LiteGraph.NODE_DEFAULT_BGCOLOR = '#102635';
  window.LiteGraph.NODE_DEFAULT_BOXCOLOR = '#7fd1ff';
  window.LiteGraph.NODE_TEXT_COLOR = '#eaf5ff';
  window.LiteGraph.LINK_COLOR = '#79c8ff';
  window.LiteGraph.EVENT_LINK_COLOR = '#79c8ff';
  state.graphCanvas.default_link_color = '#79c8ff';
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
            if (!await AudioxUI.confirm('Delete the routing helper? Any connections through it will be removed.', {
              title: 'Delete routing helper?', label: 'Delete helper',
            })) return;
            try {
              await deleteRoutingHelper();
              await loadRoutingThingsWithOptions({ silent: true, force: true });
              await loadRoutingFile();
              setStatus('routing helper deleted', 'ok');
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

  state.graphCanvas.onNodeSelected = (node) => {
    state.selectedNodeId = normalizeThingId(node?.properties?.thingId);
    renderInspector();
  };

  state.graphCanvas.onNodeDeselected = () => {
    state.selectedNodeId = null;
    renderInspector();
  };

  state.graphCanvas.onNodeMoved = (node) => {
    const thingId = normalizeThingId(node?.properties?.thingId);
    if (!thingId) {
      return;
    }
    state.autoArrange = false;
    for (const [id, current] of state.nodeByThingId) {
      state.positions[id] = { x: Math.round(current.pos[0]), y: Math.round(current.pos[1]) };
    }
    state.graphCanvas.setDirty(true, true);
  };

  state.graph.onNodeAdded = (node) => {
    if (!node) {
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
          setStatus('routing helper created', 'ok');
        })
        .catch((err) => setStatus(String(err), 'warn'));
      return;
    }
    if (templateType === 'gain' ||
      templateType === 'distortion' ||
      templateType === 'pitch' ||
      templateType === 'reverb' ||
      templateType === 'gate' ||
      templateType === 'cut') {
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
    const selected = (state.selectedEdgeIdx >= 0 && state.selectedEdgeIdx < state.edges.length)
      ? edgeKey(state.edges[state.selectedEdgeIdx])
      : '';

    for (const edge of state.edges) {
      const srcNode = state.nodeByThingId.get(edge.src);
      const dstNode = state.nodeByThingId.get(edge.dst);
      if (!srcNode || !dstNode) {
        continue;
      }

      const a = srcNode.getConnectionPos(false, Number(edge.srcChannel));
      const b = dstNode.getConnectionPos(true, Number(edge.dstChannel));
      if (!a || !b) {
        continue;
      }

      const isSelected = selected && edgeKey(edge) === selected;
      const c = Math.max(40, Math.min(220, Math.abs(b[0] - a[0]) * 0.45));
      ctx.beginPath();
      ctx.moveTo(a[0], a[1]);
      ctx.bezierCurveTo(a[0] + c, a[1], b[0] - c, b[1], b[0], b[1]);
      ctx.strokeStyle = isSelected ? '#ffcf66' : '#79c8ff';
      ctx.lineWidth = isSelected ? 4 : 3;
      ctx.globalAlpha = isSelected ? 1 : 0.92;
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
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

function routingNodeBoxes() {
  return Array.from(state.nodeByThingId, ([id, node]) => ({
    id, kind: node.properties.nodeKind,
    inputs: node.inputs?.length || 0, outputs: node.outputs?.length || 0,
    width: node.size[0], height: node.size[1], x: node.pos[0], y: node.pos[1],
  }));
}

function fitRoutingGraph() {
  if (!state.graphCanvas) return;
  resizeRoutingCanvas();
  const view = AudioxRouting.fitNodes(routingNodeBoxes(), graphCanvasEl.width, graphCanvasEl.height);
  if (!view) return;
  state.graphCanvas.ds.scale = view.scale;
  state.graphCanvas.ds.offset[0] = view.offset[0];
  state.graphCanvas.ds.offset[1] = view.offset[1];
  state.graphCanvas.setDirty(true, true);
  updateZoomLabel();
}

function arrangeRoutingGraph() {
  const boxes = routingNodeBoxes();
  const positions = AudioxRouting.layoutNodes(boxes);
  let newNodeY = Math.max(60, ...boxes.filter((node) => state.positions[node.id])
    .map((node) => state.positions[node.id].y + node.height + 80));
  for (const [id, node] of state.nodeByThingId) {
    const pos = positions[id];
    if (state.autoArrange) {
      node.pos = [pos.x, pos.y];
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
    node._inputChannelBySlot = [];

    for (let ch = 0; ch < thing.inputs; ch += 1) {
      node.addInput(`in ${ch + 1}`, 'audio');
      node._inputChannelBySlot.push(ch);
    }
    for (let ch = 0; ch < thing.outputs; ch += 1) {
      node.addOutput(`out ${ch + 1}`, 'audio');
    }

    const pos = state.positions[thing.id] || { x: 40, y: 60 };
    node.pos = [pos.x, pos.y];

    node.onConnectInput = function(inputIndex, _outputType, _outputSlot, outputNode, outputIndex) {
      addEdgeFromConnection(outputNode, outputIndex, this, inputIndex);
      return false;
    };

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

  state.internalGraphMutation = false;
  state.suspendGraphSync = false;
  arrangeRoutingGraph();
  syncEdgesFromGraph();
  state.graphCanvas.setDirty(true, true);
}

function buildNodesFromThings(things) {
  const previous = { ...state.positions };
  state.nodes = [];
  state.positions = {};

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
    const res = await fetch('/api/routing/things', { method: 'GET' });
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
  }, 1000);
}

async function pollEffectsIfNeeded() {
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
    const res = await fetch(routingPath, { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      state.edges = [];
      state.dormantEdges = [];
      rebuildRoutingGraph();
      AudioxUI.setUnsaved('routing', false);
      setStatus('routing file missing, starting empty', 'warn');
      return;
    }

    state.edges = parseRoutingText(txt);
    state.dormantEdges = [];
    state.selectedEdgeIdx = -1;
    rebuildRoutingGraph();
    AudioxUI.setUnsaved('routing', false);
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
    const res = await fetch('/api/routing/reload', { method: 'POST' });
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
  setStatus('saving routing...');
  try {
    const res = await fetch(routingPath, {
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

    AudioxUI.setUnsaved('routing', false);
    return true;
  } catch (err) {
    setStatus(`routing save error: ${err}`, 'warn');
    return false;
  }
}

async function loadVersion() {
  try {
    const res = await fetch('/api/version', { method: 'GET' });
    const txt = (await res.text()).trim();
    if (!res.ok || !txt) {
      return;
    }
    appVersionEl.textContent = `version ${txt}`;
  } catch (err) {
  }
}

function renderStorageUsage(storage) {
  const value = document.getElementById('sysinfo-storage');
  const meter = document.getElementById('sysinfo-storage-meter');
  const free = document.getElementById('sysinfo-storage-free');
  const fields = ['total_bytes', 'used_bytes', 'free_bytes', 'reserved_bytes'];
  if (!storage || !fields.every((key) => Number.isSafeInteger(storage[key]) && storage[key] >= 0)
      || storage.total_bytes === 0
      || storage.used_bytes + storage.free_bytes + storage.reserved_bytes !== storage.total_bytes) {
    value.textContent = 'Unavailable';
    meter.hidden = true;
    free.textContent = 'Storage usage could not be read.';
    return;
  }
  const format = (bytes) => bytes >= 1024 ** 3
    ? `${(bytes / 1024 ** 3).toFixed(1)} GiB`
    : `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  value.textContent = `${format(storage.used_bytes)} / ${format(storage.total_bytes)} used`;
  meter.value = (storage.total_bytes - storage.free_bytes) / storage.total_bytes * 100;
  meter.hidden = false;
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
    const uEl = document.getElementById('sysinfo-uptime');
    const mEl = document.getElementById('sysinfo-memory');
    const lEl = document.getElementById('sysinfo-load');
    const fEl = document.getElementById('sysinfo-ffmpeg');
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

    const secs = Number(data.uptime_secs || 0);
    const d = Math.floor(secs / 86400);
    const h = Math.floor((secs % 86400) / 3600);
    const m = Math.floor((secs % 3600) / 60);
    uEl.textContent = d > 0 ? `${d}d ${h}h ${m}m` : `${h}h ${m}m`;

    const totalMb = Number(data.mem_total_mb || 0);
    const availMbRaw = Number(data.mem_avail_mb || 0);
    const availMb = Math.max(0, Math.min(totalMb, availMbRaw));
    const usedMb = Math.max(0, totalMb - availMb);
    mEl.textContent = `${usedMb} / ${totalMb} MB`;
    renderStorageUsage(data.storage);

    lEl.textContent = Number(data.load1 || 0).toFixed(2);

    if (fEl) {
      fEl.textContent = data.ffmpeg || '\u2014';
    }
    AudioxUI.resolve('system-info');
  } catch (err) {
    renderStorageUsage(null);
    setStatus(`device info load error: ${err}`, 'warn', 'system-info');
  }
}

async function loadConfig() {
  setStatus('loading config...');
  try {
    let res = await fetch(configPath, { method: 'GET' });
    let txt = await res.text();
    if (!res.ok) {
      res = await fetch(realConfigPath, { method: 'GET' });
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
    const res = await fetch('/api/framebuffer/view');
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
    const res = await fetch('/api/framebuffer/view', {
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
    const res = await fetch(configPath, {
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
let macroCount = 0;

async function loadMidiChords() {
  try {
    const res = await fetch('/api/macros', { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    renderMidiChords((await res.json()).macros);
  } catch (err) {
    setStatus(`Macro load failed: ${err}`, 'warn', 'midi-chords');
  }
}

function renderMidiChords(chords) {
  macroCount = chords.length;
  const list = document.getElementById('chord-list');
  list.replaceChildren();
  for (const chord of chords) {
    const row = document.createElement('div');
    const label = document.createElement('span');
    label.textContent = `${chord.name} — ${chord.midi_enabled ? `Channel ${chord.channel}, note ${chord.note}` : 'Web only'}: ${chord.script_hex ? 'Lua sequence' : chord.keys} ` +
      `(LED idle ${chord.idle_velocity ?? 0}, pressed ${chord.pressed_velocity ?? 0})`;
    if (chord.runtime_error_hex) {
      label.textContent += ` — Last execution failed: ${chordHexDecode(chord.runtime_error_hex)}`;
    }
    const edit = document.createElement('button');
    edit.textContent = 'Edit';
    edit.addEventListener('click', async () => {
      if (AudioxUI.hasUnsavedChanges('macro-editor') && !await AudioxUI.confirm(
        'Discard unsaved macro changes and edit this saved macro?', {
          title: 'Discard macro changes?', label: 'Discard and edit',
        })) return;
      stopChordRecording();
      cancelChordLearn();
      editingMacroId = chord.id;
      document.getElementById('macro-name').value = chord.name;
      document.getElementById('macro-midi-enabled').checked = chord.midi_enabled;
      document.getElementById('chord-channel').value = chord.channel;
      document.getElementById('chord-note').value = chord.note;
      document.getElementById('chord-keys').value = chord.keys;
      document.getElementById('chord-mode').value = chord.script_hex ? 'script' : 'chord';
      document.getElementById('chord-script').value = chordHexDecode(chord.script_hex || '');
      document.getElementById('chord-idle-velocity').value = chord.idle_velocity ?? 0;
      document.getElementById('chord-pressed-velocity').value = chord.pressed_velocity ?? 0;
      updateChordMode();
      updateMacroBinding();
      AudioxUI.setUnsaved('macro-editor', false);
      document.getElementById('btn-chord-save').textContent = 'Save Changes';
      document.getElementById('macro-name').focus();
    });
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.addEventListener('click', async () => {
      if (!await AudioxUI.confirm(`Delete the saved macro "${chord.name}"? This cannot be undone.`, {
        title: 'Delete macro?', label: 'Delete macro',
      })) return;
      await saveMidiChord('DELETE', chord, remove);
    });
    const run = document.createElement('button');
    run.textContent = 'Run';
    run.addEventListener('click', () => runMacro(chord.id, run));
    row.className = 'stack';
    const actions = document.createElement('div');
    actions.className = 'macro-actions';
    actions.append(edit, remove, run);
    row.append(label, actions);
    list.appendChild(row);
  }
}

async function saveMidiChord(method, chord, button) {
  if (button.disabled) return;
  button.disabled = true;
  try {
    const payload = method === 'DELETE' ? `id=${chord.id}` :
      (chord.id ? `id=${chord.id}&` : '') +
      `name_hex=${chordHexEncode(chord.name)}&midi_enabled=${chord.midi_enabled ? 1 : 0}` +
      `&channel=${chord.channel}&note=${chord.note}&keys=${chord.keys}` +
      `&idle_velocity=${chord.idle_velocity ?? 0}&pressed_velocity=${chord.pressed_velocity ?? 0}` +
      `&script_hex=${chord.script_hex || ''}`;
    const res = await fetch('/api/macros', {
      method, headers: { 'Content-Type': 'text/plain' }, body: payload,
    });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    if (method === 'POST' || editingMacroId === chord.id) resetMacroEditor();
    setStatus(method === 'DELETE' ? 'Macro removed' : 'Macro saved', 'ok', 'midi-chords');
    await loadMidiChords();
  } catch (err) {
    setStatus(`Macro save failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = false;
  }
}

async function runMacro(id, button) {
  if (button.disabled) return;
  button.disabled = true;
  try {
    const res = await fetch('/api/macros/run', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: `id=${id}`,
    });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    setStatus('Macro queued (not confirmation of completion)', 'ok', 'midi-chords');
    await loadMidiChords();
  } catch (err) {
    setStatus(`Macro run failed: ${err}`, 'warn', 'midi-chords');
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
  stopChordRecording();
  cancelChordLearn();
  editingMacroId = null;
  document.getElementById('macro-name').value = '';
  document.getElementById('macro-midi-enabled').checked = false;
  document.getElementById('chord-channel').value = '0';
  document.getElementById('chord-note').value = '60';
  document.getElementById('chord-keys').value = '';
  document.getElementById('chord-script').value = '';
  document.getElementById('chord-mode').value = 'chord';
  document.getElementById('chord-idle-velocity').value = '0';
  document.getElementById('chord-pressed-velocity').value = '0';
  document.getElementById('btn-chord-save').textContent = 'Save Macro';
  AudioxUI.setUnsaved('macro-editor', false);
  updateChordMode();
  updateMacroBinding();
}

function chordHexEncode(text) {
  return Array.from(new TextEncoder().encode(text), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function chordHexDecode(hex) {
  return new TextDecoder().decode(Uint8Array.from(hex.match(/../g) || [], (byte) => parseInt(byte, 16)));
}

function updateChordMode() {
  stopChordRecording();
  const script = document.getElementById('chord-mode').value === 'script';
  document.getElementById('chord-capture-editor').hidden = script;
  document.getElementById('chord-script-editor').hidden = !script;
}

let chordRecording = null;

function stopChordRecording(keep = false) {
  if (!chordRecording) return;
  const input = document.getElementById('chord-keys');
  if (!keep) input.value = chordRecording.previous;
  input.readOnly = false;
  chordRecording = null;
  const button = document.getElementById('btn-chord-record');
  button.textContent = 'Record Chord';
  button.setAttribute('aria-pressed', 'false');
  document.getElementById('chord-capture-status').textContent = keep ?
    `Captured ${input.value}. Save Macro to apply.` : 'Recording cancelled. Previous chord restored.';
}

function chordKeyFromCode(code) {
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  if (/^F(?:[1-9]|1[0-9]|2[0-4])$/.test(code)) return code;
  return {
    Enter: 'ENTER', Escape: 'ESC', Backspace: 'BACKSPACE', Tab: 'TAB', Space: 'SPACE',
    Delete: 'DELETE', ArrowRight: 'RIGHT', ArrowLeft: 'LEFT', ArrowDown: 'DOWN', ArrowUp: 'UP',
    Home: 'HOME', End: 'END', PageUp: 'PAGEUP', PageDown: 'PAGEDOWN',
  }[code] || null;
}

document.getElementById('btn-chord-record').addEventListener('click', () => {
  if (chordRecording) {
    stopChordRecording();
    return;
  }
  const input = document.getElementById('chord-keys');
  chordRecording = { previous: input.value, keys: [], held: new Set(), modifiers: new Set() };
  input.readOnly = true;
  const button = document.getElementById('btn-chord-record');
  button.textContent = 'Cancel Recording';
  button.setAttribute('aria-pressed', 'true');
  document.getElementById('chord-capture-status').textContent = 'Listening: press your shortcut, then release its keys.';
  input.focus();
});
document.getElementById('btn-chord-record').addEventListener('pointerdown', (event) => {
  if (chordRecording) event.preventDefault();
});

document.getElementById('chord-keys').addEventListener('keydown', (event) => {
  if (!chordRecording) return;
  event.preventDefault();
  event.stopPropagation();
  if (event.repeat || event.isComposing) return;
  if (event.ctrlKey) chordRecording.modifiers.add('CTRL');
  if (event.shiftKey) chordRecording.modifiers.add('SHIFT');
  if (event.altKey) chordRecording.modifiers.add('ALT');
  if (event.metaKey) chordRecording.modifiers.add('GUI');
  if (/^(Control|Shift|Alt|Meta)(Left|Right)$/.test(event.code)) return;
  const key = chordKeyFromCode(event.code);
  if (!key || (chordRecording.keys.length === 6 && !chordRecording.keys.includes(key))) {
    stopChordRecording();
    document.getElementById('chord-capture-status').textContent = 'Unsupported key or more than six keys. Previous chord restored; record again to retry.';
    return;
  }
  chordRecording.held.add(event.code);
  if (!chordRecording.keys.includes(key)) chordRecording.keys.push(key);
  const keys =
    ['CTRL', 'SHIFT', 'ALT', 'GUI'].filter((modifier) => chordRecording.modifiers.has(modifier))
      .concat(chordRecording.keys).join('+');
  if (keys.length > 63) {
    stopChordRecording();
    document.getElementById('chord-capture-status').textContent = 'Chord exceeds 63 characters. Previous chord restored.';
    return;
  }
  document.getElementById('chord-keys').value = keys;
  AudioxUI.setUnsaved('macro-editor');
});

document.getElementById('chord-keys').addEventListener('keyup', (event) => {
  if (!chordRecording) return;
  event.preventDefault();
  event.stopPropagation();
  chordRecording.held.delete(event.code);
  if (chordRecording.keys.length && !chordRecording.held.size) stopChordRecording(true);
});
document.getElementById('chord-keys').addEventListener('blur', () => stopChordRecording());
window.addEventListener('blur', () => stopChordRecording());
document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopMeterPolling();
  else if (state.activeTab === 'volume') startMeterPolling();
  if (document.hidden) {
    stopChordRecording();
    cancelChordLearn();
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
    const res = await fetch('/api/midi/last_note', { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const data = await res.json();
    if (state.activeTab !== 'macros' || document.hidden ||
        !document.getElementById('macro-midi-enabled').checked) return;
    const seq = Number(data.last_seq);
    if (!Number.isInteger(seq) || seq < 0) throw new Error('device did not report a valid MIDI event sequence');
    state.mappingLastSeq = seq;
    state.mappingAssignTarget = '__macro__';
    if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
    button.setAttribute('aria-pressed', 'true');
    document.getElementById('chord-learn-status').textContent = 'Press the desired MIDI pad. Click Learn MIDI Pad again to cancel.';
  } catch (err) {
    setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = !document.getElementById('macro-midi-enabled').checked;
  }
});

document.getElementById('chord-mode').addEventListener('change', updateChordMode);
document.getElementById('macro-midi-enabled').addEventListener('change', updateMacroBinding);
document.getElementById('btn-macro-new').addEventListener('click', async () => {
  if (AudioxUI.hasUnsavedChanges('macro-editor') && !await AudioxUI.confirm(
    'Discard unsaved macro changes and start a new macro?', {
      title: 'Discard macro changes?', label: 'Discard and start new',
    })) return;
  resetMacroEditor();
});
document.getElementById('btn-chord-load').addEventListener('click', loadMidiChords);
document.getElementById('btn-chord-save').addEventListener('click', () => {
  if (chordRecording) {
    setStatus('Finish or cancel recording before saving.', 'warn', 'midi-chords');
    return;
  }
  const channel = Number(document.getElementById('chord-channel').value);
  const name = document.getElementById('macro-name').value.trim();
  const midi_enabled = document.getElementById('macro-midi-enabled').checked;
  const note = Number(document.getElementById('chord-note').value);
  const keys = document.getElementById('chord-keys').value.trim().toUpperCase();
  const script = document.getElementById('chord-mode').value === 'script' ?
    document.getElementById('chord-script').value : '';
  const idle = Number(document.getElementById('chord-idle-velocity').value);
  const pressed = Number(document.getElementById('chord-pressed-velocity').value);
  if (!name || name.includes('\0') || new TextEncoder().encode(name).length > 63 ||
      (!editingMacroId && macroCount >= 16) ||
      !Number.isInteger(channel) || channel < 0 || channel > 15 ||
      !Number.isInteger(note) || note < 0 || note > 127 ||
      !Number.isInteger(idle) || idle < 0 || idle > 127 ||
      !Number.isInteger(pressed) || pressed < 0 || pressed > 127 ||
      (document.getElementById('chord-mode').value === 'script' ? !script.trim() :
        (!/^[A-Z0-9]+(?:\+[A-Z0-9]+)*$/.test(keys) || keys.length > 63)) ||
      new TextEncoder().encode(script).length >= 2048) {
    setStatus('Enter a name (1-63 UTF-8 bytes), channel 0-15, note/LED values 0-127, and a chord (up to 63 characters) or Lua script (up to 2,047 UTF-8 bytes). Maximum 16 macros.', 'warn', 'midi-chords');
    return;
  }
  return saveMidiChord('POST', { id: editingMacroId, name, midi_enabled, channel, note, keys: script ? '' : keys, idle_velocity: idle,
    pressed_velocity: pressed, script_hex: chordHexEncode(script) }, document.getElementById('btn-chord-save'));
});

async function reloadConfig() {
  setStatus('reloading config...');
  try {
    const res = await fetch('/api/config/reload', { method: 'POST' });
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
    const res = await fetch(endpoint, { method: 'POST' });
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
    const res = await fetch('/api/soundboard/stop_all', { method: 'POST' });
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
    const res = await fetch('/api/soundboard/modes', { method: 'GET' });
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

async function saveSoundboardModeForSfx(sfx, mode) {
  const normalizedMode = (mode === 'hold') ? 'hold' : 'play';
  setStatus(`saving mode for ${sfx}...`);
  try {
    const res = await fetch('/api/soundboard/mode/set', {
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
        res = await fetch(path, { method: 'POST', signal: AbortSignal.timeout(15000) });
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
    let res = await fetch('/api/soundboard/order', { method: 'GET' });
    state.soundboardOrderSupported = res.status !== 404;
    if (!state.soundboardOrderSupported) {
      setStatus('Update the device firmware to save a shared soundboard order. Showing the existing sound list for now.', 'warn', 'soundboard-order-support');
      res = await fetch('/api/rootfs/sfx/', { method: 'GET' });
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
      const res = await fetch('/api/soundboard/order', {
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
    const res = await fetch('/api/soundboard/reload', { method: 'POST' });
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

function renderSoundboardGrid() {
  soundboardOrder.cancel();
  sbGridEl.classList.toggle('can-reorder', state.soundboardEditing && state.soundboardOrderSupported &&
    !state.soundboardOrderSaving);
  sbGridEl.innerHTML = '';
  if (!state.sfxFiles.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = 'No sounds uploaded yet.';
    sbGridEl.appendChild(empty);
    return;
  }

  for (const file of state.sfxFiles) {
    const mappedNote = state.midiMappingsBySfx[file];
    const hasMidi = mappedNote !== undefined;
    const isTarget = state.mappingAssignTarget === file;
    const mode = (state.soundboardModesBySfx[file] === 'hold') ? 'hold' : 'play';

    const card = document.createElement('div');
    card.className = `sb-card${isTarget ? ' assign-target' : ''}`;
    card.dataset.file = file;

    const nameEl = document.createElement('div');
    nameEl.className = 'sb-card-name';
    nameEl.title = file;
    nameEl.textContent = file;
    nameEl.tabIndex = state.soundboardEditing && state.soundboardOrderSupported ? 0 : -1;
    if (state.soundboardEditing && state.soundboardOrderSupported) {
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
        deleteMapping(mappedNote);
      });
      midiEl.appendChild(noteSpan);
      midiEl.appendChild(clearBtn);
    } else {
      midiEl.textContent = 'no MIDI';
    }

    const actionsEl = document.createElement('div');
    actionsEl.className = 'sb-card-actions';

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
          const r = await fetch('/api/midi/last_note', { method: 'GET' });
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

    card.appendChild(nameEl);
    card.appendChild(midiEl);
    card.appendChild(actionsEl);
    sbGridEl.appendChild(card);
  }
}

async function deleteSfxFile(filename) {
  if (!await AudioxUI.confirm(`Delete "${filename}" from the soundboard? This cannot be undone.`, {
    title: 'Delete sound?', label: 'Delete sound',
  })) return;
  setStatus(`deleting ${filename}...`);
  try {
    const res = await fetch(`/api/rootfs/sfx/${encodeURIComponent(filename)}`, {
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
    const res = await fetch(`/api/rootfs/sfx/${encodeURIComponent(filename)}`, {
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
    const res = await fetch(`/api/rootfs/sfx/${targetName}`, {
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
    await reloadSoundboardBank();
    AudioxUI.setUnsaved('sound-upload', false);
    setStatus(`uploaded ${targetName}`, 'ok');
  } catch (err) {
    setStatus(`upload error: ${err}`, 'warn');
  }
}

async function loadMappings() {
  try {
    const res = await fetch('/api/midi/mappings', { method: 'GET' });
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
    const res = await fetch('/api/midi/actions', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      state.stopAllMidiNote = null;
      state.samplerToggleMidiNote = null;
      renderStopAllMidiStatus();
      renderSamplerToggleMidiStatus();
      return;
    }

    const data = JSON.parse(txt);
    state.stopAllMidiNote = null;
    state.samplerToggleMidiNote = null;
    const actions = Array.isArray(data?.actions) ? data.actions : [];
    for (const item of actions) {
      const action = String(item?.action || '');
      if (action !== 'stop_all' && action !== 'sampler_toggle') {
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
      }
    }
    renderStopAllMidiStatus();
    renderSamplerToggleMidiStatus();
  } catch (_) {
    state.stopAllMidiNote = null;
    state.samplerToggleMidiNote = null;
    renderStopAllMidiStatus();
    renderSamplerToggleMidiStatus();
  }
}

async function saveActionMapping(note, actionName) {
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }

  if (!actionName) {
    setStatus('invalid action mapping', 'warn');
    return;
  }

  try {
    const body = `note=${note}&action=${actionName}`;
    const res = await fetch('/api/midi/action/set', {
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
    const res = await fetch('/api/midi/action/delete', {
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
    const res = await fetch('/api/midi/action/delete', {
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
    const res = await fetch('/api/midi/mapping/set', {
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
    const res = await fetch('/api/midi/mapping/delete', {
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
    const res = await fetch('/api/midi/last_note', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      if (targetAtStart === '__macro__') throw new Error(`${res.status}: ${txt}`);
      return;
    }
    if (targetAtStart !== state.mappingAssignTarget) return;

    const data = JSON.parse(txt);
    const connected = !!data.connected;
    const seq = Number(data.last_seq || 0);
    const note = Number(data.last_note);

    const isMacroTarget = state.mappingAssignTarget === '__macro__';
    const isSbTarget = !isMacroTarget && state.mappingAssignTarget !== '__action_stop_all__' &&
                       state.mappingAssignTarget !== '__action_sampler_toggle__';
    const statusEl2 = isMacroTarget ? document.getElementById('chord-learn-status') :
      (isSbTarget ? sbMappingStatusEl : midiActionCaptureStatusEl);

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
        statusEl2.textContent = `Selected channel ${data.last_channel}, note ${note}. Record a chord or enter a script, then save.`;
      } else if (target === '__action_stop_all__') {
        await saveStopAllActionMapping(note);
        if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = `Assigned note ${note} to stop all.`;
      } else if (target === '__action_sampler_toggle__') {
        await saveSamplerToggleActionMapping(note);
        if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = `Assigned note ${note} to sampler toggle.`;
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
    }
  }
}

async function loadSamplerConfig() {
  try {
    const res = await fetch('/api/midi/sampler_config', { method: 'GET' });
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
    const res = await fetch('/api/midi/sampler_config', {
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
  await loadFramebufferView();
  await loadSoundboardModes();
  await listSfxFiles();
  await loadMappings();
  await loadMidiActions();
  await loadSamplerConfig();
  await loadRoutingThings();
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

function stopMeterPolling() {
  if (meterPollTimer !== null) clearInterval(meterPollTimer);
  meterPollTimer = null;
}

function startMeterPolling() {
  stopMeterPolling();
  if (document.hidden) return;
  pollMeters();
  meterPollTimer = setInterval(pollMeters, 250);
}

function updateVolumeMeter(id, levels) {
  const group = volumeMeters.get(id);
  if (!group) return;
  if (group.children.length !== levels.length) {
    group.replaceChildren();
    levels.forEach((_, channel) => {
      const row = document.createElement('label');
      row.className = 'vol-meter-row';
      const label = document.createElement('span');
      label.textContent = `Ch ${channel + 1}`;
      const meter = document.createElement('meter');
      meter.min = 0;
      meter.max = 1;
      meter.high = 0.95;
      meter.optimum = 0;
      meter.setAttribute('aria-label', `${id} channel ${channel + 1}`);
      const value = document.createElement('span');
      row.append(label, meter, value);
      group.appendChild(row);
    });
  }
  levels.forEach((raw, channel) => {
    const level = Number.isFinite(raw) ? Math.max(0, Math.min(1, raw)) : 0;
    const db = level > 0 ? 20 * Math.log10(level) : -Infinity;
    const row = group.children[channel];
    row.children[1].value = Math.max(0, (db + 60) / 60);
    row.children[2].textContent = `${db > -60 ? db.toFixed(1) : '-inf'} dBFS`;
  });
}

async function pollMeters() {
  if (meterPollBusy || state.activeTab !== 'volume' || document.hidden) return;
  meterPollBusy = true;
  const status = document.getElementById('vol-meter-status');
  try {
    const res = await fetch('/api/audio/meters', { cache: 'no-store', signal: AbortSignal.timeout(2000) });
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
    const message = 'Live levels: 4 updates/s, -60 to 0 dBFS.';
    if (status && status.textContent !== message) status.textContent = message;
  } catch (err) {
    if (state.activeTab !== 'volume' || document.hidden) return;
    for (const [id, group] of volumeMeters) {
      updateVolumeMeter(id, Array(group.children.length).fill(0));
    }
    const message = `Meters unavailable: ${AudioxUI.formatMessage(err)}`;
    if (status && status.textContent !== message) status.textContent = message;
  } finally {
    meterPollBusy = false;
  }
}

async function loadVolumes() {
  try {
    const [vRes, ccRes] = await Promise.all([
      fetch('/api/audio/volumes', { method: 'GET' }),
      fetch('/api/midi/cc_volumes', { method: 'GET' }),
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

  if (!state.volumes.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = 'No active devices. Open the Routing tab first.';
    volGridEl.appendChild(empty);
    return;
  }

  for (const vol of state.volumes) {
    const { id, name, gain } = vol;
    const cc = ccForThing(id);
    const isTarget = state.volAssignTarget === id;

    const card = document.createElement('div');
    card.className = `vol-card${isTarget ? ' cc-target' : ''}`;

    const nameEl = document.createElement('div');
    nameEl.className = 'vol-card-name';
    nameEl.title = name;
    nameEl.textContent = name;

    const idEl = document.createElement('div');
    idEl.className = 'vol-card-id';
    idEl.textContent = id;

    const sliderRow = document.createElement('div');
    sliderRow.className = 'vol-slider-row';

    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = '0';
    slider.max = '200';
    slider.step = '1';
    slider.value = String(Math.max(0, Math.min(200, gainToPercent(gain))));

    const pctEl = document.createElement('div');
    pctEl.className = 'vol-pct';
    pctEl.textContent = `${gainToPercent(gain)}%`;

    let saveTimer = null;
    slider.addEventListener('input', () => {
      pctEl.textContent = `${slider.value}%`;
      clearTimeout(saveTimer);
      saveTimer = setTimeout(() => {
        setVolumeGain(id, percentToGain(slider.value));
      }, 120);
    });

    sliderRow.appendChild(slider);
    sliderRow.appendChild(pctEl);

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
        const r = await fetch('/api/midi/last_cc', { method: 'GET' });
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
    card.appendChild(idEl);
    const meters = document.createElement('div');
    meters.className = 'vol-meters';
    volumeMeters.set(id, meters);
    card.appendChild(meters);
    card.appendChild(sliderRow);
    card.appendChild(ccRow);
    volGridEl.appendChild(card);
  }
}

async function setVolumeGain(thingId, gain) {
  try {
    const res = await fetch('/api/audio/volume/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `id=${encodeURIComponent(thingId)}&gain=${gain.toFixed(4)}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`volume set failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    // Update local state
    const v = state.volumes.find((e) => e.id === thingId);
    if (v) v.gain = gain;
    setStatus(`${thingId} volume set to ${Math.round(gain * 50)}%`, 'ok');
  } catch (err) {
    setStatus(`volume set error: ${err}`, 'warn');
  }
}

async function pollVolCcCapture() {
  if (!state.volAssignTarget) {
    if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
    return;
  }
  try {
    const res = await fetch('/api/midi/last_cc', { method: 'GET' });
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
    const res = await fetch('/api/midi/cc_volume/set', {
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
    const res = await fetch('/api/midi/cc_volume/delete', {
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
  btn.addEventListener('click', () => {
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
  if (!await AudioxUI.confirm('Apply the saved USB configuration? USB audio, MIDI, keyboard, and networking will briefly disconnect. Save any pending changes first.', {
    title: 'Reload USB configuration', confirmLabel: 'Apply and reconnect',
  })) return;
  return runLoadingTask('Reloading configuration', async () => {
    const ok = await reloadConfig();
    if (ok) {
      await loadRoutingThings();
      await loadRoutingFile();
    }
  });
});

document.getElementById('btn-route-save').addEventListener('click', () => runLoadingTask('Saving routing', saveRouting));
document.getElementById('btn-route-arrange').addEventListener('click', () => {
  state.autoArrange = true;
  state.positions = {};
  arrangeRoutingGraph();
});
document.getElementById('btn-zoom-fit').addEventListener('click', fitRoutingGraph);
document.getElementById('btn-route-details').addEventListener('click', (event) => {
  const visible = document.querySelector('.routing-grid').classList.toggle('show-details');
  event.currentTarget.setAttribute('aria-expanded', String(visible));
});

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

document.addEventListener('keydown', async (ev) => {
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
  state.soundboardEditing = editing;
  document.getElementById('soundboard-layout').classList.toggle('play-mode', !editing);
  sbGridEl.querySelectorAll('.sb-card-name').forEach((name) => {
    name.tabIndex = editing && state.soundboardOrderSupported ? 0 : -1;
    if (editing && state.soundboardOrderSupported) {
      name.setAttribute('role', 'button');
      name.setAttribute('aria-label', `Reorder ${name.textContent}`);
      name.setAttribute('aria-pressed', 'false');
    } else {
      name.removeAttribute('role');
      name.removeAttribute('aria-label');
      name.removeAttribute('aria-pressed');
    }
  });
  sbGridEl.classList.toggle('can-reorder', editing && state.soundboardOrderSupported);
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
  sbMappingStatusEl.textContent = editing
    ? 'Manage sounds, playback modes, and MIDI assignments.'
    : 'Tap a sound to play. Switch to Edit to manage sounds and MIDI.';
}
document.getElementById('btn-sb-play-mode').addEventListener('click', () => setSoundboardEditing(false));
document.getElementById('btn-sb-edit-mode').addEventListener('click', () => setSoundboardEditing(true));

document.getElementById('btn-upload-clear').addEventListener('click', () => {
  document.getElementById('upload_name').value = '';
  document.getElementById('upload_file').value = '';
  AudioxUI.setUnsaved('sound-upload', false);
  setStatus('upload form cleared');
});

document.getElementById('btn-sb-refresh').addEventListener('click', async () => {
  await reloadSoundboardBank();
  await listSfxFiles();
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
    const r = await fetch('/api/midi/last_note', { method: 'GET' });
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
    const r = await fetch('/api/midi/last_note', { method: 'GET' });
    if (r.ok) { const d = JSON.parse(await r.text()); state.mappingLastSeq = Number(d.last_seq || 0); }
  } catch (_) {}
  state.mappingAssignTarget = '__action_sampler_toggle__';
  if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
  if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = 'Press a MIDI button to map sampler toggle.';
});

document.getElementById('btn-sb-clear-sampler-toggle').addEventListener('click', async () => {
  await deleteSamplerToggleActionMapping();
});

document.getElementById('btn-vol-refresh').addEventListener('click', loadVolumes);

document.getElementById('btn-sampler-config-save').addEventListener('click', saveSamplerConfig);
document.getElementById('btn-sampler-config-load').addEventListener('click', loadSamplerConfig);

document.getElementById('btn-system-sync').addEventListener('click', async () => {
  await runSystemAction('/api/system/sync', 'sync');
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

AudioxUpdates.init({ setStatus, ui: AudioxUI });
AudioxBackup.init({ setStatus, ui: AudioxUI });


const lightDimButton = document.getElementById('btn-light-dim');
let lightDimBusy = false;

function renderLightDim(dimmed) {
  lightDimButton.textContent = dimmed ? 'Restore lights' : 'Dim lights';
  lightDimButton.setAttribute('aria-pressed', String(dimmed));
  lightDimButton.disabled = false;
}

async function requestLightDim(dimmed) {
  const res = await fetch('/api/midi/light_dim', dimmed === undefined
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
    const res = await fetch('/api/midi/light_config', { method: 'GET' });
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
    const res = await fetch('/api/midi/light_config', {
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
    const res = await fetch('/api/midi/light_refresh', { method: 'POST' });
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
    const res = await fetch('/api/system/boot-failure', { cache: 'no-store' });
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
    const res = await fetch('/api/system/boot-failure', { method: 'DELETE' });
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
