const statusEl = document.getElementById('status');
const edgeListEl = document.getElementById('edge-list');
const graphWrapEl = document.getElementById('graph-wrap');
const graphCanvasEl = document.getElementById('graph-canvas');
const zoomLabelEl = document.getElementById('zoom-label');
const tabsEl = document.getElementById('tabs');
const appVersionEl = document.getElementById('app-version');
const sbGridEl = document.getElementById('sb-grid');
const sbMappingStatusEl = document.getElementById('sb-mapping-status');
const sbStopAllMidiEl = document.getElementById('sb-stop-all-midi');
const inspectorEl = document.getElementById('route-inspector');
const nodeCountEl = document.getElementById('metric-node-count');
const routeCountEl = document.getElementById('metric-route-count');
const selectedNodeEl = document.getElementById('metric-selected-node');
const selectedRouteEl = document.getElementById('metric-selected-route');

const configPath = '/api/rootfs/config.staging.txt';
const realConfigPath = '/api/rootfs/config.txt';
const routingPath = '/api/rootfs/routing.txt';

const state = {
  nodes: [],
  edges: [],
  positions: {},
  selectedNodeId: null,
  selectedEdgeIdx: -1,
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
  soundboardModesBySfx: {},
  stopAllMidiNote: null,
};

function setStatus(text, kind = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${kind}`.trim();
}

function setActiveTab(tab) {
  state.activeTab = tab;
  document.querySelectorAll('.tab-btn').forEach((el) => {
    el.classList.toggle('active', el.dataset.tab === tab);
  });
  document.querySelectorAll('.page').forEach((el) => {
    el.classList.toggle('active', el.dataset.page === tab);
  });
  if (tab === 'routing') {
    ensureRoutingGraph();
    state.graphCanvas.setDirty(true, true);
    updateZoomLabel();
  }
  if (tab === 'system') {
    loadSystemInfo();
  }
}

function updateZoomLabel() {
  const scale = state.graphCanvas ? state.graphCanvas.ds.scale : 1;
  const txt = `${Math.round(scale * 100)}%`;
  zoomLabelEl.textContent = `zoom ${txt}`;
  document.getElementById('btn-zoom-reset').textContent = txt;
}

function setRoutingZoom(nextScale) {
  if (!state.graphCanvas) {
    return;
  }

  const clamped = Math.max(0.35, Math.min(2.8, nextScale));
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
  };

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    if (key === 'soundboard_mode') {
      const mode = line.slice(eq + 1).trim().toLowerCase();
      cfg.soundboard_mode = (mode === 'hold') ? 'hold' : 'play';
      continue;
    }
    const value = Number(line.slice(eq + 1).trim());
    if (Number.isNaN(value)) continue;
    if (key in cfg) cfg[key] = value;
  }
  return cfg;
}

function formatConfigText(cfg) {
  return [
    `usb_playback_channels=${cfg.usb_playback_channels}`,
    `usb_capture_channels=${cfg.usb_capture_channels}`,
    `usb_sample_rate=${cfg.usb_sample_rate}`,
    `usb_sample_size=${cfg.usb_sample_size}`,
    `soundboard_mode=${cfg.soundboard_mode || 'play'}`,
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
  for (const edge of state.edges) {
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
  document.getElementById('usb_sample_rate').value = String(cfg.usb_sample_rate ?? 48000);
  document.getElementById('usb_sample_size').value = String(cfg.usb_sample_size ?? 2);
}

function getConfigFromForm() {
  return {
    usb_playback_channels: Number(document.getElementById('usb_playback_channels').value) || 2,
    usb_capture_channels: Number(document.getElementById('usb_capture_channels').value) || 2,
    usb_sample_rate: Number(document.getElementById('usb_sample_rate').value) || 48000,
    usb_sample_size: Number(document.getElementById('usb_sample_size').value) || 2,
    soundboard_mode: 'play',
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

function isDeviceNode(node) {
  return String(node?.properties?.nodeKind || '') === 'device';
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
  const seen = new Set();
  for (const edge of state.edges) {
    const srcNode = state.nodeByThingId.get(edge.src);
    const dstNode = state.nodeByThingId.get(edge.dst);
    if (!srcNode || !dstNode) {
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
  const w = Math.max(320, Math.floor(rect.width));
  const h = Math.max(260, Math.floor(rect.height));

  graphCanvasEl.style.width = `${w}px`;
  graphCanvasEl.style.height = `${h}px`;

  if (state.graphCanvas.bgcanvas) {
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
  syncEdgesFromGraph();
  setStatus('route removed (not saved yet)', 'ok');
  return true;
}

function removeEdgeByIndex(index) {
  if (index < 0 || index >= state.edges.length) {
    return;
  }

  state.edges.splice(index, 1);

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
    if (!state.internalGraphMutation && isDeviceNode(node)) {
      setStatus('device nodes cannot be deleted', 'warn');
      return;
    }
    graphRemove(node);
  };
  state.graphCanvas = new window.LGraphCanvas(graphCanvasEl, state.graph, { autoresize: true });

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
    if (isDeviceNode(node)) {
      return [];
    }
    return [
      {
        content: 'Remove Node',
        callback: () => {
          if (isDeviceNode(node)) {
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
    state.positions[thingId] = {
      x: Math.round(node.pos[0]),
      y: Math.round(node.pos[1]),
    };
    state.graphCanvas.setDirty(true, true);
  };

  state.graphCanvas.onNodeAdded = (node) => {
    if (!node) {
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

function defaultNodePosition(idx) {
  const col = idx % 3;
  const row = Math.floor(idx / 3);
  return { x: 30 + (col * 300), y: 20 + (row * 220) };
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

  const known = {
    usb_mic_in: { x: 20, y: 20 },
    soundboard_out: { x: 20, y: 250 },
    fx_slot_0: { x: 350, y: 120 },
    usb_gadget_out: { x: 680, y: 80 },
  };

  state.nodes.forEach((thing, idx) => {
    const node = window.LiteGraph.createNode('audiox/thing');
    if (!node) {
      return;
    }
    node.title = shortNodeTitle(thing.label);
    node.size = [260, 60];
    node.properties = { thingId: thing.id, nodeKind: 'device' };
    node.removable = false;
    node._baseInputCount = thing.inputs;
    node._inputChannelBySlot = [];

    for (let ch = 0; ch < thing.inputs; ch += 1) {
      node.addInput(`in ${ch + 1}`, 'audio');
      node._inputChannelBySlot.push(ch);
    }
    for (let ch = 0; ch < thing.outputs; ch += 1) {
      node.addOutput(`out ${ch + 1}`, 'audio');
    }

    const pos = state.positions[thing.id] || known[thing.id] || defaultNodePosition(idx);
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
  });

  state.internalGraphMutation = false;
  state.suspendGraphSync = false;
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
  }, 1000);
}

async function loadRoutingFile() {
  setStatus('loading routing...');
  try {
    const res = await fetch(routingPath, { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      state.edges = [];
      rebuildRoutingGraph();
      setStatus('routing file missing, starting empty', 'warn');
      return;
    }

    state.edges = parseRoutingText(txt);
    state.selectedEdgeIdx = -1;
    rebuildRoutingGraph();
    setStatus('routing loaded', 'ok');
  } catch (err) {
    setStatus(`routing load error: ${err}`, 'warn');
  }
}

async function reloadRoutingFast() {
  try {
    const res = await fetch('/api/routing/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`routing reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
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

    const reloaded = await reloadRoutingFast();
    if (!reloaded) {
      return false;
    }

    setStatus('routing saved and reloaded', 'ok');
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

async function loadSystemInfo() {
  try {
    const res = await fetch('/api/system/info', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) return;
    const data = JSON.parse(txt);

    const vEl = document.getElementById('sysinfo-version');
    const kEl = document.getElementById('sysinfo-kernel');
    const uEl = document.getElementById('sysinfo-uptime');
    const mEl = document.getElementById('sysinfo-memory');
    const lEl = document.getElementById('sysinfo-load');
    if (!vEl) return;

    vEl.textContent = data.version ? `v${data.version}` : '\u2014';
    kEl.textContent = data.kernel || '\u2014';

    const secs = Number(data.uptime_secs || 0);
    const d = Math.floor(secs / 86400);
    const h = Math.floor((secs % 86400) / 3600);
    const m = Math.floor((secs % 3600) / 60);
    uEl.textContent = d > 0 ? `${d}d ${h}h ${m}m` : `${h}h ${m}m`;

    const totalMb = Number(data.mem_total_mb || 0);
    const availMb = Number(data.mem_avail_mb || 0);
    mEl.textContent = `${availMb} / ${totalMb} MB`;

    lEl.textContent = Number(data.load1 || 0).toFixed(2);
  } catch (err) {}
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
    setStatus('config loaded', 'ok');
    return cfg;
  } catch (err) {
    setStatus(`config load error: ${err}`, 'warn');
    return null;
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
    return true;
  } catch (err) {
    setStatus(`config save error: ${err}`, 'warn');
    return false;
  }
}

