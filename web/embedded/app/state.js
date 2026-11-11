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

