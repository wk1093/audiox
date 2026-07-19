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

  class EffectsManager {
    constructor(setStatus) {
      this.setStatus = typeof setStatus === 'function' ? setStatus : (() => {});
      this.effectsById = new Map();
      this.nodeById = new Map();
      this.ccPoll = null;
      this.notePoll = null;
      this.ccTarget = null;
      this.noteTarget = null;
      this.lastCcSeq = 0;
      this.lastNoteSeq = 0;
      this.paramUpdateTimers = new Map();
    }

    async loadEffects() {
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
      this.refreshAllNodes();
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

    async setEnabled(effectId, enabled) {
      await this.postSet({ id: effectId, enabled: enabled ? 1 : 0 });
      await this.loadEffects();
      this.setStatus(`${effectId} ${enabled ? 'enabled' : 'bypassed'}`, 'ok');
    }

    async setType(effectId, type) {
      await this.postSet({ id: effectId, type });
      await this.loadEffects();
      this.setStatus(`${effectId} type set to ${type}`, 'ok');
    }

    async setParam(effectId, param, value) {
      const numeric = Number(value);
      if (!Number.isFinite(numeric)) {
        return;
      }
      await this.postSet({ id: effectId, param, value: numeric.toFixed(4) });

      const fx = this.effectsById.get(effectId);
      if (fx && fx.params) {
        fx.params[param] = numeric;
      }
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
        headers: { 'Content-Type': 'text/plain' },
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
        headers: { 'Content-Type': 'text/plain' },
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
        headers: { 'Content-Type': 'text/plain' },
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
        headers: { 'Content-Type': 'text/plain' },
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
        headers: { 'Content-Type': 'text/plain' },
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
      const enabled = !!fx?.enabled;
      const params = fx?.params || {};
      const midi = fx?.midi || {};
      const cc = midi.cc || {};

      node.widgets = [];
      node.size = [320, hasFx ? 370 : 130];
      node.title = hasFx
        ? `${effectId} (${type}${enabled ? '' : ' bypass'})`
        : `${effectId} (loading...)`;

      if (!hasFx) {
        node.addWidget('button', 'Reload Effect State', '', async () => {
          try {
            await this.loadEffects();
          } catch (err) {
            this.setStatus(String(err), 'warn');
          }
        });
        return;
      }

      node.addWidget('toggle', 'Enabled', enabled, async (v) => {
        try {
          await this.setEnabled(effectId, !!v);
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      }, { on: 'on', off: 'bypass' });

      node.addWidget('combo', 'Type', type, async (v) => {
        try {
          await this.setType(effectId, String(v || 'gain'));
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      }, { values: ['gain', 'distortion'] });

      node.addWidget('slider', 'Gain', toNumber(params.gain, 1), (v) => {
        this.queueParamUpdate(effectId, 'gain', v);
      }, { min: 0, max: 4, step: 0.01, precision: 2 });

      node.addWidget('slider', 'Drive', toNumber(params.drive, 1.5), (v) => {
        this.queueParamUpdate(effectId, 'drive', v);
      }, { min: 0, max: 8, step: 0.01, precision: 2 });

      node.addWidget('slider', 'Clip', toNumber(params.clip, 0.6), (v) => {
        this.queueParamUpdate(effectId, 'clip', v);
      }, { min: 0.05, max: 1, step: 0.01, precision: 2 });

      node.addWidget('slider', 'Output', toNumber(params.output, 1), (v) => {
        this.queueParamUpdate(effectId, 'output', v);
      }, { min: 0, max: 4, step: 0.01, precision: 2 });

      node.addWidget('button', `Map Gain CC${cc.gain >= 0 ? ` (CC ${cc.gain})` : ''}`, '', async () => {
        try {
          await this.beginCcCapture(effectId, 'gain');
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', `Map Drive CC${cc.drive >= 0 ? ` (CC ${cc.drive})` : ''}`, '', async () => {
        try {
          await this.beginCcCapture(effectId, 'drive');
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', `Map Clip CC${cc.clip >= 0 ? ` (CC ${cc.clip})` : ''}`, '', async () => {
        try {
          await this.beginCcCapture(effectId, 'clip');
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', `Map Output CC${cc.output >= 0 ? ` (CC ${cc.output})` : ''}`, '', async () => {
        try {
          await this.beginCcCapture(effectId, 'output');
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', `Map Bypass Note${toNumber(midi.toggle_note, -1) >= 0 ? ` (N ${midi.toggle_note})` : ''}`, '', async () => {
        try {
          await this.beginNoteCapture(effectId);
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', 'Clear CC Mappings', '', async () => {
        try {
          const ccMap = [cc.gain, cc.drive, cc.clip, cc.output];
          for (const ccNum of ccMap) {
            if (toNumber(ccNum, -1) >= 0) {
              // eslint-disable-next-line no-await-in-loop
              await this.deleteEffectCc(ccNum);
            }
          }
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', 'Clear Bypass Note', '', async () => {
        try {
          const note = toNumber(midi.toggle_note, -1);
          if (note >= 0) {
            await this.deleteEffectToggle(note);
          }
        } catch (err) {
          this.setStatus(String(err), 'warn');
        }
      });

      node.addWidget('button', 'Delete Effect', '', async () => {
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