async function reloadConfig() {
  setStatus('reloading config...');
  try {
    const res = await fetch('/api/config/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    setStatus(txt.trim() || 'config reloaded', 'ok');
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
      setStatus(txt.trim() || `released ${fileName}`, 'ok');
    } else {
      setStatus(txt.trim() || `triggered ${fileName}`, 'ok');
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
    setStatus(txt.trim() || 'all clips stopped', 'ok');
  } catch (err) {
    setStatus(`request failed: ${err}`, 'warn');
  }
}

function renderStopAllMidiStatus() {
  if (!sbStopAllMidiEl) {
    return;
  }
  if (state.stopAllMidiNote === null || state.stopAllMidiNote === undefined) {
    sbStopAllMidiEl.textContent = 'Stop All MIDI: unassigned';
    return;
  }
  sbStopAllMidiEl.textContent = `Stop All MIDI: note ${state.stopAllMidiNote}`;
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
  setStatus(`${label}...`);
  try {
    const res = await fetch(path, { method: 'POST' });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`${label} failed: ${res.status} ${txt}`, 'warn');
      return false;
    }
    setStatus(txt || `${label} ok`, 'ok');
    return true;
  } catch (err) {
    setStatus(`${label} error: ${err}`, 'warn');
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
    const res = await fetch('/api/rootfs/sfx/', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      sfxListEl.textContent = 'failed to list sfx files';
      return [];
    }

    const files = [];
    for (const line of txt.split(/\r?\n/)) {
      const t = line.trim();
      if (!t || t.startsWith('rootfs listing:')) continue;
      if (t.endsWith('/')) continue;
      files.push(t);
    }

    files.sort((a, b) => a.localeCompare(b));
    state.sfxFiles = files;
    renderSoundboardGrid();
    return files;
  } catch (err) {
    return [];
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

    const midiEl = document.createElement('div');
    midiEl.className = `sb-card-midi${hasMidi ? ' mapped' : ''}`;
    if (hasMidi) {
      const noteSpan = document.createElement('span');
      noteSpan.textContent = `MIDI ${mappedNote}`;
      const clearBtn = document.createElement('button');
      clearBtn.className = 'sb-midi-clear';
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
    playBtn.className = 'primary';
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
    playBtn.addEventListener('click', (ev) => {
      if (mode === 'hold') {
        ev.preventDefault();
        return;
      }
      triggerSfx(file, 'trigger');
    });

    const assignBtn = document.createElement('button');
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
    delBtn.className = 'warn';
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
      await replaceSfxFile(file, selected);
      replaceInput.value = '';
    });

    const replaceBtn = document.createElement('button');
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
      renderStopAllMidiStatus();
      return;
    }

    const data = JSON.parse(txt);
    state.stopAllMidiNote = null;
    const actions = Array.isArray(data?.actions) ? data.actions : [];
    for (const item of actions) {
      if (String(item?.action || '') !== 'stop_all') {
        continue;
      }
      const note = Number(item?.note);
      if (Number.isFinite(note) && note >= 0 && note <= 127) {
        state.stopAllMidiNote = note;
        break;
      }
    }
    renderStopAllMidiStatus();
  } catch (_) {
    state.stopAllMidiNote = null;
    renderStopAllMidiStatus();
  }
}

