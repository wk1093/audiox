const statusEl = document.getElementById('status');
const edgeListEl = document.getElementById('edge-list');
const graphWrapEl = document.getElementById('graph-wrap');
const graphCanvasEl = document.getElementById('graph-canvas');
const zoomLabelEl = document.getElementById('zoom-label');
const tabsEl = document.getElementById('tabs');
const appVersionEl = document.getElementById('app-version');
const sfxListEl = document.getElementById('sfx-list');
const soundboardTriggerListEl = document.getElementById('soundboard-trigger-list');
const mappingListEl = document.getElementById('mapping-list');
const mappingSfxEl = document.getElementById('mapping_sfx');
const mappingNoteEl = document.getElementById('mapping_note');
const mappingCaptureStatusEl = document.getElementById('mapping-capture-status');
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
  mappingMode: false,
  mappingPollTimer: null,
  mappingLastSeq: 0,
  sfxFiles: [],
  graph: null,
  graphCanvas: null,
  nodeByThingId: new Map(),
  suspendGraphSync: false,
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
    usb_sample_rate: 44100,
    usb_sample_size: 2,
  };

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
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

function setFormFromConfig(cfg) {
  if (!cfg) return;
  document.getElementById('usb_playback_channels').value = String(cfg.usb_playback_channels ?? 2);
  document.getElementById('usb_capture_channels').value = String(cfg.usb_capture_channels ?? 2);
  document.getElementById('usb_sample_rate').value = String(cfg.usb_sample_rate ?? 44100);
  document.getElementById('usb_sample_size').value = String(cfg.usb_sample_size ?? 2);
}

