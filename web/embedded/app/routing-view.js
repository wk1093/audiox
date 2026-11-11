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