async function saveStopAllActionMapping(note) {
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }

  try {
    const body = `note=${note}&action=stop_all`;
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
    setStatus(`mapped note ${note} -> stop all`, 'ok');
  } catch (err) {
    setStatus(`action mapping save error: ${err}`, 'warn');
  }
}

async function deleteStopAllActionMapping() {
  if (!Number.isFinite(state.stopAllMidiNote)) {
    setStatus('no stop-all MIDI mapping to remove', 'warn');
    return;
  }

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

  try {
    const res = await fetch('/api/midi/last_note', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) return;

    const data = JSON.parse(txt);
    const connected = !!data.connected;
    const seq = Number(data.last_seq || 0);
    const note = Number(data.last_note);

    if (!connected) {
      sbMappingStatusEl.textContent = 'Waiting for MIDI device\u2026';
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
      if (target === '__action_stop_all__') {
        await saveStopAllActionMapping(note);
        sbMappingStatusEl.textContent = `Assigned note ${note} to stop all.`;
      } else {
        await saveMapping(note, target);
        sbMappingStatusEl.textContent = `Assigned note ${note} to "${target}".`;
      }
      renderSoundboardGrid();
    }
  } catch (err) {}
}

async function initializeGraphFromConfig() {
  ensureRoutingGraph();
  await loadConfig();
  await loadSoundboardModes();
  await listSfxFiles();
  await loadMappings();
  await loadMidiActions();
  await loadRoutingThings();
  await loadRoutingFile();
  state.edgeDeleteEnabled = true;
  updateZoomLabel();
  startRoutingThingsPolling();
}

