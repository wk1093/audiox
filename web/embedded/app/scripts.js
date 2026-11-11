let editingMacroId = null;
const defaultScriptSource = 'hook("run", function(event)\n  log("Hello from AudioX")\nend)\n';
let macroCount = 0;
const scriptControlEdits = new Set();

function scriptListReloadSafe(preserveEditor) {
  return !(preserveEditor && AudioxUI.hasUnsavedChanges('macro-editor')) &&
    !Array.from(scriptControlEdits).some(control => control.pending || control.dirty);
}

async function loadMidiChords({ preserveEditor = false } = {}) {
  if (!scriptListReloadSafe(preserveEditor)) return;
  try {
    const res = await apiFetch('/api/scripts', { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const data = await res.json();
    if (scriptListReloadSafe(preserveEditor)) renderMidiChords(data.scripts);
  } catch (err) {
    setStatus(`Script load failed: ${err}`, 'warn', 'midi-chords');
  }
}

function renderMidiChords(chords) {
  macroCount = chords.length;
  const list = document.getElementById('chord-list');
  list.replaceChildren();
  scriptControlEdits.clear();
  for (const chord of chords) {
    const row = document.createElement('div');
    const label = document.createElement('span');
    label.textContent = `${chord.name} — ${chord.midi_enabled ? `Channel ${chord.channel}, note ${chord.note}` : 'Event hooks'} ` +
      `(LED idle ${chord.idle_velocity ?? 0}, pressed ${chord.pressed_velocity ?? 0})`;
    if (chord.runtime_error_hex) {
      label.textContent += ` — Last execution failed: ${chordHexDecode(chord.runtime_error_hex)}`;
    }
    if (chord.ui_error_hex) {
      label.textContent += ` — Script controls/settings load failed: ${chordHexDecode(chord.ui_error_hex)}`;
    }
    const edit = document.createElement('button');
    edit.textContent = 'Edit';
    edit.addEventListener('click', async () => {
      if (AudioxUI.hasUnsavedChanges('macro-editor') && !await AudioxUI.confirm(
        'Discard unsaved script changes and edit this saved script?', {
          title: 'Discard script changes?', label: 'Discard and edit',
        })) return;
      cancelChordLearn();
      editingMacroId = chord.id;
      document.getElementById('macro-name').value = chord.name;
      document.getElementById('macro-midi-enabled').checked = chord.midi_enabled;
      document.getElementById('chord-channel').value = chord.channel;
      document.getElementById('chord-note').value = chord.note;
      AudioxLuaEditor.setValue(document.getElementById('chord-script'), chordHexDecode(chord.script_hex || ''));
      document.getElementById('macro-config-write').checked = Boolean(chord.config_write_allowed);
      document.getElementById('chord-idle-velocity').value = chord.idle_velocity ?? 0;
      document.getElementById('chord-pressed-velocity').value = chord.pressed_velocity ?? 0;
      updateMacroBinding();
      AudioxUI.setUnsaved('macro-editor', false);
      document.getElementById('btn-chord-save').textContent = 'Save Changes';
      document.getElementById('macro-name').focus();
    });
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.addEventListener('click', async () => {
      if (!await AudioxUI.confirm(`Delete the saved script "${chord.name}"? This cannot be undone.`, {
        title: 'Delete script?', label: 'Delete script',
      })) return;
      await saveMidiChord('DELETE', chord, remove);
    });
    const run = document.createElement('button');
    run.textContent = 'Run';
    run.disabled = !(chord.event_mask & 128);
    if (run.disabled) run.title = 'Add hook("run", function(event) ... end) to enable Run';
    run.addEventListener('click', () => runMacro(chord.id, run));
    row.className = 'stack';
    const actions = document.createElement('div');
    actions.className = 'macro-actions';
    actions.append(edit, remove, run);
    row.append(label, actions);
    if (Array.isArray(chord.ui) && chord.ui.length) {
      const controls = document.createElement('div');
      controls.className = 'script-controls';
      for (const descriptor of chord.ui) {
        if (!descriptor || typeof descriptor.key !== 'string' ||
            !['boolean', 'number', 'string', 'button'].includes(descriptor.type)) continue;
        const field = document.createElement('label');
        field.className = 'script-control';
        const input = document.createElement(descriptor.type === 'button' ? 'button' : 'input');
        const caption = String(descriptor.label ?? descriptor.key);
        if (descriptor.type === 'button') {
          input.type = 'button';
          input.textContent = caption;
        } else {
          const text = document.createElement('span');
          text.textContent = caption;
          field.append(text);
          input.type = { boolean: 'checkbox', number: 'number', string: 'text' }[descriptor.type];
          if (descriptor.type === 'number') {
            input.step = 'any';
            for (const bound of ['min', 'max']) {
              if (typeof descriptor[bound] === 'number' && Number.isFinite(descriptor[bound])) {
                input[bound] = String(descriptor[bound]);
              }
            }
          } else if (descriptor.type === 'string') {
            input.maxLength = 127;
            input.title = 'Up to 127 UTF-8 bytes';
          }
        }
        const control = { input, descriptor, savedValue: descriptor.value, pending: false, dirty: false };
        scriptControlEdits.add(control);
        restoreScriptControl(control);
        input.addEventListener('input', () => { control.dirty = true; });
        input.addEventListener(descriptor.type === 'button' ? 'click' : 'change',
          () => saveScriptControl(chord.id, control));
        field.append(input);
        controls.append(field);
      }
      row.append(controls);
    }
    list.appendChild(row);
  }
}

function restoreScriptControl(control) {
  if (control.descriptor.type === 'boolean') control.input.checked = Boolean(control.savedValue);
  else if (control.descriptor.type !== 'button') control.input.value = String(control.savedValue);
  control.dirty = false;
}

async function saveScriptControl(id, control) {
  if (control.pending || control.input.disabled) return;
  const { input, descriptor } = control;
  control.pending = true;
  input.disabled = true;
  let saved = false;
  try {
    let value;
    if (descriptor.type === 'boolean') value = input.checked;
    else if (descriptor.type === 'number') {
      value = Number(input.value);
      if (!String(input.value).trim() || !Number.isFinite(value) ||
          (input.min !== undefined && input.min !== '' && value < Number(input.min)) ||
          (input.max !== undefined && input.max !== '' && value > Number(input.max))) {
        throw new Error('Enter a finite number within the displayed bounds');
      }
    } else if (descriptor.type === 'string') {
      value = input.value;
      if (new TextEncoder().encode(value).length > 127) {
        throw new Error('Text must be at most 127 UTF-8 bytes');
      }
    }
    const body = `id=${id}&key_hex=${chordHexEncode(descriptor.key)}` +
      (descriptor.type === 'button' ? '' : `&value_hex=${chordHexEncode(String(value))}`);
    const res = await apiFetch('/api/scripts/control', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' }, body,
    });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const result = await res.text();
    control.savedValue = value;
    descriptor.value = value;
    control.dirty = false;
    saved = true;
    setStatus(descriptor.type === 'button' ?
      'Script button queued (not confirmation of completion)' :
      result.includes('config_change queued') ?
        'Script setting saved; config_change queued (not confirmation of completion)' :
        'Script setting saved', 'ok', 'midi-chords');
  } catch (err) {
    restoreScriptControl(control);
    setStatus(`Script control failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    control.pending = false;
    input.disabled = false;
  }
  if (saved) await loadMidiChords({ preserveEditor: true });
}

async function saveMidiChord(method, chord, button) {
  if (button.disabled) return;
  button.disabled = true;
  try {
    const activityScript = method === 'POST' &&
      /\bdevice_active\b/.test(chordHexDecode(chord.script_hex || ''));
    if (activityScript && !await AudioxUI.confirm(
      'Device activity hooks can run ten times per second per device. Repeated Lua evaluation can cause significant CPU usage and compete with audio processing. Keep callbacks small. Save this script?', {
        title: 'High-frequency activity hook', label: 'Acknowledge and save',
      })) return;
    const configurationScript = method === 'POST' &&
      (Boolean(chord.config_write_allowed) ||
        /\b(?:config|settings|system|bluetooth)\b/.test(chordHexDecode(chord.script_hex || '')));
    if (configurationScript && !await AudioxUI.confirm(
      'This script may write official configuration, restart the system, or manage Bluetooth devices. Configuration writes can replace persistent settings, including MIDI bindings, routing and startup configuration, and whole-file writes are not format-validated. Back up your settings first; reload or restart may be required. Allow these permissions and save this script?', {
        title: 'Allow configuration and system management?', label: 'Allow and save script',
      })) return;
    const payload = method === 'DELETE' ? `id=${chord.id}` :
      (chord.id ? `id=${chord.id}&` : '') +
      `name_hex=${chordHexEncode(chord.name)}&midi_enabled=${chord.midi_enabled ? 1 : 0}` +
      `&channel=${chord.channel}&note=${chord.note}` +
      `&idle_velocity=${chord.idle_velocity ?? 0}&pressed_velocity=${chord.pressed_velocity ?? 0}` +
      `&config_write_ack=${configurationScript ? 1 : 0}` +
      `&activity_ack=${activityScript ? 1 : 0}` +
      `&script_hex=${chord.script_hex || ''}`;
    let res = await apiFetch('/api/scripts', {
      method, headers: { 'Content-Type': 'text/plain' }, body: payload,
    });
    if (method === 'POST' && res.status === 409 && !activityScript) {
      const warning = await res.text();
      if (!warning.includes('device_active')) throw new Error(`409: ${warning}`);
      if (!await AudioxUI.confirm(warning, {
        title: 'High-frequency activity hook', label: 'Acknowledge and save',
      })) return;
      res = await apiFetch('/api/scripts', {
        method, headers: { 'Content-Type': 'text/plain' },
        body: payload.replace('&activity_ack=0', '&activity_ack=1'),
      });
    }
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    if (method === 'POST' || editingMacroId === chord.id) resetMacroEditor();
    setStatus(method === 'DELETE' ? 'Script removed' : 'Script saved', 'ok', 'midi-chords');
    await loadMidiChords();
  } catch (err) {
    setStatus(`Script save failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = false;
  }
}

async function runMacro(id, button) {
  if (button.disabled) return;
  button.disabled = true;
  try {
    const res = await apiFetch('/api/scripts/run', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: `id=${id}`,
    });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    setStatus('Script queued (not confirmation of completion)', 'ok', 'midi-chords');
    await loadMidiChords();
  } catch (err) {
    setStatus(`Script run failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = false;
  }
}

function updateMacroBinding() {
  const enabled = document.getElementById('macro-midi-enabled').checked;
  if (!enabled) cancelChordLearn();
  document.getElementById('btn-chord-learn').disabled = !enabled;
  document.getElementById('macro-midi-binding').hidden = !enabled;
}

function resetMacroEditor() {
  cancelChordLearn();
  editingMacroId = null;
  document.getElementById('macro-name').value = '';
  document.getElementById('macro-midi-enabled').checked = false;
  document.getElementById('chord-channel').value = '0';
  document.getElementById('chord-note').value = '60';
  AudioxLuaEditor.setValue(document.getElementById('chord-script'), defaultScriptSource);
  document.getElementById('macro-config-write').checked = false;
  document.getElementById('chord-idle-velocity').value = '0';
  document.getElementById('chord-pressed-velocity').value = '0';
  document.getElementById('btn-chord-save').textContent = 'Save Script';
  AudioxUI.setUnsaved('macro-editor', false);
  updateMacroBinding();
}

function chordHexEncode(text) {
  return Array.from(new TextEncoder().encode(text), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function chordHexDecode(hex) {
  return new TextDecoder().decode(Uint8Array.from(hex.match(/../g) || [], (byte) => parseInt(byte, 16)));
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopMeterPolling();
  else if (state.activeTab === 'volume') startMeterPolling();
  if (document.hidden) {
    cancelChordLearn();
    effectsManager?.closeCustomUis();
  }
});

function cancelChordLearn() {
  if (state.mappingAssignTarget !== '__macro__') return;
  state.mappingAssignTarget = null;
  if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
  state.mappingPollTimer = null;
  document.getElementById('btn-chord-learn').setAttribute('aria-pressed', 'false');
  document.getElementById('chord-learn-status').textContent = 'MIDI learning stopped.';
}

document.getElementById('btn-chord-learn').addEventListener('click', async () => {
  if (!document.getElementById('macro-midi-enabled').checked) return;
  if (state.mappingAssignTarget === '__macro__') {
    cancelChordLearn();
    return;
  }
  const button = document.getElementById('btn-chord-learn');
  if (button.disabled) return;
  button.disabled = true;
  state.mappingAssignTarget = null;
  if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
  state.mappingPollTimer = null;
  try {
    const res = await apiFetch('/api/midi/last_note', { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status}: ${await res.text()}`);
    const data = await res.json();
    if (state.activeTab !== 'scripts' || document.hidden ||
        !document.getElementById('macro-midi-enabled').checked) return;
    const seq = Number(data.last_seq);
    if (!Number.isInteger(seq) || seq < 0) throw new Error('device did not report a valid MIDI event sequence');
    state.mappingLastSeq = seq;
    state.mappingAssignTarget = '__macro__';
    if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
    button.setAttribute('aria-pressed', 'true');
    document.getElementById('chord-learn-status').textContent = 'Press a pad. Select Learn MIDI Pad again to cancel.';
  } catch (err) {
    setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-chords');
  } finally {
    button.disabled = !document.getElementById('macro-midi-enabled').checked;
  }
});

