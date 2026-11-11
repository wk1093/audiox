async function loadMappings() {
  try {
    const res = await apiFetch('/api/midi/mappings', { method: 'GET' });
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
    const res = await apiFetch('/api/midi/actions', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      throw new Error(`${res.status} ${txt.trim()}`);
    }

    const data = JSON.parse(txt);
    state.stopAllMidiNote = null;
    state.samplerToggleMidiNote = null;
    state.passthroughToggleMidiNote = null;
    state.passthroughToggleMidiChannel = null;
    const actions = Array.isArray(data?.actions) ? data.actions : [];
    for (const item of actions) {
      const action = String(item?.action || '');
      if (action !== 'stop_all' && action !== 'sampler_toggle' && action !== 'passthrough_toggle') {
        continue;
      }
      const note = Number(item?.note);
      if (Number.isFinite(note) && note >= 0 && note <= 127) {
        if (action === 'stop_all' && state.stopAllMidiNote === null) {
          state.stopAllMidiNote = note;
        }
        if (action === 'sampler_toggle' && state.samplerToggleMidiNote === null) {
          state.samplerToggleMidiNote = note;
        }
        if (action === 'passthrough_toggle' && state.passthroughToggleMidiNote === null) {
          state.passthroughToggleMidiNote = note;
          state.passthroughToggleMidiChannel = item.channel;
        }
      }
    }
    renderStopAllMidiStatus();
    renderSamplerToggleMidiStatus();
    document.getElementById('passthrough-toggle-midi').textContent =
      state.passthroughToggleMidiNote === null ? 'unassigned' :
        `channel ${state.passthroughToggleMidiChannel}, note ${state.passthroughToggleMidiNote}`;
  } catch (error) {
    setStatus(`Could not load MIDI action mappings: ${error}`, 'warn');
  }
  renderSoundboardGrid();
}