tabsEl.querySelectorAll('.tab-btn').forEach((btn) => {
  btn.addEventListener('click', () => {
    setActiveTab(btn.dataset.tab);
    if (btn.dataset.tab === 'routing') {
      pollRoutingThingsIfNeeded();
    }
  });
});

document.getElementById('btn-load').addEventListener('click', async () => {
  const cfg = await loadConfig();
  if (cfg) {
    await loadRoutingThings();
    await loadRoutingFile();
  }
});

document.getElementById('btn-save').addEventListener('click', async () => {
  const ok = await saveConfig();
  if (ok) {
    await loadRoutingThings();
    await loadRoutingFile();
  }
});

document.getElementById('btn-reload').addEventListener('click', async () => {
  const ok = await reloadConfig();
  if (ok) {
    await loadRoutingThings();
    await loadRoutingFile();
  }
});

document.getElementById('btn-route-save').addEventListener('click', saveRouting);
document.getElementById('btn-route-remove-selected').addEventListener('click', deleteSelectedEdge);

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

document.addEventListener('keydown', (ev) => {
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
    if (isDeviceNode(node)) {
      setStatus('device nodes cannot be deleted', 'warn');
      return;
    }
    state.graph.remove(node);
    syncEdgesFromGraph();
    state.graphCanvas.setDirty(true, true);
    setStatus('node removed', 'ok');
  }
});

document.getElementById('btn-upload-real').addEventListener('click', uploadSfxFile);

document.getElementById('btn-upload-clear').addEventListener('click', () => {
  document.getElementById('upload_name').value = '';
  document.getElementById('upload_file').value = '';
  setStatus('upload form cleared');
});

document.getElementById('btn-sb-refresh').addEventListener('click', async () => {
  await reloadSoundboardBank();
  await listSfxFiles();
  await loadMappings();
  await loadMidiActions();
  await loadLightConfig();
  await loadSoundboardModes();
  setStatus('soundboard refreshed', 'ok');
});

document.getElementById('btn-sb-stop-all').addEventListener('click', triggerStopAll);

document.getElementById('btn-sb-assign-stop-all').addEventListener('click', async () => {
  const isTarget = state.mappingAssignTarget === '__action_stop_all__';
  if (isTarget) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) {
      clearInterval(state.mappingPollTimer);
      state.mappingPollTimer = null;
    }
    sbMappingStatusEl.textContent = 'Assignment cancelled.';
    return;
  }

  try {
    const r = await fetch('/api/midi/last_note', { method: 'GET' });
    if (r.ok) {
      const d = JSON.parse(await r.text());
      state.mappingLastSeq = Number(d.last_seq || 0);
    }
  } catch (_) {}

  state.mappingAssignTarget = '__action_stop_all__';
  if (!state.mappingPollTimer) {
    state.mappingPollTimer = setInterval(pollMappingCapture, 220);
  }
  sbMappingStatusEl.textContent = 'Press a MIDI button to map stop all.';
  renderSoundboardGrid();
});

document.getElementById('btn-sb-clear-stop-all').addEventListener('click', async () => {
  await deleteStopAllActionMapping();
});

document.getElementById('btn-system-sync').addEventListener('click', async () => {
  await runSystemAction('/api/system/sync', 'sync');
});

document.getElementById('btn-system-restart').addEventListener('click', async () => {
  await runSystemAction('/api/system/restart', 'restart');
});

document.getElementById('btn-system-shutdown').addEventListener('click', async () => {
  await runSystemAction('/api/system/shutdown', 'shutdown');
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

initializeGraphFromConfig();
loadVersion();
loadLightConfig();