function getConfigFromForm() {
  return {
    usb_playback_channels: Number(document.getElementById('usb_playback_channels').value) || 2,
    usb_capture_channels: Number(document.getElementById('usb_capture_channels').value) || 2,
    usb_sample_rate: Number(document.getElementById('usb_sample_rate').value) || 44100,
    usb_sample_size: Number(document.getElementById('usb_sample_size').value) || 2,
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

  inspectorEl.textContent = 'Drag from output to input to route. Multi-source into one input is supported.';
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
      state.selectedEdgeIdx = idx;
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

function getOrCreateInputSlotForChannel(node, channel) {
  if (!node || !Array.isArray(node.inputs) || !Array.isArray(node._inputChannelBySlot)) {
    return channel;
  }

  for (let i = 0; i < node.inputs.length; i += 1) {
    if (node._inputChannelBySlot[i] === channel && node.inputs[i]?.link == null) {
      return i;
    }
  }

  node.addInput(`in ${channel + 1}`, 'audio');
  node._inputChannelBySlot.push(channel);
  return node.inputs.length - 1;
}

function syncEdgesFromGraph() {
  if (!state.graph || state.suspendGraphSync) {
    return;
  }

  const prevSelectedKey = (state.selectedEdgeIdx >= 0 && state.selectedEdgeIdx < state.edges.length)
    ? edgeKey(state.edges[state.selectedEdgeIdx])
    : null;

  const next = [];
  const links = state.graph.links || {};
  for (const linkId of Object.keys(links)) {
    const link = links[linkId];
    if (!link) continue;

    const srcNode = state.graph.getNodeById(link.origin_id);
    const dstNode = state.graph.getNodeById(link.target_id);
    if (!srcNode || !dstNode) continue;

    const src = normalizeThingId(srcNode.properties?.thingId);
    const dst = normalizeThingId(dstNode.properties?.thingId);
    const srcChannel = Number(link.origin_slot);
    const dstChannel = getInputChannelFromSlot(dstNode, Number(link.target_slot));
    if (!src || !dst || srcChannel < 0 || dstChannel < 0) {
      continue;
    }

    next.push({ src, dst, srcChannel, dstChannel });
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
  const srcNode = state.graph.getNodeById(link.origin_id);
  const dstNode = state.graph.getNodeById(link.target_id);
  if (!srcNode || !dstNode) {
    return Number.POSITIVE_INFINITY;
  }

  const a = srcNode.getConnectionPos(false, link.origin_slot);
  const b = dstNode.getConnectionPos(true, link.target_slot);
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
  let bestId = null;
  let bestDist = Number.POSITIVE_INFINITY;

  const links = state.graph.links || {};
  for (const linkId of Object.keys(links)) {
    const link = links[linkId];
    if (!link) continue;
    const d = linkDistance(link, x, y);
    if (d < bestDist) {
      bestDist = d;
      bestId = Number(linkId);
    }
  }

  if (bestId == null || bestDist > threshold) {
    return false;
  }

  state.graph.removeLink(bestId);
  syncEdgesFromGraph();
  setStatus('route removed (not saved yet)', 'ok');
  return true;
}

function removeEdgeByIndex(index) {
  if (index < 0 || index >= state.edges.length) {
    return;
  }

  const target = state.edges[index];
  const links = state.graph ? (state.graph.links || {}) : {};
  for (const linkId of Object.keys(links)) {
    const link = links[linkId];
    if (!link) continue;

    const srcNode = state.graph.getNodeById(link.origin_id);
    const dstNode = state.graph.getNodeById(link.target_id);
    if (!srcNode || !dstNode) continue;

    const edge = {
      src: normalizeThingId(srcNode.properties?.thingId),
      dst: normalizeThingId(dstNode.properties?.thingId),
      srcChannel: Number(link.origin_slot),
      dstChannel: getInputChannelFromSlot(dstNode, Number(link.target_slot)),
    };

    if (edgeKey(edge) === edgeKey(target)) {
      state.graph.removeLink(Number(linkId));
      syncEdgesFromGraph();
      setStatus('route removed (not saved yet)', 'ok');
      return;
    }
  }

  state.edges.splice(index, 1);
  rebuildRoutingGraph();
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
  state.graphCanvas = new window.LGraphCanvas(graphCanvasEl, state.graph, { autoresize: true });

  if (!window.LiteGraph.registered_node_types['audiox/thing']) {
    function AudioxThingNode() {
      this.size = [260, 60];
      this.properties = { thingId: '' };
    }
    AudioxThingNode.title = 'Thing';
    window.LiteGraph.registerNodeType('audiox/thing', AudioxThingNode);
  }
  state.graphCanvas.background_image = null;
  window.LiteGraph.NODE_DEFAULT_COLOR = '#1f4a62';
  window.LiteGraph.NODE_DEFAULT_BGCOLOR = '#102635';
  window.LiteGraph.NODE_DEFAULT_BOXCOLOR = '#7fd1ff';
  window.LiteGraph.NODE_TEXT_COLOR = '#eaf5ff';
  window.LiteGraph.LINK_COLOR = '#79c8ff';
  window.LiteGraph.EVENT_LINK_COLOR = '#79c8ff';
  state.graphCanvas.default_link_color = '#79c8ff';
  state.graphCanvas.allow_searchbox = false;
  state.graphCanvas.render_shadows = true;
  state.graphCanvas.render_curved_connections = true;
  state.graphCanvas.connections_width = 3;
  state.graphCanvas.processContextMenu = () => false;
  state.graphCanvas.getCanvasMenuOptions = () => [];

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
    node.properties = { thingId: thing.id };
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

    node.onBeforeConnectInput = function(inputIndex) {
      const channel = getInputChannelFromSlot(this, inputIndex);
      const input = this.inputs?.[inputIndex];
      if (channel < 0 || !input || input.link == null) {
        return inputIndex;
      }
      this.addInput(`in ${channel + 1}`, 'audio');
      this._inputChannelBySlot.push(channel);
      return this.inputs.length - 1;
    };

    node.onConnectionsChange = function() {
      if (!state.suspendGraphSync) {
        syncEdgesFromGraph();
      }
    };

    state.graph.add(node);
    state.nodeByThingId.set(thing.id, node);
  });

  for (const edge of state.edges) {
    const srcNode = state.nodeByThingId.get(edge.src);
    const dstNode = state.nodeByThingId.get(edge.dst);
    if (!srcNode || !dstNode) {
      continue;
    }
    const dstSlot = getOrCreateInputSlotForChannel(dstNode, edge.dstChannel);
    srcNode.connect(edge.srcChannel, dstNode, dstSlot);
  }

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
  try {
    const res = await fetch('/api/routing/things', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`things load failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    const parsed = JSON.parse(txt);
    buildNodesFromThings(parsed?.things || []);
    rebuildRoutingGraph();
    return true;
  } catch (err) {
    setStatus(`things load error: ${err}`, 'warn');
    return false;
  }
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

function setEdgeDeleteMode(enabled) {
  state.edgeDeleteEnabled = !!enabled;
  const btn = document.getElementById('btn-edge-delete-mode');
  btn.textContent = `Delete Click: ${state.edgeDeleteEnabled ? 'ON' : 'OFF'}`;
  btn.classList.toggle('warn', state.edgeDeleteEnabled);
  btn.classList.toggle('primary', !state.edgeDeleteEnabled);
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

async function triggerSfx(fileName) {
  if (!fileName) {
    return;
  }

  setStatus(`triggering ${fileName}...`);
  try {
    const target = encodeURIComponent(fileName);
    const res = await fetch(`/api/soundboard/trigger/${target}`, { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`error: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    setStatus(txt.trim() || `triggered ${fileName}`, 'ok');
  } catch (err) {
    setStatus(`request failed: ${err}`, 'warn');
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
  return base.replace(/[^a-zA-Z0-9._-]/g, '_');
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
    renderSfxFiles();
    renderSoundboardTriggers();
    renderMappingSfxSelect();
    return files;
  } catch (err) {
    sfxListEl.textContent = `failed to list sfx files: ${err}`;
    return [];
  }
}

function renderSfxFiles() {
  sfxListEl.innerHTML = '';
  if (!state.sfxFiles.length) {
    sfxListEl.textContent = 'no files in /audiox/sfx';
    return;
  }

  for (const file of state.sfxFiles) {
    const row = document.createElement('div');
    row.className = 'edge-item';
    row.textContent = file;
    sfxListEl.appendChild(row);
  }
}

function renderSoundboardTriggers() {
  soundboardTriggerListEl.innerHTML = '';
  if (!state.sfxFiles.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = 'No SFX files uploaded yet.';
    soundboardTriggerListEl.appendChild(empty);
    return;
  }

  for (const file of state.sfxFiles) {
    const btn = document.createElement('button');
    btn.textContent = `Play ${file}`;
    btn.addEventListener('click', () => triggerSfx(file));
    soundboardTriggerListEl.appendChild(btn);
  }
}

function renderMappingSfxSelect() {
  const prev = mappingSfxEl.value;
  mappingSfxEl.innerHTML = '';
  for (const file of state.sfxFiles) {
    const opt = document.createElement('option');
    opt.value = file;
    opt.textContent = file;
    mappingSfxEl.appendChild(opt);
  }

  if (prev && state.sfxFiles.includes(prev)) {
    mappingSfxEl.value = prev;
  }
}

async function uploadSfxFile() {
  const slot = Number(document.getElementById('upload_slot').value || 0);
  const file = document.getElementById('upload_file').files[0];
  const manualName = document.getElementById('upload_name').value;
  if (!file) {
    setStatus('upload failed: pick a file first', 'warn');
    return;
  }

  const targetName = sanitizeSfxName(manualName || file.name);
  if (!targetName) {
    setStatus('upload failed: invalid target name', 'warn');
    return;
  }

  setStatus(`uploading ${targetName}...`);
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
    setStatus(`uploaded ${targetName} for slot ${slot}`, 'ok');
  } catch (err) {
    setStatus(`upload error: ${err}`, 'warn');
  }
}

async function loadMappings() {
  try {
    const res = await fetch('/api/midi/mappings', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      mappingListEl.textContent = `failed to load mappings: ${res.status}`;
      return;
    }

    const data = JSON.parse(txt);
    const mappings = Array.isArray(data?.mappings) ? data.mappings : [];
    mappingListEl.innerHTML = '';
    if (!mappings.length) {
      mappingListEl.textContent = 'no mappings yet';
      return;
    }

    for (const map of mappings) {
      const row = document.createElement('div');
      row.className = 'edge-item';
      row.textContent = `note ${map.note} -> ${map.sfx}`;
      row.addEventListener('click', () => {
        mappingNoteEl.value = String(map.note);
        if (state.sfxFiles.includes(map.sfx)) {
          mappingSfxEl.value = map.sfx;
        }
      });
      mappingListEl.appendChild(row);
    }
  } catch (err) {
    mappingListEl.textContent = `failed to load mappings: ${err}`;
  }
}

async function saveMapping() {
  const note = Number(mappingNoteEl.value);
  const sfx = mappingSfxEl.value;
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }
  if (!sfx) {
    setStatus('select a target sound first', 'warn');
    return;
  }

  setStatus(`saving mapping note ${note} -> ${sfx}...`);
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
    setStatus(`mapping saved note ${note} -> ${sfx}`, 'ok');
  } catch (err) {
    setStatus(`mapping save error: ${err}`, 'warn');
  }
}

