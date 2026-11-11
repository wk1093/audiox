// Startup and shared UI wiring; index.html loads state and feature scripts first.
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
