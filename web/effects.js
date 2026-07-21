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

  class EffectsManager {
    constructor(setStatus) {
      this.setStatus = typeof setStatus === 'function' ? setStatus : (() => {});
      this.effectsById = new Map();
      this.nodeById = new Map();
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
      this.panelStatusEl.textContent = String(text || '');
      this.panelStatusEl.className = `small ${kind}`.trim();
    }

    renderPanel() {
      if (!this.panelEl) {
        return;
      }

      this.panelEl.innerHTML = '';
      const effects = [...this.effectsById.values()]
        .filter((fx) => normalizeEffectId(fx?.id))
        .sort((a, b) => String(a.id).localeCompare(String(b.id)));

      if (!effects.length) {
        const empty = document.createElement('div');
        empty.className = 'small';
        empty.textContent = 'No effects yet. Add one from Routing > Add menu.';
        this.panelEl.appendChild(empty);
        return;
      }

      for (const fx of effects) {
        const effectId = normalizeEffectId(fx.id);
        const params = fx.params || {};
        const midi = fx.midi || {};
        const cc = midi.cc || {};
        const meta = paramMetaForType(fx.type);
        const enabled = toEnabled(fx.enabled);

        const card = document.createElement('section');
        card.className = 'card fx-card';

        const header = document.createElement('div');
        header.className = 'fx-card-head';
        header.textContent = `${effectId} (${fx.type || 'gain'})`;
        card.appendChild(header);

        const enabledRow = document.createElement('div');
        enabledRow.className = 'fx-map-row';
        const enabledMeta = document.createElement('div');
        enabledMeta.className = 'fx-map-meta';
        enabledMeta.textContent = `State: ${enabled ? 'Enabled' : 'Bypassed'}`;
        const enabledBtn = document.createElement('button');
        enabledBtn.textContent = enabled ? 'Bypass' : 'Enable';
        enabledBtn.addEventListener('click', async () => {
          try {
            enabledBtn.disabled = true;
            await this.setEnabled(effectId, !toEnabled(this.effectsById.get(effectId)?.enabled));
            this.setPanelStatus(`${effectId} ${toEnabled(this.effectsById.get(effectId)?.enabled) ? 'enabled' : 'bypassed'}`, 'ok');
          } catch (err) {
            this.setPanelStatus(String(err), 'warn');
          } finally {
            enabledBtn.disabled = false;
          }
        });
        enabledRow.appendChild(enabledMeta);
        enabledRow.appendChild(enabledBtn);
        card.appendChild(enabledRow);

        const rows = [
          { key: 'gain', label: meta.gain.label, value: params.gain, cc: cc.gain },
          { key: 'drive', label: meta.drive.label, value: params.drive, cc: cc.drive },
          { key: 'clip', label: meta.clip.label, value: params.clip, cc: cc.clip },
          { key: 'output', label: meta.output.label, value: params.output, cc: cc.output },
        ];

        for (const row of rows) {
          const line = document.createElement('div');
          line.className = 'fx-map-row';

          const left = document.createElement('div');
          left.className = 'fx-map-meta';
          left.textContent = `${row.label}: ${toNumber(row.value, 0).toFixed(2)}`;

          const badge = document.createElement('span');
          badge.className = `fx-map-badge${toNumber(row.cc, -1) >= 0 ? '' : ' unset'}`;
          badge.textContent = toNumber(row.cc, -1) >= 0 ? `CC ${row.cc}` : 'unmapped';

          const mapBtn = document.createElement('button');
          mapBtn.textContent = 'Map';
          mapBtn.addEventListener('click', async () => {
            await this.beginCcCapture(effectId, row.key);
            this.setPanelStatus(`Move a MIDI CC for ${effectId}.${row.key}`, 'ok');
          });

          const clearBtn = document.createElement('button');
          clearBtn.className = 'flat';
          clearBtn.textContent = 'Clear';
          clearBtn.disabled = toNumber(row.cc, -1) < 0;
          clearBtn.addEventListener('click', async () => {
            const ccNum = toNumber(row.cc, -1);
            if (ccNum < 0) {
              return;
            }
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
          card.appendChild(line);
        }

        const bypassRow = document.createElement('div');
        bypassRow.className = 'fx-map-row';
        const bypassMeta = document.createElement('div');
        bypassMeta.className = 'fx-map-meta';
        bypassMeta.textContent = 'Bypass Toggle';
        const note = toNumber(midi.toggle_note, -1);
        const bypassBadge = document.createElement('span');
        bypassBadge.className = `fx-map-badge${note >= 0 ? '' : ' unset'}`;
        bypassBadge.textContent = note >= 0 ? `Note ${note}` : 'unmapped';
        const bypassMap = document.createElement('button');
        bypassMap.textContent = 'Map';
        bypassMap.addEventListener('click', async () => {
          await this.beginNoteCapture(effectId);
          this.setPanelStatus(`Press a MIDI note for ${effectId} bypass`, 'ok');
        });
        const bypassClear = document.createElement('button');
        bypassClear.className = 'flat';
        bypassClear.textContent = 'Clear';
        bypassClear.disabled = note < 0;
        bypassClear.addEventListener('click', async () => {
          if (note < 0) {
            return;
          }
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
        card.appendChild(bypassRow);

        this.panelEl.appendChild(card);
      }
    }

    async loadEffects() {
      const myEpoch = this.loadEpoch;
      const res = await fetch('/api/effects', { method: 'GET' });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effects load failed: ${res.status} ${txt.trim()}`);
      }
      const data = parseJson(txt) || {};
      this.effectsById.clear();
      const effects = Array.isArray(data.effects) ? data.effects : [];
      for (const fx of effects) {
        const id = normalizeEffectId(fx?.id);
        if (!id) continue;
        this.effectsById.set(id, fx);
      }
      const nextSig = this.buildEffectsSignature();
      if (nextSig === this.effectsSignature) {
        return;
      }
      this.effectsSignature = nextSig;
      // Skip updating nodes if a mutation started while we were fetching;
      // the mutation's own loadEffects call will apply the correct state.
      if (myEpoch !== this.loadEpoch) {
        return;
      }
      this.refreshAllNodes();
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
      if (body && body.id && Object.prototype.hasOwnProperty.call(body, 'enabled')) {
        this.setStatus(`sending enable request for ${body.id}: ${body.enabled}`, 'ok');
      }
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

    async setEnabled(effectId, enabled) {
      const current = this.effectsById.get(effectId);
      const nextEnabled = toEnabled(enabled);
      this.setStatus(`setEnabled(${effectId}) -> ${nextEnabled ? 1 : 0}`, 'ok');

      if (current) {
        current.enabled = nextEnabled;
        this.refreshAllNodes();
        this.renderPanel();
      }

      this.loadEpoch++;
      this.mutationInFlight++;
      try {
        await this.postSet({ id: effectId, enabled: nextEnabled ? 1 : 0 });
        await this.loadEffects();
        this.setStatus(`${effectId} ${nextEnabled ? 'enabled' : 'bypassed'}`, 'ok');
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
        this.setStatus(`${effectId} type set to ${type}`, 'ok');
      } finally {
        this.mutationInFlight--;
      }
    }

    async setParam(effectId, param, value) {
      const numeric = Number(value);
      if (!Number.isFinite(numeric)) {
        return;
      }

      const fx = this.effectsById.get(effectId);
      const prev = Number(fx?.params?.[param]);
      if (Number.isFinite(prev) && Math.abs(prev - numeric) < 0.0005) {
        return;
      }

      await this.postSet({ id: effectId, param, value: numeric.toFixed(4) });

      if (fx && fx.params) {
        fx.params[param] = numeric;
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
        const midi = fx?.midi || {};
        const cc = midi?.cc || {};
        rows.push([
          id,
          type,
          enabled,
          toNumber(params.gain, 0).toFixed(4),
          toNumber(params.drive, 0).toFixed(4),
          toNumber(params.clip, 0).toFixed(4),
          toNumber(params.output, 0).toFixed(4),
          toNumber(midi.toggle_note, -1),
          toNumber(cc.gain, -1),
          toNumber(cc.drive, -1),
          toNumber(cc.clip, -1),
          toNumber(cc.output, -1),
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
          this.setStatus(`${effectId} ${param} updated`, 'ok');
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
      this.setStatus(`Mapped CC ${cc} to ${effectId}.${param}`, 'ok');
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
      this.setStatus(`Mapped note ${note} to ${effectId} bypass`, 'ok');
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

    async deleteEffect(effectId) {
      const res = await fetch('/api/effect/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8' },
        body: formBody({ id: effectId }),
      });
      const txt = await res.text();
      if (!res.ok) {
        throw new Error(`effect delete failed: ${res.status} ${txt.trim()}`);
      }
      await this.loadEffects();
      this.setStatus(`Deleted ${effectId}`, 'ok');
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

      this.setStatus(`Move a MIDI CC to map ${effectId}.${param}...`, 'ok');
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
          this.setPanelStatus(`Mapped CC ${cc} to ${target.effectId}.${target.param}`, 'ok');
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

      this.setStatus(`Press a MIDI note to map ${effectId} bypass toggle...`, 'ok');
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
          this.setPanelStatus(`Mapped note ${note} to ${target.effectId} bypass`, 'ok');
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
      const meta = paramMetaForType(type);

      node.widgets = [];
      node.size = [320, hasFx ? 340 : 130];
      node.title = hasFx
        ? `${effectId} (${type}${enabled ? '' : ' bypass'})`
        : `${effectId} (loading...)`;

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

      node.addWidget('slider', meta.gain.label, toNumber(params.gain, 1), (v) => {
        this.queueParamUpdate(effectId, 'gain', v);
      }, { min: meta.gain.min, max: meta.gain.max, step: meta.gain.step, precision: meta.gain.precision });

      node.addWidget('slider', meta.drive.label, toNumber(params.drive, 1.5), (v) => {
        this.queueParamUpdate(effectId, 'drive', v);
      }, { min: meta.drive.min, max: meta.drive.max, step: meta.drive.step, precision: meta.drive.precision });

      node.addWidget('slider', meta.clip.label, toNumber(params.clip, 0.6), (v) => {
        this.queueParamUpdate(effectId, 'clip', v);
      }, { min: meta.clip.min, max: meta.clip.max, step: meta.clip.step, precision: meta.clip.precision });

      node.addWidget('slider', meta.output.label, toNumber(params.output, 1), (v) => {
        this.queueParamUpdate(effectId, 'output', v);
      }, { min: meta.output.min, max: meta.output.max, step: meta.output.step, precision: meta.output.precision });

      node.addWidget('button', 'Open Effects Mapping Page', null, () => {
        const btn = document.querySelector('.tab-btn[data-tab="effects"]');
        if (btn) {
          btn.click();
        }
      });

      node.addWidget('button', 'Delete Effect', null, async () => {
        try {
          await this.deleteEffect(effectId);
          if (typeof window.loadRoutingThingsWithOptions === 'function') {
            await window.loadRoutingThingsWithOptions({ silent: true, force: true });
          }
          if (typeof window.loadRoutingFile === 'function') {
            await window.loadRoutingFile();
          }
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });
    }
  }

  window.AudioxEffects = {
    createManager(setStatus) {
      return new EffectsManager(setStatus);
    },
  };
})();