async function deleteMapping() {
  const note = Number(mappingNoteEl.value);
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }

  setStatus(`deleting mapping for note ${note}...`);
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
    setStatus(`mapping deleted for note ${note}`, 'ok');
  } catch (err) {
    setStatus(`mapping delete error: ${err}`, 'warn');
  }
}

async function pollMappingCapture() {
  if (!state.mappingMode) {
    return;
  }

  try {
    const res = await fetch('/api/midi/last_note', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      return;
    }

    const data = JSON.parse(txt);
    const connected = !!data.connected;
    const seq = Number(data.last_seq || 0);
    const note = Number(data.last_note);
    const velocity = Number(data.last_velocity || 0);

    if (!connected) {
      mappingCaptureStatusEl.textContent = 'Mapping mode ON - waiting for MIDI device...';
      return;
    }

    if (seq > state.mappingLastSeq && note >= 0 && note <= 127) {
      state.mappingLastSeq = seq;
      mappingNoteEl.value = String(note);
      mappingCaptureStatusEl.textContent = `Captured note ${note} (velocity ${velocity})`;
      setStatus(`captured midi note ${note}`, 'ok');
    }
  } catch (err) {
  }
}

function setMappingMode(enabled) {
  state.mappingMode = !!enabled;
  const btn = document.getElementById('btn-mapping-mode');
  btn.textContent = `Mapping Mode: ${state.mappingMode ? 'ON' : 'OFF'}`;
  btn.classList.toggle('primary', state.mappingMode);
  btn.classList.toggle('warn', !state.mappingMode);

  if (state.mappingPollTimer) {
    clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
  }

  if (state.mappingMode) {
    mappingCaptureStatusEl.textContent = 'Mapping mode ON - press a MIDI button to capture note.';
    state.mappingPollTimer = setInterval(pollMappingCapture, 220);
  } else {
    mappingCaptureStatusEl.textContent = 'Mapping mode OFF.';
  }
}