let passthroughRequestPending = false;
async function loadPassthroughOnly(toggle = false) {
  if (passthroughRequestPending) return;
  passthroughRequestPending = true;
  const button = document.getElementById('btn-passthrough-only');
  button.disabled = true;
  try {
    const response = await apiFetch('/api/midi/passthrough_only', { method: toggle ? 'POST' : 'GET' });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${text.trim()}`);
    const { active } = JSON.parse(text);
    button.setAttribute('aria-pressed', String(active));
    button.textContent = active ? 'Restore MIDI Controls' : 'Enable Passthrough Only';
    document.getElementById('btn-assign-passthrough-toggle').disabled = !!active;
  } catch (error) {
    setStatus(`Passthrough-only mode: ${error}`, 'warn', 'midi-passthrough');
  } finally {
    button.disabled = false;
    passthroughRequestPending = false;
  }
}

async function saveActionMapping(note, actionName, channel) {
  if (!Number.isFinite(note) || note < 0 || note > 127) {
    setStatus('invalid midi note, expected 0-127', 'warn');
    return;
  }

  if (!actionName) {
    setStatus('invalid action mapping', 'warn');
    return;
  }

  try {
    const body = `note=${note}&action=${actionName}` +
      (actionName === 'passthrough_toggle' ? `&channel=${channel}` : '');
    const res = await apiFetch('/api/midi/action/set', {
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
    setStatus(`mapped note ${note} -> ${actionName}`, 'ok');
    return true;
  } catch (err) {
    setStatus(`action mapping save error: ${err}`, 'warn');
  }
}

async function saveStopAllActionMapping(note) {
  await saveActionMapping(note, 'stop_all');
}

async function saveSamplerToggleActionMapping(note) {
  await saveActionMapping(note, 'sampler_toggle');
}

async function deleteStopAllActionMapping() {
  if (!Number.isFinite(state.stopAllMidiNote)) {
    setStatus('no stop-all MIDI mapping to remove', 'warn');
    return;
  }
  if (!await AudioxUI.confirm('Remove the MIDI mapping for Stop All?', {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;

  try {
    const res = await apiFetch('/api/midi/action/delete', {
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

async function deleteSamplerToggleActionMapping() {
  if (!Number.isFinite(state.samplerToggleMidiNote)) {
    setStatus('no sampler-toggle MIDI mapping to remove', 'warn');
    return;
  }
  if (!await AudioxUI.confirm('Remove the MIDI mapping for Sampler Toggle?', {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;

  try {
    const res = await apiFetch('/api/midi/action/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `note=${state.samplerToggleMidiNote}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`action mapping delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await loadMidiActions();
    setStatus('sampler-toggle MIDI mapping removed', 'ok');
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
    const res = await apiFetch('/api/midi/mapping/set', {
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
  if (!await AudioxUI.confirm(`Remove the soundboard MIDI mapping for note ${note}?`, {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;

  try {
    const res = await apiFetch('/api/midi/mapping/delete', {
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

  const targetAtStart = state.mappingAssignTarget;
  try {
    const res = await apiFetch('/api/midi/last_note', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      if (targetAtStart === '__macro__' || targetAtStart === '__action_passthrough_toggle__' ||
          String(targetAtStart).startsWith('__grid_'))
        throw new Error(`${res.status}: ${txt}`);
      return;
    }
    if (targetAtStart !== state.mappingAssignTarget) return;

    const data = JSON.parse(txt);
    const connected = !!data.connected;
    const seq = Number(data.last_seq || 0);
    const note = Number(data.last_note);

    const isMacroTarget = state.mappingAssignTarget === '__macro__';
    const isGridTarget = state.mappingAssignTarget.startsWith('__grid_');
    const isSbTarget = !isMacroTarget && state.mappingAssignTarget !== '__action_stop_all__' &&
                       state.mappingAssignTarget !== '__action_sampler_toggle__' &&
                       state.mappingAssignTarget !== '__action_passthrough_toggle__' && !isGridTarget;
    const statusEl2 = isMacroTarget ? document.getElementById('chord-learn-status') :
      (isGridTarget ? document.getElementById('sb-grid-learn-status') :
        (isSbTarget ? sbMappingStatusEl : midiActionCaptureStatusEl));

    if (!connected) {
      if (statusEl2) statusEl2.textContent = 'Waiting for MIDI device\u2026';
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
      if (target === '__macro__') {
        document.getElementById('btn-chord-learn').setAttribute('aria-pressed', 'false');
        if (!Number.isInteger(data.last_channel) || data.last_channel < 0 || data.last_channel > 15) {
          throw new Error('device did not report a valid MIDI channel');
        }
        document.getElementById('chord-channel').value = data.last_channel;
        document.getElementById('chord-note').value = note;
        AudioxUI.setUnsaved('macro-editor');
        statusEl2.textContent = `Channel ${data.last_channel}, note ${note} captured. Save Script to apply the binding.`;
      } else if (target === '__action_stop_all__') {
        await saveStopAllActionMapping(note);
        if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = `Assigned note ${note} to stop all.`;
      } else if (target === '__action_sampler_toggle__') {
        await saveSamplerToggleActionMapping(note);
        if (midiActionCaptureStatusEl) midiActionCaptureStatusEl.textContent = `Assigned note ${note} to sampler toggle.`;
      } else if (target === '__action_passthrough_toggle__') {
        if (!Number.isInteger(data.last_channel) || data.last_channel < 0 || data.last_channel > 15)
          throw new Error('device did not report a valid MIDI channel');
        if (await saveActionMapping(note, 'passthrough_toggle', data.last_channel) && midiActionCaptureStatusEl)
          midiActionCaptureStatusEl.textContent =
            `Assigned channel ${data.last_channel}, note ${note} to passthrough toggle.`;
      } else if (target.startsWith('__grid_')) {
        const corner = target.slice('__grid_'.length, -2);
        await saveSoundboardGridCorner(corner, note);
      } else {
        await saveMapping(note, target);
        if (sbMappingStatusEl) sbMappingStatusEl.textContent = `Assigned note ${note} to "${target}".`;
      }
      renderSoundboardGrid();
    }
  } catch (err) {
    if (targetAtStart === '__macro__') {
      cancelChordLearn();
      setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-chords');
    } else if (targetAtStart === '__action_passthrough_toggle__') {
      state.mappingAssignTarget = null;
      clearInterval(state.mappingPollTimer);
      state.mappingPollTimer = null;
      setStatus(`MIDI learn failed: ${err}`, 'warn', 'midi-passthrough');
    } else if (String(targetAtStart).startsWith('__grid_')) {
      document.getElementById('sb-grid-learn-status').textContent = `Grid MIDI learn failed: ${err}`;
      setStatus(`Grid MIDI learn failed: ${err}`, 'warn', 'soundboard-layout');
    }
  }
}

async function loadSamplerConfig() {
  try {
    const res = await apiFetch('/api/midi/sampler_config', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      return;
    }
    const data = JSON.parse(txt);
    state.samplerKeyboardEnabled = !!data.keyboard_enabled;
    state.samplerKeyboardChannel = Number(data.keyboard_channel ?? 0);
    state.samplerRootNote = Number(data.root_note ?? 60);

    const enabledEl = document.getElementById('sampler_keyboard_enabled');
    const channelEl = document.getElementById('sampler_keyboard_channel');
    const rootEl = document.getElementById('sampler_root_note');
    if (enabledEl) enabledEl.value = state.samplerKeyboardEnabled ? '1' : '0';
    if (channelEl) channelEl.value = String(state.samplerKeyboardChannel);
    if (rootEl) rootEl.value = String(state.samplerRootNote);
    AudioxUI.setUnsaved('sampler-config', false);
  } catch (_) {
  }
}

async function saveSamplerConfig() {
  const enabled = document.getElementById('sampler_keyboard_enabled')?.value === '1' ? 1 : 0;
  const channel = Number(document.getElementById('sampler_keyboard_channel')?.value ?? 0);
  const rootNote = Number(document.getElementById('sampler_root_note')?.value ?? 60);

  if (!Number.isFinite(channel) || channel < 0 || channel > 15) {
    setStatus('invalid sampler keyboard channel, expected 0-15', 'warn');
    return;
  }
  if (!Number.isFinite(rootNote) || rootNote < 0 || rootNote > 127) {
    setStatus('invalid sampler root note, expected 0-127', 'warn');
    return;
  }

  try {
    const body = `keyboard_enabled=${enabled}&keyboard_channel=${Math.trunc(channel)}&root_note=${Math.trunc(rootNote)}`;
    const res = await apiFetch('/api/midi/sampler_config', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`sampler config save failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    await loadSamplerConfig();
    AudioxUI.setUnsaved('sampler-config', false);
    setStatus('sampler config saved', 'ok');
  } catch (err) {
    setStatus(`sampler config save error: ${err}`, 'warn');
  }
}

