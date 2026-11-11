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