document.getElementById('macro-midi-enabled').addEventListener('change', updateMacroBinding);
document.getElementById('btn-macro-new').addEventListener('click', async () => {
  if (AudioxUI.hasUnsavedChanges('macro-editor') && !await AudioxUI.confirm(
    'Discard unsaved script changes and start a new script?', {
      title: 'Discard script changes?', label: 'Discard and start new',
    })) return;
  resetMacroEditor();
});
document.getElementById('btn-chord-load').addEventListener('click', loadMidiChords);
document.getElementById('btn-chord-save').addEventListener('click', () => {
  const channel = Number(document.getElementById('chord-channel').value);
  const name = document.getElementById('macro-name').value.trim();
  const midi_enabled = document.getElementById('macro-midi-enabled').checked;
  const note = Number(document.getElementById('chord-note').value);
  const script = document.getElementById('chord-script').value;
  const idle = Number(document.getElementById('chord-idle-velocity').value);
  const pressed = Number(document.getElementById('chord-pressed-velocity').value);
  if (!name || name.includes('\0') || new TextEncoder().encode(name).length > 63 ||
      (!editingMacroId && macroCount >= 16) ||
      !Number.isInteger(channel) || channel < 0 || channel > 15 ||
      !Number.isInteger(note) || note < 0 || note > 127 ||
      !Number.isInteger(idle) || idle < 0 || idle > 127 ||
      !Number.isInteger(pressed) || pressed < 0 || pressed > 127 ||
      !script.trim() || script.includes('\0') ||
      new TextEncoder().encode(script).length > 17408) {
    setStatus('Enter a name (1-63 UTF-8 bytes), channel 0-15, note/LED values 0-127, and a Lua callback script (up to 17,408 UTF-8 bytes). Maximum 16 scripts.', 'warn', 'midi-chords');
    return;
  }
  return saveMidiChord('POST', { id: editingMacroId, name, midi_enabled, channel, note, idle_velocity: idle,
    pressed_velocity: pressed,
    config_write_allowed: script && document.getElementById('macro-config-write').checked,
    script_hex: chordHexEncode(script) }, document.getElementById('btn-chord-save'));
});