const lightDimButton = document.getElementById('btn-light-dim');
let lightDimBusy = false;

function renderLightDim(dimmed) {
  lightDimButton.textContent = dimmed ? 'Restore lights' : 'Dim lights';
  lightDimButton.setAttribute('aria-pressed', String(dimmed));
  lightDimButton.disabled = false;
}

async function requestLightDim(dimmed) {
  const res = await apiFetch('/api/midi/light_dim', dimmed === undefined
    ? { cache: 'no-store', signal: AbortSignal.timeout(10000) }
    : {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `dimmed=${dimmed ? 1 : 0}`,
      signal: AbortSignal.timeout(10000),
    });
  if (!res.ok) throw new Error(`MIDI lights request failed: ${res.status} ${await res.text()}`);
  const data = await res.json();
  if (data.ok !== true || typeof data.dimmed !== 'boolean') {
    throw new Error('MIDI lights returned an unexpected response. Try again.');
  }
  renderLightDim(data.dimmed);
  return data.dimmed;
}

async function loadLightDim() {
  if (lightDimBusy) return;
  lightDimBusy = true;
  lightDimButton.disabled = true;
  try {
    await requestLightDim();
    AudioxUI.resolve('midi-light-dim');
  } catch (err) {
    setStatus(`MIDI lights load failed: ${err}`, 'warn', 'midi-light-dim');
  } finally {
    lightDimBusy = false;
  }
}

lightDimButton.addEventListener('click', async () => {
  if (lightDimBusy) return;
  const dimmed = lightDimButton.getAttribute('aria-pressed') !== 'true';
  lightDimBusy = true;
  lightDimButton.disabled = true;
  try {
    const result = await requestLightDim(dimmed);
    setStatus(result ? 'MIDI lights dimmed until restored or rebooted.' : 'MIDI lights restored.', 'ok', 'midi-light-dim');
  } catch (err) {
    setStatus(`MIDI lights change failed: ${err}`, 'warn', 'midi-light-dim');
  } finally {
    lightDimButton.disabled = false;
    lightDimBusy = false;
  }
});

async function loadLightConfig() {
  try {
    const res = await apiFetch('/api/midi/light_config', { method: 'GET' });
    if (!res.ok) return;
    const data = JSON.parse(await res.text());
    document.getElementById('light_channel').value = String(data.channel ?? 0);
    document.getElementById('light_mapped_vel').value = String(data.mapped_vel ?? 0);
    document.getElementById('light_stop_all_vel').value = String(data.stop_all_vel ?? 0);
    document.getElementById('light_playing_vel').value = String(data.playing_vel ?? 0);
    AudioxUI.setUnsaved('light-config', false);
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
    const res = await apiFetch('/api/midi/light_config', {
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
    AudioxUI.setUnsaved('light-config', false);
  } catch (err) {
    setStatus(`lighting save error: ${err}`, 'warn');
  }
}

document.getElementById('btn-light-save').addEventListener('click', saveLightConfig);

document.getElementById('btn-light-refresh').addEventListener('click', async () => {
  setStatus('refreshing LEDs...');
  try {
    const res = await apiFetch('/api/midi/light_refresh', { method: 'POST' });
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