async function initializeGraphFromConfig() {
  ensureRoutingGraph();
  await loadConfig();
  await listSfxFiles();
  await loadMappings();
  await loadRoutingThings();
  await loadRoutingFile();
  setEdgeDeleteMode(true);
  setMappingMode(false);
  updateZoomLabel();
}

tabsEl.querySelectorAll('.tab-btn').forEach((btn) => {
  btn.addEventListener('click', () => {
    setActiveTab(btn.dataset.tab);
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

document.getElementById('btn-edge-delete-mode').addEventListener('click', () => {
  setEdgeDeleteMode(!state.edgeDeleteEnabled);
});

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
    state.graphCanvas.resize();
    state.graphCanvas.setDirty(true, true);
    updateZoomLabel();
  }
});

document.addEventListener('keydown', (ev) => {
  if (ev.key === 'Delete' || ev.key === 'Backspace') {
    if (state.activeTab === 'routing' && state.selectedEdgeIdx >= 0) {
      ev.preventDefault();
      deleteSelectedEdge();
    }
  }
});

document.getElementById('btn-upload-real').addEventListener('click', uploadSfxFile);

document.getElementById('btn-upload-clear').addEventListener('click', () => {
  document.getElementById('upload_slot').value = '0';
  document.getElementById('upload_name').value = '';
  document.getElementById('upload_file').value = '';
  setStatus('upload form cleared');
});

document.getElementById('btn-mapping-mode').addEventListener('click', () => {
  setMappingMode(!state.mappingMode);
});

document.getElementById('btn-mapping-refresh').addEventListener('click', async () => {
  await listSfxFiles();
  await loadMappings();
  setStatus('mapping data refreshed', 'ok');
});

document.getElementById('btn-mapping-save').addEventListener('click', saveMapping);
document.getElementById('btn-mapping-delete').addEventListener('click', deleteMapping);

document.getElementById('btn-system-sync').addEventListener('click', async () => {
  await runSystemAction('/api/system/sync', 'sync');
});

document.getElementById('btn-system-restart').addEventListener('click', async () => {
  await runSystemAction('/api/system/restart', 'restart');
});

document.getElementById('btn-system-shutdown').addEventListener('click', async () => {
  await runSystemAction('/api/system/shutdown', 'shutdown');
});

initializeGraphFromConfig();
loadVersion();
