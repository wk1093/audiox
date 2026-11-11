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

