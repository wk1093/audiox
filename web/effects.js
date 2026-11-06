(() => {
  function parseJson(text) {
    try {
      return JSON.parse(text);
    } catch (_) {
      return null;
    }
  }

  function formBody(obj) {
    const params = new URLSearchParams();
    for (const [k, v] of Object.entries(obj)) {
      params.set(String(k), String(v));
    }
    return params.toString();
  }

  function normalizeEffectId(v) {
    const id = String(v || '').trim();
    return /^fx_[a-zA-Z0-9_]+$/.test(id) ? id : '';
  }

  function toNumber(v, fallback = 0) {
    const n = Number(v);
    return Number.isFinite(n) ? n : fallback;
  }

  function toEnabled(v) {
    if (typeof v === 'boolean') return v;
    if (typeof v === 'number') return v !== 0;
    if (typeof v === 'string') {
      const s = v.trim().toLowerCase();
      if (!s) return false;
      if (s === '0' || s === 'false' || s === 'off' || s === 'no') return false;
      if (s === '1' || s === 'true' || s === 'on' || s === 'yes') return true;
      const n = Number(s);
      if (Number.isFinite(n)) return n !== 0;
      return true;
    }
    return !!v;
  }

  function paramMetaForType(type) {
    const t = String(type || 'gain').trim();
    if (t === 'cut') {
      return {
        channels: { label: 'Channels', min: 1, max: 16, step: 1, precision: 0 },
      };
    }
    if (t === 'pitch') {
      return {
        gain: { label: 'Input Gain', min: 0, max: 4, step: 0.01, precision: 2 },
        drive: { label: 'Semitones', min: -12, max: 12, step: 0.01, precision: 2 },
        clip: { label: 'Mix', min: 0, max: 1, step: 0.01, precision: 2 },
        output: { label: 'Output Gain', min: 0, max: 4, step: 0.01, precision: 2 },
      };
    }
    if (t === 'reverb') {
      return {
        gain: { label: 'Input Gain', min: 0, max: 4, step: 0.01, precision: 2 },
        drive: { label: 'Feedback', min: 0.05, max: 0.95, step: 0.01, precision: 2 },
        clip: { label: 'Damping', min: 0, max: 1, step: 0.01, precision: 2 },
        output: { label: 'Wet Mix', min: 0, max: 1, step: 0.01, precision: 2 },
      };
    }
    if (t === 'gate') {
      return {
        gain: { label: 'Threshold dB', min: -80, max: 0, step: 0.1, precision: 1 },
        drive: { label: 'Attack ms', min: 0.2, max: 100, step: 0.1, precision: 1 },
        clip: { label: 'Release ms', min: 5, max: 600, step: 1, precision: 0 },
        output: { label: 'Range dB', min: 0, max: 80, step: 0.1, precision: 1 },
      };
    }
    if (t === 'distortion') {
      return {
        gain: { label: 'Input Gain', min: 0, max: 4, step: 0.01, precision: 2 },
        drive: { label: 'Drive', min: 0, max: 8, step: 0.01, precision: 2 },
        clip: { label: 'Clip', min: 0.05, max: 1, step: 0.01, precision: 2 },
        output: { label: 'Output', min: 0, max: 4, step: 0.01, precision: 2 },
      };
    }
    return {
      gain: { label: 'Gain', min: 0, max: 4, step: 0.01, precision: 2 },
      drive: { label: 'Drive', min: 0, max: 8, step: 0.01, precision: 2 },
      clip: { label: 'Clip', min: 0, max: 1, step: 0.01, precision: 2 },
      output: { label: 'Output', min: 0, max: 4, step: 0.01, precision: 2 },
    };
  }

  function dynamicParamList(fx) {
    const params = fx?.params || {};
    const out = [];

    const explicit = Array.isArray(fx?.param_meta) ? fx.param_meta : [];
    for (const raw of explicit) {
      const name = String(raw?.name || '').trim();
      if (!name) continue;
      out.push({
        name,
        label: String(raw?.label || name),
        min: Number.isFinite(Number(raw?.min)) ? Number(raw.min) : 0,
        max: Number.isFinite(Number(raw?.max)) ? Number(raw.max) : 1,
        ...(normalizePluginId(fx?.type) ? {
          unit: String(raw.unit || ''),
          description: String(raw.description || ''),
          type: raw.type || 'float',
          step: Number(raw.step) > 0 ? Number(raw.step) : 0.01,
          precision: raw.type && raw.type !== 'float' ? 0 : 6,
          ...(raw.type === 'boolean' ? { options: ['Off', 'On'] } :
            raw.type === 'enum' && Array.isArray(raw.options) ? { options: raw.options } : {}),
        } : parameterHelp(name, fx?.type)),
        defaultValue: Number(raw.default),
        value: toNumber(params[name], Number(raw?.default) || 0),
      });
    }
    if (out.length || normalizePluginId(fx?.type)) {
      return out;
    }

    const fallback = paramMetaForType(fx?.type || 'gain');
    const keys = Object.keys(fallback);
    for (const key of keys) {
      const m = fallback[key];
      out.push({
        name: key,
        label: m.label,
        min: m.min,
        max: m.max,
        step: m.step,
        precision: m.precision,
        value: toNumber(params[key], m.min),
      });
    }
    return out;
  }

  const effectTypes = {
    gain: 'Gain', distortion: 'Distortion', pitch: 'Pitch Shift',
    reverb: 'Reverb', gate: 'Noise Gate', cut: 'Mute',
    compressor: 'Compressor', limiter: 'Limiter', eq: 'Parametric EQ',
    multiband: 'Multiband Compressor', noise_reduction: 'Noise Reduction',
  };

  const pluginStatusText = {
    ready: 'Plugin running.',
    fault: 'Plugin fault: processing stopped. Reload the plugin from Plugins after fixing the cause.',
    unavailable: 'Plugin unavailable: it is not installed or failed to load. Install or reload it from Plugins.',
  };

  function normalizePluginId(value) {
    const id = String(value ?? '').trim();
    return /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/.test(id) && !effectTypes[id] ? id : '';
  }

  function pluginAssetUrl(id) {
    const safe = normalizePluginId(id);
    return safe ? `/api/plugins/${encodeURIComponent(safe)}/assets/index.html` : '';
  }

  function effectName(fx) {
    return fx?.name || `${effectTypes[fx?.type] || (normalizePluginId(fx?.type) ? fx.type : 'Effect')} ${String(fx?.id || '').replace(/^fx_/, '')}`.trim();
  }

  function parameterHelp(name, type) {
    const help = {
      __proto__: null,
      level: ['Level', 'x', 'Multiplies the signal level. 1 is unchanged; 0 silences it.'],
      input_gain: ['Input gain', 'x', 'Level entering the effect. 1 leaves the input unchanged.'],
      output_gain: ['Output gain', 'x', 'Level after processing. Reduce this if the result is too loud.'],
      drive: ['Drive', 'x', 'Pushes the distortion harder for a more saturated sound.'],
      clip: ['Clip threshold', '', 'Lower values clip the waveform sooner, adding more distortion.'],
      semitones: ['Pitch shift', 'st', 'Moves pitch down or up. 12 semitones is one octave.'],
      mix: ['Effect mix', '', 'Blends dry and processed sound. 0 is dry; 1 is fully processed. Delayed effects align their dry signal internally.'],
      noise_floor: ['Noise floor', '', 'Suppresses very quiet input in the pitch shifter. Increase to reduce low-level artifacts.'],
      wet_lpf_hz: ['Wet tone cutoff', 'Hz', 'Low-pass filter on the shifted signal. Lower values make it darker.'],
      quality: ['Quality', '', 'High: 8× overlap. Balanced: 4×. Economy: 2×. Lower modes use less CPU but may add artifacts. Changing mode clears the audio history.'],
      feedback: ['Reverb length', '', 'Higher feedback makes the reverb tail last longer.'],
      damping: ['High-frequency damping', '', 'Higher values absorb more high frequencies for a darker reverb tail.'],
      wet_mix: ['Reverb mix', '', 'Blends the reverb with the original sound. 0 is dry; 1 is wet.'],
      threshold_db: ['Threshold', 'dB', 'The gate turns down signals quieter than this level.'],
      attack_ms: ['Attack', 'ms', 'How quickly the gate opens when input rises above the threshold.'],
      release_ms: ['Release', 'ms', 'How gradually the gate closes when input becomes quiet.'],
      range_db: ['Attenuation', 'dB', 'How much the gate reduces quiet input. Higher values silence more noise.'],
      channels: ['Channels', '', 'Number of audio channels. Changing this can remove routing sockets; check your connections.'],
      ratio: ['Ratio', ':1', 'Compression ratio above the threshold. 1 leaves dynamics unchanged.'],
      knee_db: ['Knee', 'dB', 'Width of the gradual transition into compression. 0 uses a hard knee.'],
      makeup_db: ['Makeup gain', 'dB', 'Gain after compression. Positive values can overload downstream effects or outputs.'],
      ceiling_db: ['Ceiling', 'dBFS', 'Maximum output sample peak; this is not a true-peak or intersample limiter.'],
      lookahead_ms: ['Lookahead', 'ms', 'Delays the signal so the limiter can react before peaks reach the output. Changing this clears its delay history.'],
      q: ['Q', '', 'Width of all three peaking EQ bands. Higher Q makes a narrower band.'],
      reduction_db: ['Noise reduction', 'dB', 'Maximum suppression of estimated stationary noise. High values can make speech sound watery.'],
      noise_floor_db: ['Noise floor', 'dBFS', 'Starting noise estimate. Set below the quietest speech you want to preserve.'],
      adaptation: ['Adaptation', '', 'How quickly the estimated noise floor tracks quiet background noise. 0 freezes the estimate.'],
      smoothing: ['Smoothing', '', 'Smooths spectral suppression across time to reduce musical noise.'],
    }[name];
    const dynamics = type === 'compressor' || type === 'limiter' || type === 'multiband';
    if (dynamics && name === 'threshold_db') help[2] = 'Signals above this dBFS level are compressed.';
    if (dynamics && name === 'attack_ms') help[2] = 'How quickly gain reduction responds to a rising signal.';
    if (dynamics && name === 'release_ms') help[2] = 'How gradually gain recovers after a peak.';
    const band = name.match(/^(low|mid|high)_(hz|gain_db|threshold_db)$/);
    const bandHelp = band ? {
      unit: band[2] === 'hz' ? 'Hz' : 'dB',
      description: band[2] === 'hz' ? (type === 'multiband' ?
        'Crossover dividing the independently compressed bands.' : 'Center frequency of this peaking EQ band.') :
        band[2] === 'gain_db' ? 'Boost or cut around this band center.' : 'Signals above this level are compressed in this band.',
    } : {};
    return {
      ...(help ? { label: help[0], unit: help[1], description: help[2] } : { unit: '', description: '' }),
      ...bandHelp,
      ...(type === 'pitch' && name === 'quality' ? { options: ['High', 'Balanced', 'Economy'] } : {}),
      step: (type === 'pitch' && name === 'quality') || name === 'channels' || name.endsWith('_hz') ? 1 :
        name === 'noise_floor' ? 0.0001 : name.endsWith('_ms') || name.endsWith('_db') ? 0.1 : 0.01,
      precision: (type === 'pitch' && name === 'quality') || name === 'channels' || name.endsWith('_hz') ? 0 :
        name === 'noise_floor' ? 4 : name.endsWith('_ms') || name.endsWith('_db') ? 1 : 2,
    };
  }

  class EffectsManager {
    constructor(setStatus, onLayoutChange = () => {}) {
      this.setStatus = typeof setStatus === 'function' ? setStatus : (() => {});
      this.onLayoutChange = onLayoutChange;
      this.effectsById = new Map();
      this.nodeById = new Map();
      this.textValues = new Map();
      this.panelEl = null;
      this.panelStatusEl = null;
      this.ccPoll = null;
      this.notePoll = null;
      this.ccTarget = null;
      this.noteTarget = null;
      this.lastCcSeq = 0;
      this.lastNoteSeq = 0;
      this.paramUpdateTimers = new Map();
      this.mutationInFlight = 0;
      this.loadEpoch = 0;
      this.effectsSignature = '';
      this.nameDrafts = new Map();
      this.openSections = new Set();
      this.pluginCatalog = new Map();
      this.paramViews = new Map();
      this.customUiOpen = new Set();
      this.customUiViews = new Map();
      this.frames = new Map();
      this.messageListener = null;
    }

    setPluginCatalog(plugins) {
      const next = new Map();
      for (const plugin of Array.isArray(plugins) ? plugins : []) {
        const id = normalizePluginId(plugin?.id);
        if (id) next.set(id, plugin);
      }
      const signature = JSON.stringify([...next.entries()]);
      if (signature === this.catalogSignature) return;
      this.catalogSignature = signature;
      this.pluginCatalog = next;
      if (this.panelEl?.contains(document.activeElement) &&
          ['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName)) {
        this.effectsSignature = '';
        return;
      }
      this.refreshAllNodes();
      this.renderPanel();
    }

    isPluginEffect(fx) {
      if (!fx || effectTypes[fx.type]) return false;
      return fx.plugin_status !== undefined || fx.plugin_ui !== undefined ||
        this.pluginCatalog.has(normalizePluginId(fx.type));
    }

    typeLabel(fx) {
      if (!this.isPluginEffect(fx)) return effectTypes[fx.type] || 'Effect';
      const plugin = this.pluginCatalog.get(normalizePluginId(fx.type));
      if (!plugin) return `Plugin: ${String(fx.type || 'unknown')}`;
      return `Plugin: ${plugin.name || plugin.id}${plugin.version ? ` ${plugin.version}` : ''}`;
    }

    appendPluginSection(card, fx) {
      const effectId = normalizeEffectId(fx.id);
      const status = typeof fx.plugin_status === 'string' ? fx.plugin_status : '';
      const statusEl = document.createElement('p');
      statusEl.className = `small fx-plugin-status ${pluginStatusText[status] ? status : 'unknown'}`;
      statusEl.setAttribute('role', 'status');
      statusEl.textContent = pluginStatusText[status] || 'Plugin status unknown. Reload the plugin from Plugins.';
      card.appendChild(statusEl);
      if (typeof fx.plugin_error === 'string' && fx.plugin_error.trim()) {
        const error = document.createElement('p');
        error.className = 'small fx-plugin-error';
        error.textContent = `Plugin error: ${fx.plugin_error}`;
        card.appendChild(error);
      }
      if (fx.plugin_ui !== true) return;

      const section = document.createElement('div');
      section.className = 'fx-plugin-ui';
      const warning = document.createElement('p');
      warning.className = 'small fx-plugin-warning';
      warning.textContent = 'The custom UI runs scripts from the plugin package in a sandbox. A malicious package can show ' +
        'misleading controls; it can only change this effect\'s listed parameters. Open it only for plugins you trust.';
      const toggle = document.createElement('button');
      const host = document.createElement('div');
      host.className = 'fx-plugin-frame-host';
      const ready = status === 'ready' && !!pluginAssetUrl(fx.type);
      const view = {
        setOpen: (open) => {
          if (open && ready) {
            this.customUiOpen.add(effectId);
            this.openFrame(effectId, host);
          } else {
            if (!open) this.customUiOpen.delete(effectId);
            this.destroyFrame(effectId);
          }
          const isOpen = this.frames.has(effectId);
          toggle.textContent = isOpen ? 'Close custom UI' : 'Open custom UI';
          toggle.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
          toggle.disabled = !ready && !isOpen;
          toggle.title = ready ? '' : 'The custom UI is available only while the plugin is running.';
        },
      };
      toggle.setAttribute('aria-label', `Custom UI for ${effectName(fx)}`);
      toggle.addEventListener('click', () => view.setOpen(!this.frames.has(effectId)));
      this.customUiViews.set(effectId, view);
      section.append(warning, toggle, host);
      card.appendChild(section);
      view.setOpen(ready && this.customUiOpen.has(effectId));
    }

    openFrame(effectId, host) {
      this.destroyFrame(effectId);
      const fx = this.effectsById.get(effectId);
      const pluginId = normalizePluginId(fx?.type);
      const src = pluginAssetUrl(pluginId);
      if (!fx || !src) return;
      const iframe = document.createElement('iframe');
      iframe.setAttribute('sandbox', 'allow-scripts');
      iframe.setAttribute('referrerpolicy', 'no-referrer');
      iframe.referrerPolicy = 'no-referrer';
      iframe.className = 'fx-plugin-frame';
      iframe.title = `Custom UI for ${effectName(fx)}`;
      const onLoad = () => this.postParameters(effectId);
      iframe.addEventListener('load', onLoad);
      iframe.src = src;
      host.appendChild(iframe);
      this.frames.set(effectId, { iframe, host, onLoad, pluginId });
      this.ensureMessageListener();
    }

    destroyFrame(effectId) {
      const entry = this.frames.get(effectId);
      if (!entry) return;
      this.frames.delete(effectId);
      entry.iframe.removeEventListener?.('load', entry.onLoad);
      if (typeof entry.iframe.remove === 'function') entry.iframe.remove();
      else entry.iframe.parentNode?.removeChild(entry.iframe);
      if (!this.frames.size) this.removeMessageListener();
    }

    destroyFrames() {
      for (const id of [...this.frames.keys()]) this.destroyFrame(id);
      this.customUiViews.clear();
    }

    closeCustomUis() {
      for (const [id, view] of this.customUiViews) {
        if (this.frames.has(id) || this.customUiOpen.has(id)) view.setOpen(false);
      }
      for (const id of [...this.frames.keys()]) this.destroyFrame(id);
      this.customUiOpen.clear();
    }

    ensureMessageListener() {
      if (this.messageListener || typeof window.addEventListener !== 'function') return;
      this.messageListener = event => this.handleFrameMessage(event);
      window.addEventListener('message', this.messageListener);
    }

    removeMessageListener() {
      if (!this.messageListener) return;
      window.removeEventListener?.('message', this.messageListener);
      this.messageListener = null;
    }

    frameParameters(fx) {
      const meta = [];
      const params = {};
      for (const raw of Array.isArray(fx?.param_meta) ? fx.param_meta : []) {
        const name = typeof raw?.name === 'string' ? raw.name : '';
        if (!name) continue;
        const row = { ...raw, name, label: String(raw.label || name), min: toNumber(raw.min, 0), max: toNumber(raw.max, 1),
          default: toNumber(raw.default, toNumber(raw.min, 0)) };
        meta.push(row);
        params[name] = toNumber(fx.params?.[name], row.default);
      }
      return { params, param_meta: meta };
    }

    postParameters(effectId) {
      const entry = this.frames.get(effectId);
      const target = entry?.iframe.contentWindow;
      const fx = this.effectsById.get(effectId);
      if (!target || !fx || typeof target.postMessage !== 'function') return;
      // Sandboxed frames have an opaque origin, so no narrower target origin can match.
      target.postMessage({ type: 'audiox:parameters', id: effectId, ...this.frameParameters(fx) }, '*');
    }

    handleFrameMessage(event) {
      if (!event || !event.source || event.origin !== 'null') return;
      let effectId = '';
      let entry = null;
      for (const [id, candidate] of this.frames) {
        if (candidate.iframe.contentWindow === event.source) {
          effectId = id;
          entry = candidate;
          break;
        }
      }
      if (!entry) return;
      const data = event.data;
      if (!data || typeof data !== 'object' || Array.isArray(data) || data.type !== 'audiox:set-param') return;
      const fx = this.effectsById.get(effectId);
      if (!fx || normalizePluginId(fx.type) !== entry.pluginId || fx.plugin_status !== 'ready') return;
      const param = data.param;
      const value = data.value;
      const meta = typeof param === 'string' && Array.isArray(fx.param_meta)
        ? fx.param_meta.find(row => row && row.name === param) : null;
      const min = Number(meta?.min);
      const max = Number(meta?.max);
      if (!meta || typeof value !== 'number' || !Number.isFinite(value) || !Number.isFinite(min) ||
          !Number.isFinite(max) || value < min || value > max ||
          (meta.type && meta.type !== 'float' && !Number.isInteger(value))) {
        this.setPanelStatus(`${effectName(fx)} custom UI sent an invalid parameter change; it was ignored.`, 'warn');
        return;
      }
      this.queueParamUpdate(effectId, param, value);
    }

    syncParamView(effectId, param, value) {
      const view = this.paramViews.get(`${effectId}\n${param}`);
      if (!view) return;
      if (document.activeElement !== view.number) view.number.value = String(value);
      if (document.activeElement !== view.slider) view.slider.value = String(value);
      if (view.meta) view.meta.textContent = `${view.row.label}: ${view.row.options?.[Math.round(value)] || toNumber(value, 0).toFixed(view.row.precision)}`;
    }

    mountPanel(panelEl, statusEl) {
      this.panelEl = panelEl || null;
      this.panelStatusEl = statusEl || null;
      this.renderPanel();
    }

    setPanelStatus(text, kind = '') {
      if (!this.panelStatusEl) {
        return;
      }
      this.panelStatusEl.textContent = AudioxUI.formatMessage(text);
      this.panelStatusEl.className = `small ${kind}`.trim();
      if (kind === 'warn') this.setStatus(text, kind);
    }

    displayName(id) {
      return effectName(this.effectsById.get(id) || { id });
    }

    parameterName(id, param) {
      return dynamicParamList(this.effectsById.get(id)).find(row => row.name === param)?.label || param;
    }

    appendParameterControl(parent, row, fx) {
      const control = document.createElement('div');
      control.className = 'fx-parameter';
      const label = document.createElement('label');
      const inputId = `${fx.id}-${row.name}-value`;
      label.htmlFor = inputId;
      label.textContent = row.label + (row.unit ? ` (${row.unit})` : '');
      const heading = document.createElement('div');
      heading.className = 'fx-param-heading';
      heading.appendChild(label);
      const info = document.createElement('details');
      info.className = 'fx-param-info';
      const infoToggle = document.createElement('summary');
      infoToggle.textContent = 'i';
      infoToggle.setAttribute('aria-label', `Help for ${row.label}`);
      const help = document.createElement('p');
      help.className = 'small fx-param-help';
      help.id = `${inputId}-help`;
      help.textContent = row.description || (this.isPluginEffect(fx) ? '' :
        `Adjust ${row.label.toLowerCase()} between ${row.min} and ${row.max}.`);
      infoToggle.title = help.textContent;
      info.addEventListener('keydown', event => {
        if (event.key === 'Escape') {
          info.open = false;
          infoToggle.focus();
          event.stopPropagation();
        }
      });
      info.append(infoToggle, help);
      if (row.description || !this.isPluginEffect(fx)) heading.appendChild(info);
      control.appendChild(heading);
      const inputs = document.createElement('div');
      inputs.className = 'fx-param-inputs';
      const slider = document.createElement('input');
      slider.type = 'range';
      slider.setAttribute('aria-label', `${row.label} slider for ${effectName(fx)}`);
      const number = document.createElement(row.options ? 'select' : 'input');
      if (row.options) {
        row.options.forEach((name, value) => {
          const option = document.createElement('option');
          option.value = String(value);
          option.textContent = name;
          number.appendChild(option);
        });
      } else number.type = 'number';
      number.id = inputId;
      for (const input of [slider, number]) {
        input.min = String(row.min);
        input.max = String(row.max);
        input.step = String(row.step);
        input.value = String(row.value);
        input.title = help.textContent;
        input.setAttribute('aria-describedby', help.id);
      }
      const save = value => {
        number.value = String(value);
        slider.value = String(value);
        this.queueParamUpdate(fx.id, row.name, value);
      };
      slider.addEventListener('input', () => save(Number(slider.value)));
      number.addEventListener('change', () => {
        const value = Number(number.value);
        if (!number.value.trim() || !Number.isFinite(value) || value < row.min || value > row.max ||
          ((row.type === 'integer' || row.name === 'channels' || row.options) && !Number.isInteger(value))) {
          this.setPanelStatus(`${row.label} must be ${row.name === 'channels' ? 'a whole number ' : ''}between ${row.min} and ${row.max}.`, 'warn');
          return;
        }
        save(value);
      });
      const reset = document.createElement('button');
      reset.className = 'flat';
      reset.textContent = 'Reset';
      reset.setAttribute('aria-label', `Reset ${row.label} for ${effectName(fx)}`);
      reset.title = `Factory default: ${row.defaultValue}${row.unit ? ` ${row.unit}` : ''}`;
      reset.disabled = !Number.isFinite(row.defaultValue);
      reset.addEventListener('click', () => save(row.defaultValue));
      if (row.options) inputs.append(number, reset);
      else inputs.append(slider, number, reset);
      control.appendChild(inputs);
      parent.appendChild(control);
      this.paramViews.set(`${fx.id}\n${row.name}`, { slider, number, row, meta: null });
    }

    appendTextControls(parent, fx) {
      for (const text of Array.isArray(fx.text_controls) ? fx.text_controls : []) {
        const control = document.createElement('div');
        control.className = 'fx-parameter';
        const label = document.createElement('label');
        const input = document.createElement('input');
        const key = `${fx.id}\n${text.name}`;
        input.id = `${fx.id}-${text.name}-text`;
        input.type = 'text';
        input.maxLength = 255;
        input.value = this.textValues.get(key) ?? String(text.default || '');
        input.title = String(text.description || '');
        label.htmlFor = input.id;
        label.textContent = String(text.label || text.name);
        input.addEventListener('input', () => {
          this.textValues.set(key, input.value);
          this.refreshAllNodes();
        });
        const reset = document.createElement('button');
        reset.className = 'flat';
        reset.textContent = 'Reset';
        reset.addEventListener('click', () => {
          input.value = String(text.default || '');
          this.textValues.set(key, input.value);
          this.refreshAllNodes();
        });
        control.append(label, input, reset);
        parent.appendChild(control);
      }
    }

    rememberSection(section, key) {
      section.open = this.openSections.has(key);
      section.addEventListener('toggle', () => {
        if (section.open) this.openSections.add(key);
        else this.openSections.delete(key);
      });
    }

    renderPanel() {
      if (!this.panelEl) {
        return;
      }

      this.destroyFrames();
      this.paramViews.clear();
      this.panelEl.innerHTML = '';
      const effects = [...this.effectsById.values()]
        .filter((fx) => normalizeEffectId(fx?.id))
        .sort((a, b) => String(a.id).localeCompare(String(b.id)));

      if (!effects.length) {
        const empty = document.createElement('div');
        empty.className = 'small';
        empty.textContent = 'No effects. Add a processor from the Routing menu.';
        this.panelEl.appendChild(empty);
        return;
      }

      for (const fx of effects) {
        const effectId = normalizeEffectId(fx.id);
        const params = fx.params || {};
        const midi = fx.midi || {};
        const isCut = fx.type === 'cut';
        const cc = midi.cc || {};
        const light = midi.light || {};
        const paramList = dynamicParamList(fx);
        const enabled = toEnabled(fx.enabled);

        const card = document.createElement('section');
        card.className = 'card fx-card';

        const header = document.createElement('div');
        header.className = 'fx-card-head';
        header.textContent = effectName(fx);
        card.appendChild(header);
        const typeLabel = document.createElement('p');
        typeLabel.className = 'small';
        typeLabel.textContent = this.typeLabel(fx);
        card.appendChild(typeLabel);
        if (this.isPluginEffect(fx)) this.appendPluginSection(card, fx);

        const renameRow = document.createElement('div');
        renameRow.className = 'fx-name-row';
        const name = document.createElement('input');
        name.type = 'text';
        name.maxLength = 63;
        name.value = this.nameDrafts.get(effectId) ?? fx.custom_name ?? '';
        name.placeholder = effectName({ ...fx, name: '' });
        name.setAttribute('aria-label', `Name for ${effectName(fx)}`);
        name.addEventListener('input', () => this.nameDrafts.set(effectId, name.value));
        const rename = document.createElement('button');
        rename.textContent = 'Save name';
        rename.setAttribute('aria-label', `Save name for ${effectName(fx)}`);
        rename.addEventListener('click', async () => {
          try {
            rename.disabled = true;
            await this.setName(effectId, name.value.trim());
          } catch (error) {
            this.setPanelStatus(String(error), 'warn');
          } finally {
            rename.disabled = false;
          }
        });
        renameRow.append(name, rename);
        card.appendChild(renameRow);

        const enabledRow = document.createElement('div');
        enabledRow.className = 'fx-map-row';
        const enabledMeta = document.createElement('div');
        enabledMeta.className = 'fx-map-meta';
        enabledMeta.textContent = isCut
          ? `State: ${enabled ? 'Muted' : 'Passing'}`
          : `State: ${enabled ? 'Enabled' : 'Bypassed'}`;
        const enabledBtn = document.createElement('button');
        enabledBtn.textContent = isCut ? (enabled ? 'Unmute' : 'Mute') : (enabled ? 'Bypass' : 'Enable');
        enabledBtn.addEventListener('click', async () => {
          try {
            enabledBtn.disabled = true;
            await this.setEnabled(effectId, !toEnabled(this.effectsById.get(effectId)?.enabled));
            const nowEnabled = toEnabled(this.effectsById.get(effectId)?.enabled);
            this.setPanelStatus(isCut
              ? `${this.displayName(effectId)} ${nowEnabled ? 'muted' : 'passing'}`
              : `${this.displayName(effectId)} ${nowEnabled ? 'enabled' : 'bypassed'}`, 'ok');
          } catch (err) {
            this.setPanelStatus(String(err), 'warn');
          } finally {
            enabledBtn.disabled = false;
          }
        });
        enabledRow.appendChild(enabledMeta);
        enabledRow.appendChild(enabledBtn);
        card.appendChild(enabledRow);

        const mapping = document.createElement('details');
        mapping.className = 'fx-midi-details';
        this.rememberSection(mapping, `${effectId}-midi`);
        const summary = document.createElement('summary');
        summary.textContent = 'MIDI controls and pad lighting';
        mapping.appendChild(summary);
        for (const row of paramList) {
          this.appendParameterControl(card, row, fx);
          const line = document.createElement('div');
          line.className = 'fx-map-row';

          const left = document.createElement('div');
          left.className = 'fx-map-meta';
          left.textContent = `${row.label}: ${row.options?.[Math.round(row.value)] || toNumber(row.value, 0).toFixed(row.precision)}`;
          const view = this.paramViews.get(`${fx.id}\n${row.name}`);
          if (view) view.meta = left;

          const badge = document.createElement('span');
          const rowCc = toNumber(cc[row.name], -1);
          badge.className = `fx-map-badge${rowCc >= 0 ? '' : ' unset'}`;
          badge.textContent = rowCc >= 0 ? `CC ${rowCc}` : 'unmapped';

          const mapBtn = document.createElement('button');
          mapBtn.textContent = 'Map';
          mapBtn.setAttribute('aria-label', `Map MIDI CC to ${row.label} for ${effectName(fx)}`);
          mapBtn.disabled = isCut && row.name === 'channels';
          mapBtn.addEventListener('click', async () => {
            await this.beginCcCapture(effectId, row.name);
            this.setPanelStatus(`Move a MIDI CC for ${effectName(fx)}: ${row.label}`, 'ok');
          });

          const clearBtn = document.createElement('button');
          clearBtn.className = 'flat';
          clearBtn.textContent = 'Clear';
          clearBtn.setAttribute('aria-label', `Clear MIDI CC for ${row.label} on ${effectName(fx)}`);
          clearBtn.disabled = rowCc < 0;
          clearBtn.addEventListener('click', async () => {
            const ccNum = rowCc;
            if (ccNum < 0) {
              return;
            }
            if (!await window.AudioxUI.confirm(`Remove the MIDI CC ${ccNum} mapping for ${effectName(fx)}: ${row.label}?`, {
              title: 'Remove MIDI mapping?', label: 'Remove mapping',
            })) return;
            try {
              await this.deleteEffectCc(ccNum);
              this.setPanelStatus(`Cleared CC ${ccNum}`, 'ok');
            } catch (err) {
              this.setPanelStatus(String(err), 'warn');
            }
          });

          line.appendChild(left);
          line.appendChild(badge);
          line.appendChild(mapBtn);
          line.appendChild(clearBtn);
          mapping.appendChild(line);
        }
        if (this.isPluginEffect(fx)) this.appendTextControls(card, fx);

        const bypassRow = document.createElement('div');
        bypassRow.className = 'fx-map-row';
        const bypassMeta = document.createElement('div');
        bypassMeta.className = 'fx-map-meta';
        bypassMeta.textContent = isCut ? 'Mute Toggle' : 'Bypass Toggle';
        const note = toNumber(midi.toggle_note, -1);
        const bypassBadge = document.createElement('span');
        bypassBadge.className = `fx-map-badge${note >= 0 ? '' : ' unset'}`;
        bypassBadge.textContent = note >= 0 ? `Note ${note}` : 'unmapped';
        const bypassMap = document.createElement('button');
        bypassMap.textContent = 'Map';
        bypassMap.addEventListener('click', async () => {
          await this.beginNoteCapture(effectId);
          this.setPanelStatus(`Press a MIDI note for ${effectName(fx)} ${isCut ? 'mute' : 'bypass'}`, 'ok');
        });
        const bypassClear = document.createElement('button');
        bypassClear.className = 'flat';
        bypassClear.textContent = 'Clear';
        bypassClear.disabled = note < 0;
        bypassClear.addEventListener('click', async () => {
          if (note < 0) {
            return;
          }
          if (!await window.AudioxUI.confirm(`Remove the MIDI toggle mapping for ${effectName(fx)}?`, {
            title: 'Remove MIDI mapping?', label: 'Remove mapping',
          })) return;
          try {
            await this.deleteEffectToggle(note);
            this.setPanelStatus(`Cleared bypass note ${note}`, 'ok');
          } catch (err) {
            this.setPanelStatus(String(err), 'warn');
          }
        });
        bypassRow.appendChild(bypassMeta);
        bypassRow.appendChild(bypassBadge);
        bypassRow.appendChild(bypassMap);
        bypassRow.appendChild(bypassClear);
        mapping.appendChild(bypassRow);

        const lightRow = document.createElement('div');
        lightRow.className = 'fx-map-row';
        const lightMeta = document.createElement('div');
        lightMeta.className = 'fx-map-meta';
        lightMeta.textContent = isCut ? 'LED Colors (muted / passing)' : 'LED Colors (enabled / bypassed)';

        const enabledInput = document.createElement('input');
        enabledInput.type = 'number';
        enabledInput.min = '0';
        enabledInput.max = '127';
        enabledInput.step = '1';
        enabledInput.value = String(toNumber(light.enabled_vel, 0));
        enabledInput.setAttribute('aria-label', `${effectName(fx)} ${isCut ? 'muted' : 'enabled'} LED velocity`);
        enabledInput.style.width = '62px';

        const bypassedInput = document.createElement('input');
        bypassedInput.type = 'number';
        bypassedInput.min = '0';
        bypassedInput.max = '127';
        bypassedInput.step = '1';
        bypassedInput.value = String(toNumber(light.bypassed_vel, 0));
        bypassedInput.setAttribute('aria-label', `${effectName(fx)} ${isCut ? 'passing' : 'bypassed'} LED velocity`);
        bypassedInput.style.width = '62px';

        const lightSave = document.createElement('button');
        lightSave.textContent = 'Save';
        lightSave.addEventListener('click', async () => {
          const enabledVel = Math.max(0, Math.min(127, Math.round(toNumber(enabledInput.value, 0))));
          const bypassedVel = Math.max(0, Math.min(127, Math.round(toNumber(bypassedInput.value, 0))));
          try {
            lightSave.disabled = true;
            await this.setEffectLight(effectId, enabledVel, bypassedVel);
            this.setPanelStatus(`Saved LED colors for ${effectName(fx)}`, 'ok');
          } catch (err) {
            this.setPanelStatus(String(err), 'warn');
          } finally {
            lightSave.disabled = false;
          }
        });

        const lightClear = document.createElement('button');
        lightClear.className = 'flat';
        lightClear.textContent = 'Default';
        lightClear.addEventListener('click', async () => {
          if (!await window.AudioxUI.confirm(`Reset the custom LED colors for ${effectName(fx)}?`, {
            title: 'Reset effect lighting?', label: 'Reset lighting',
          })) return;
          try {
            lightClear.disabled = true;
            await this.deleteEffectLight(effectId);
            this.setPanelStatus(`Using default LED colors for ${effectName(fx)}`, 'ok');
          } catch (err) {
            this.setPanelStatus(String(err), 'warn');
          } finally {
            lightClear.disabled = false;
          }
        });

        lightRow.appendChild(lightMeta);
        lightRow.appendChild(enabledInput);
        lightRow.appendChild(bypassedInput);
        lightRow.appendChild(lightSave);
        lightRow.appendChild(lightClear);
        mapping.appendChild(lightRow);
        card.appendChild(mapping);
        const advanced = document.createElement('details');
        advanced.className = 'fx-advanced';
        this.rememberSection(advanced, `${effectId}-advanced`);
        const advancedTitle = document.createElement('summary');
        advancedTitle.textContent = 'Advanced and delete';
        advanced.appendChild(advancedTitle);
        const identity = document.createElement('p');
        identity.className = 'small';
        identity.textContent = `Script / routing ID: ${effectId}. Renaming does not change connections or scripts. Clear the name to restore the default.`;
        advanced.appendChild(identity);

        const deleteRow = document.createElement('div');
        deleteRow.className = 'fx-map-row';
        const deleteMeta = document.createElement('div');
        deleteMeta.className = 'fx-map-meta';
        deleteMeta.textContent = 'Delete Effect';
        const deleteBtn = document.createElement('button');
        deleteBtn.textContent = 'Delete';
        deleteBtn.addEventListener('click', async () => {
          try {
            deleteBtn.disabled = true;
            let deleted = true;
            if (typeof window.deleteEffectThing === 'function') {
              deleted = await window.deleteEffectThing(effectId);
            } else {
              if (!await window.AudioxUI.confirm(`Delete ${effectName(fx)}? Its settings and routing connections will be removed.`, {
                title: 'Delete effect?', label: 'Delete effect',
              })) return;
              await this.deleteEffect(effectId);
            }
            if (deleted !== false) this.setPanelStatus(`Deleted ${effectName(fx)}`, 'ok');
          } catch (err) {
            this.setPanelStatus(String(err), 'warn');
          } finally {
            deleteBtn.disabled = false;
          }
        });
        deleteRow.appendChild(deleteMeta);
        deleteRow.appendChild(deleteBtn);
        advanced.appendChild(deleteRow);
        card.appendChild(advanced);

        this.panelEl.appendChild(card);
      }
    }

    async loadEffects() {
      if (!this.mutationInFlight && this.panelEl?.contains(document.activeElement) &&
          ['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName)) return;
      const myEpoch = this.loadEpoch;
      const res = await fetch('/api/effects', { method: 'GET' });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effects load failed: ${res.status} ${txt.trim()}`);
      }
      const data = JSON.parse(txt);
      if (!data || !Array.isArray(data.effects)) {
        throw new Error('Effects data is not available. Refresh the page and try again.');
      }
      AudioxUI.resolve('effects-load');
      if (myEpoch !== this.loadEpoch) return;
      this.effectsById.clear();
      for (const fx of data.effects) {
        const id = normalizeEffectId(fx?.id);
        if (!id) continue;
        this.effectsById.set(id, fx);
      }
      for (const key of this.textValues.keys()) {
        const [id, name] = key.split('\n');
        if (!this.effectsById.get(id)?.text_controls?.some(text => text.name === name))
          this.textValues.delete(key);
      }
      const nextSig = this.buildEffectsSignature();
      if (nextSig === this.effectsSignature) {
        return;
      }
      this.effectsSignature = nextSig;
      this.refreshAllNodes();
      this.onLayoutChange();
      this.renderPanel();
    }

    registerNode(node) {
      const id = normalizeEffectId(node?.properties?.thingId);
      if (!id) {
        return;
      }
      this.nodeById.set(id, node);
      this.applyWidgets(node, this.effectsById.get(id));
    }

    unregisterNode(effectId) {
      const id = normalizeEffectId(effectId);
      if (!id) {
        return;
      }
      this.nodeById.delete(id);
    }

    refreshAllNodes() {
      for (const [, node] of this.nodeById.entries()) {
        const id = normalizeEffectId(node?.properties?.thingId);
        if (!id) continue;
        this.applyWidgets(node, this.effectsById.get(id));
      }
    }

    async postSet(body) {
      const res = await fetch('/api/effect/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody(body),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect update failed: ${res.status} ${txt.trim()}`);
      }
      return parseJson(txt) || {};
    }

    async setName(effectId, name) {
      if (new TextEncoder().encode(name).length > 63 || /[\u0000-\u001f\u007f-\u009f]/.test(name)) {
        throw new Error('Effect names allow up to 63 UTF-8 bytes and no control characters.');
      }
      const nameHex = Array.from(new TextEncoder().encode(name), byte => byte.toString(16).padStart(2, '0')).join('');
      this.loadEpoch++;
      this.mutationInFlight++;
      try {
        const res = await fetch('/api/effect/name', { method: 'POST',
          headers: { 'Content-Type': 'text/plain' }, body: `id=${effectId}&name_hex=${nameHex}` });
        if (!res.ok) throw new Error(`Could not save effect name: ${res.status} ${await res.text()}`);
        this.nameDrafts.delete(effectId);
        await this.loadEffects();
        this.setPanelStatus(`Saved name: ${this.displayName(effectId)}`, 'ok');
      } finally {
        this.mutationInFlight--;
      }
    }

    async setEnabled(effectId, enabled) {
      const nextEnabled = toEnabled(enabled);

      this.loadEpoch++;
      this.mutationInFlight++;
      try {
        await this.postSet({ id: effectId, enabled: nextEnabled ? 1 : 0 });
        await this.loadEffects();
        const isCut = this.effectsById.get(effectId)?.type === 'cut';
        this.setStatus(isCut
          ? `${this.displayName(effectId)} ${nextEnabled ? 'muted' : 'passing'}`
          : `${this.displayName(effectId)} ${nextEnabled ? 'enabled' : 'bypassed'}`, 'ok');
      } finally {
        this.mutationInFlight--;
      }
    }

    async setType(effectId, type) {
      this.loadEpoch++;
      this.mutationInFlight++;
      try {
        await this.postSet({ id: effectId, type });
        await this.loadEffects();
        this.setStatus(`${this.displayName(effectId)} type set to ${effectTypes[type] || type}`, 'ok');
      } finally {
        this.mutationInFlight--;
      }
    }

    async setParam(effectId, param, value) {
      const numeric = Number(value);
      if (!Number.isFinite(numeric)) {
        throw new Error('Effect parameter value must be a finite number.');
      }

      const fx = this.effectsById.get(effectId);
      const prev = Number(fx?.params?.[param]);
      if (Number.isFinite(prev) && prev === numeric) {
        return;
      }

      this.loadEpoch++;
      this.mutationInFlight++;
      try {
        const result = await this.postSet({ id: effectId, param, value: String(numeric) });
        const saved = typeof result.params?.[param] === 'number' && Number.isFinite(result.params[param])
          ? result.params[param] : numeric;
        const rendered = this.effectsSignature === this.buildEffectsSignature();
        if (fx && fx.params) fx.params[param] = saved;
        // Keep the open panel (and any custom UI frame) instead of re-rendering for our own save.
        if (rendered) this.effectsSignature = this.buildEffectsSignature();
        this.syncParamView(effectId, param, saved);
        this.refreshAllNodes();
        this.postParameters(effectId);
      } finally {
        this.mutationInFlight--;
      }
    }

    buildEffectsSignature() {
      const rows = [];
      for (const fx of this.effectsById.values()) {
        const id = normalizeEffectId(fx?.id);
        if (!id) {
          continue;
        }
        const type = String(fx?.type || 'gain');
        const enabled = fx?.enabled ? 1 : 0;
        const params = fx?.params || {};
        const paramList = dynamicParamList(fx);
        const midi = fx?.midi || {};
        const cc = midi?.cc || {};
        const light = midi?.light || {};
        const paramSig = paramList.map((p) => `${JSON.stringify(p)}:${toNumber(params[p.name], p.value)}`).join(';') +
          JSON.stringify(fx.text_controls || []);
        const ccSig = paramList.map((p) => `${p.name}:${toNumber(cc[p.name], -1)}`).join(';');
        rows.push([
          id,
          fx.name || '',
          fx.custom_name || '',
          type,
          enabled,
          paramSig,
          toNumber(midi.toggle_note, -1),
          `l:${toNumber(light.enabled_vel, -1)}:${toNumber(light.bypassed_vel, -1)}`,
          ccSig,
          `p:${fx.plugin_ui === true ? 1 : 0}:${String(fx.plugin_status ?? '')}:${String(fx.plugin_error ?? '')}`,
        ].join('|'));
      }
      rows.sort((a, b) => a.localeCompare(b));
      return rows.join('\n');
    }

    queueParamUpdate(effectId, param, value) {
      const key = `${effectId}:${param}`;
      const prev = this.paramUpdateTimers.get(key);
      if (prev) {
        clearTimeout(prev);
      }
      const timer = setTimeout(async () => {
        this.paramUpdateTimers.delete(key);
        try {
          await this.setParam(effectId, param, value);
          this.setStatus(`${this.displayName(effectId)}: ${this.parameterName(effectId, param)} saved`, 'ok');
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      }, 80);
      this.paramUpdateTimers.set(key, timer);
    }

    async setEffectCc(effectId, param, cc) {
      const res = await fetch('/api/effect/midi_cc/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ id: effectId, param, cc }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect CC mapping failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Mapped CC ${cc} to ${this.displayName(effectId)}: ${this.parameterName(effectId, param)}`, 'ok');
    }

    async deleteEffectCc(cc) {
      const res = await fetch('/api/effect/midi_cc/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ cc }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect CC unmap failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Removed CC ${cc} effect mapping`, 'ok');
    }

    async setEffectToggle(effectId, note) {
      const res = await fetch('/api/effect/midi_toggle/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ id: effectId, note }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect toggle mapping failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Mapped note ${note} to ${this.displayName(effectId)} toggle`, 'ok');
    }

    async deleteEffectToggle(note) {
      const res = await fetch('/api/effect/midi_toggle/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ note }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect toggle unmap failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Removed note ${note} bypass mapping`, 'ok');
    }

    async setEffectLight(effectId, enabledVel, bypassedVel) {
      const res = await fetch('/api/effect/midi_light/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ id: effectId, enabled_vel: enabledVel, bypassed_vel: bypassedVel }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect light mapping failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Updated LED colors for ${this.displayName(effectId)}`, 'ok');
    }

    async deleteEffectLight(effectId) {
      const res = await fetch('/api/effect/midi_light/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ id: effectId }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect light mapping delete failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Reset LED colors for ${this.displayName(effectId)}`, 'ok');
    }

    async deleteEffect(effectId) {
      const name = this.displayName(effectId);
      const res = await fetch('/api/effect/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ id: effectId }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect delete failed: ${res.status} ${txt.trim()}`);
      }
      this.nameDrafts.delete(effectId);
      await this.loadEffects();
      this.setStatus(`Deleted ${name}`, 'ok');
    }

    async beginCcCapture(effectId, param) {
      try {
        const snap = await fetch('/api/midi/last_cc', { method: 'GET' });
        if (snap.ok) {
          const d = parseJson(await snap.text()) || {};
          this.lastCcSeq = toNumber(d.last_seq, 0);
        }
      } catch (_) {}

      this.ccTarget = { effectId, param };
      if (this.ccPoll) {
        clearInterval(this.ccPoll);
      }

      this.setStatus(`Move a MIDI CC to map ${this.displayName(effectId)}: ${this.parameterName(effectId, param)}...`, 'ok');
      this.renderPanel();
      this.ccPoll = setInterval(() => this.pollCcCapture(), 220);
    }

    async pollCcCapture() {
      if (!this.ccTarget) {
        if (this.ccPoll) {
          clearInterval(this.ccPoll);
          this.ccPoll = null;
        }
        return;
      }

      try {
        const res = await fetch('/api/midi/last_cc', { method: 'GET' });
        const txt = await res.text();
        if (!res.ok) return;
        const data = parseJson(txt) || {};
        if (!data.connected) return;

        const seq = toNumber(data.last_seq, 0);
        const cc = toNumber(data.last_cc, -1);
        if (seq > this.lastCcSeq && cc >= 0 && cc <= 127) {
          this.lastCcSeq = seq;
          const target = this.ccTarget;
          this.ccTarget = null;
          if (this.ccPoll) {
            clearInterval(this.ccPoll);
            this.ccPoll = null;
          }
          await this.setEffectCc(target.effectId, target.param, cc);
          this.setPanelStatus(`Mapped CC ${cc} to ${this.displayName(target.effectId)}: ${this.parameterName(target.effectId, target.param)}`, 'ok');
        }
      } catch (_) {}
    }

    async beginNoteCapture(effectId) {
      try {
        const snap = await fetch('/api/midi/last_note', { method: 'GET' });
        if (snap.ok) {
          const d = parseJson(await snap.text()) || {};
          this.lastNoteSeq = toNumber(d.last_seq, 0);
        }
      } catch (_) {}

      this.noteTarget = { effectId };
      if (this.notePoll) {
        clearInterval(this.notePoll);
      }

      const isCut = this.effectsById.get(effectId)?.type === 'cut';
      this.setStatus(`Press a MIDI note to map ${this.displayName(effectId)} ${isCut ? 'mute' : 'bypass'} toggle...`, 'ok');
      this.renderPanel();
      this.notePoll = setInterval(() => this.pollNoteCapture(), 220);
    }

    async pollNoteCapture() {
      if (!this.noteTarget) {
        if (this.notePoll) {
          clearInterval(this.notePoll);
          this.notePoll = null;
        }
        return;
      }

      try {
        const res = await fetch('/api/midi/last_note', { method: 'GET' });
        const txt = await res.text();
        if (!res.ok) return;
        const data = parseJson(txt) || {};
        if (!data.connected) return;

        const seq = toNumber(data.last_seq, 0);
        const note = toNumber(data.last_note, -1);
        if (seq > this.lastNoteSeq && note >= 0 && note <= 127) {
          this.lastNoteSeq = seq;
          const target = this.noteTarget;
          this.noteTarget = null;
          if (this.notePoll) {
            clearInterval(this.notePoll);
            this.notePoll = null;
          }
          await this.setEffectToggle(target.effectId, note);
          const isCut = this.effectsById.get(target.effectId)?.type === 'cut';
          this.setPanelStatus(`Mapped note ${note} to ${this.displayName(target.effectId)} ${isCut ? 'mute' : 'bypass'}`, 'ok');
        }
      } catch (_) {}
    }

    applyWidgets(node, fx) {
      if (!node) {
        return;
      }
      const effectId = normalizeEffectId(node?.properties?.thingId);
      if (!effectId) {
        return;
      }

      const hasFx = !!fx;
      const type = String(fx?.type || 'gain');
      const enabled = toEnabled(fx?.enabled);
      const params = fx?.params || {};
      const midi = fx?.midi || {};
      const paramList = dynamicParamList(fx);

      node.widgets = [];
      node.size = [340, hasFx ? Math.max(210, 150 + (paramList.length * 36)) : 130];
      node.title = hasFx
        ? `${effectName(fx)}${type === 'cut' ? (enabled ? ' (muted)' : '') : (enabled ? '' : ' (bypassed)')}`
        : 'Effect (loading...)';

      if (!hasFx) {
        node.addWidget('button', 'Reload Effect State', null, async () => {
          try {
            await this.loadEffects();
          } catch (err) {
            this.setStatus(String(err), 'warn');
          }
        });
        return;
      }

      for (const p of paramList) {
        if (p.options) {
          node.addWidget('combo', p.label, p.options[Math.round(toNumber(params[p.name], p.value))], (value) => {
            this.queueParamUpdate(effectId, p.name, p.options.indexOf(value));
          }, { values: p.options });
        } else {
          node.addWidget('slider', p.label, toNumber(params[p.name], p.value), (v) => {
            this.queueParamUpdate(effectId, p.name, p.type === 'integer' ? Math.round(v) : v);
          }, { min: p.min, max: p.max, step: p.step, precision: p.precision });
        }
      }
      for (const text of Array.isArray(fx.text_controls) ? fx.text_controls : []) {
        const key = `${fx.id}\n${text.name}`;
        node.addWidget('text', text.label || text.name,
          this.textValues.get(key) ?? String(text.default || ''),
          value => {
            this.textValues.set(key, String(value).slice(0, 255));
            this.renderPanel();
          });
      }

      node.addWidget('button', 'Edit effect and MIDI controls', null, () => {
        const btn = document.querySelector('.tab-btn[data-tab="effects"]');
        if (btn) {
          btn.click();
        }
      });

      // Deletion is handled from the Effects page panel, not routing-node widgets.
    }
  }

  window.AudioxEffects = {
    effectName, dynamicParamList, normalizePluginId, pluginAssetUrl,
    createManager(setStatus, onLayoutChange) {
      return new EffectsManager(setStatus, onLayoutChange);
    },
  };
})();
