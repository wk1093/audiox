// ---- Volume page ----

const volumeMeters = new Map();
let meterPollTimer = null;
let meterPollBusy = false;
let meterTopology = null;
const meterLevels = new WeakMap();
const meterReducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
let meterAnimationFrame = null;
let meterAnimationTime = 0;

function resetVolumeMeters() {
  if (meterAnimationFrame !== null) cancelAnimationFrame(meterAnimationFrame);
  meterAnimationFrame = null;
  for (const [id, group] of volumeMeters) {
    updateVolumeMeter(id, Array(group.children[0]?.children.length || 0).fill(0), true);
  }
}

function animateVolumeMeters(now) {
  meterAnimationFrame = null;
  if (state.activeTab !== 'volume' || document.hidden) {
    resetVolumeMeters();
    return;
  }
  const elapsed = Math.max(0, now - meterAnimationTime);
  meterAnimationTime = now;
  let moving = false;
  for (const group of volumeMeters.values()) {
    for (const row of group.children[0]?.children || []) {
      const meter = row.children[0];
      const level = meterLevels.get(meter);
      if (!level) continue;
      // Smooth in dB, with a fast attack (35 ms) and gentler release (300 ms).
      const duration = level.target > level.current ? 35 : 300;
      level.current += (level.target - level.current) * -Math.expm1(-elapsed / duration);
      if (meterReducedMotion.matches || Math.abs(level.target - level.current) < 0.05) {
        level.current = level.target;
      } else {
        moving = true;
      }
      meter.value = (level.current + 60) / 60;
      row.style.setProperty('--meter-level', `${meter.value * 100}%`);
    }
  }
  if (moving) meterAnimationFrame = requestAnimationFrame(animateVolumeMeters);
}

function stopMeterPolling() {
  if (meterPollTimer !== null) clearInterval(meterPollTimer);
  meterPollTimer = null;
  resetVolumeMeters();
}

function startMeterPolling() {
  stopMeterPolling();
  if (document.hidden) return;
  pollMeters();
  meterPollTimer = setInterval(pollMeters, 250);
}

function updateVolumeMeter(id, levels, immediate = false) {
  const group = volumeMeters.get(id);
  if (!group) return;
  if (!levels.length) {
    group.replaceChildren();
    return;
  }
  if (group.children[0]?.children.length !== levels.length) {
    group.replaceChildren();
    const bars = document.createElement('div');
    bars.className = 'vol-meter-bars';
    const name = state.volumes?.find(vol => vol.id === id)?.name || id;
    levels.forEach((_, channel) => {
      const row = document.createElement('div');
      row.className = 'vol-meter-row';
      const meter = document.createElement('meter');
      meter.min = 0;
      meter.max = 1;
      meter.high = 0.95;
      meter.optimum = 0;
      meter.value = 0;
      row.style.setProperty('--meter-level', '0%');
      meterLevels.set(meter, { current: -60, target: -60 });
      meter.setAttribute('aria-label', `${name} channel ${channel + 1} signal level`);
      row.appendChild(meter);
      bars.appendChild(row);
    });
    const value = document.createElement('span');
    value.className = 'vol-meter-reading';
    group.append(bars, value);
  }
  levels.forEach((raw, channel) => {
    const level = Number.isFinite(raw) ? Math.max(0, Math.min(1, raw)) : 0;
    const db = level > 0 ? 20 * Math.log10(level) : -Infinity;
    const row = group.children[0].children[channel];
    const meter = row.children[0];
    const display = meterLevels.get(meter);
    display.target = Math.max(-60, db);
    if (immediate || meterReducedMotion.matches) {
      display.current = display.target;
      meter.value = (display.current + 60) / 60;
      row.style.setProperty('--meter-level', `${meter.value * 100}%`);
    }
    meter.setAttribute('aria-valuetext', `${db > -60 ? db.toFixed(1) : '-inf'} dBFS`);
  });
  const level = levels.reduce((sum, raw) =>
    sum + (Number.isFinite(raw) ? Math.max(0, Math.min(1, raw)) : 0), 0) / levels.length;
  const db = level > 0 ? 20 * Math.log10(level) : -Infinity;
  group.children[1].textContent = `${db > -60 ? db.toFixed(1) : '-inf'} dB`;
  group.children[1].title = `Average channel peak: ${db > -60 ? db.toFixed(1) : '-inf'} dBFS`;
  if (!immediate && !meterReducedMotion.matches && meterAnimationFrame === null && levels.length) {
    meterAnimationTime = performance.now();
    meterAnimationFrame = requestAnimationFrame(animateVolumeMeters);
  }
}

async function pollMeters() {
  if (meterPollBusy || state.activeTab !== 'volume' || document.hidden) return;
  meterPollBusy = true;
  const status = document.getElementById('vol-meter-status');
  try {
    const res = await apiFetch('/api/audio/meters', { cache: 'no-store', signal: AbortSignal.timeout(2000) });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    if (!Array.isArray(data.meters) || data.meters.some(meter =>
      typeof meter.id !== 'string' || !Array.isArray(meter.levels) ||
      meter.levels.length > 16 || meter.levels.some(level => !Number.isFinite(level)))) {
      throw new Error('Invalid meter data');
    }
    if (state.activeTab !== 'volume' || document.hidden) return;
    const topology = JSON.stringify(data.meters.map(meter => [meter.id, meter.levels.length]));
    if (meterTopology !== null && topology !== meterTopology && !(await loadVolumes())) {
      throw new Error('Could not refresh meter channels');
    }
    meterTopology = topology;
    if (state.activeTab !== 'volume' || document.hidden) return;
    const liveIds = new Set(data.meters.map(meter => meter.id));
    for (const [id, group] of volumeMeters) {
      if (!liveIds.has(id)) group.replaceChildren();
    }
    data.meters.forEach(meter => updateVolumeMeter(meter.id, meter.levels));
    if (status) {
      status.textContent = '';
      status.hidden = true;
    }
  } catch (err) {
    if (state.activeTab !== 'volume' || document.hidden) return;
    resetVolumeMeters();
    const message = `Meters unavailable: ${AudioxUI.formatMessage(err)}`;
    if (status) {
      status.textContent = message;
      status.hidden = false;
    }
  } finally {
    meterPollBusy = false;
  }
}

async function loadVolumes() {
  try {
    const [vRes, ccRes] = await Promise.all([
      apiFetch('/api/audio/volumes', { method: 'GET' }),
      apiFetch('/api/midi/cc_volumes', { method: 'GET' }),
    ]);
    if (!vRes.ok || !ccRes.ok) {
      throw new Error(`Volume data load failed (${!vRes.ok ? vRes.status : ccRes.status}). Try refreshing the Volume page.`);
    }
    const volumes = await vRes.json();
    const mappings = await ccRes.json();
    state.volumes = Array.isArray(volumes?.volumes) ? volumes.volumes : [];
    state.ccVolumes = Array.isArray(mappings?.mappings) ? mappings.mappings : [];
    renderVolGrid();
    AudioxUI.resolve('volumes-load');
    return true;
  } catch (err) {
    setStatus(`volume load error: ${err}`, 'warn', 'volumes-load');
    return false;
  }
}

function ccForThing(thingId) {
  const m = state.ccVolumes.find((e) => e.id === thingId);
  return m ? Number(m.cc) : null;
}

function gainToPercent(gain) {
  return Math.round(Number(gain) * 100);
}

function percentToGain(pct) {
  return Number(pct) / 100;
}

function renderVolGrid() {
  if (!volGridEl) return;
  volumeMeters.clear();
  volGridEl.innerHTML = '';
  const audioVolumes = state.volumes.filter(vol => vol.id !== 'routing_helper');
  const effects = audioVolumes.filter(vol => isEffectThingId(vol.id));
  const visibleVolumes = audioVolumes.filter(vol => state.volShowEffects || !isEffectThingId(vol.id));
  const effectSummary = document.getElementById('vol-effect-summary');
  if (effectSummary) {
    const adjusted = effects.filter(vol => vol.gain !== 1).length;
    effectSummary.textContent = !state.volShowEffects && effects.length
      ? `${effects.length} effect ${effects.length === 1 ? 'channel' : 'channels'} hidden${adjusted ? `; ${adjusted} with adjusted gain` : ''}. Saved gain, MIDI mappings, and script control remain active.`
      : '';
    effectSummary.hidden = !effectSummary.textContent;
  }

  if (!visibleVolumes.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = audioVolumes.length
      ? 'Only effect channels are available. Enable Show effect channels to see them.'
      : 'No active devices. Open the Routing tab first.';
    volGridEl.appendChild(empty);
    return;
  }

  for (const vol of visibleVolumes) {
    const { id, name, gain } = vol;
    const cc = ccForThing(id);
    const isTarget = state.volAssignTarget === id;

    const card = document.createElement('div');
    card.className = `vol-card${isTarget ? ' cc-target' : ''}`;

    const nameEl = document.createElement('div');
    nameEl.className = 'vol-card-name';
    nameEl.title = name;
    nameEl.textContent = name;

    const idEl = document.createElement('details');
    idEl.className = 'vol-card-id';
    const idSummary = document.createElement('summary');
    idSummary.textContent = 'Advanced';
    const idText = document.createElement('span');
    idText.textContent = `Script / routing ID: ${id}`;
    idEl.appendChild(idSummary);
    idEl.appendChild(idText);

    const sliderRow = document.createElement('div');
    sliderRow.className = 'vol-slider-row';

    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = '0';
    slider.max = '200';
    slider.step = '1';
    slider.value = String(Math.max(0, Math.min(200, gainToPercent(gain))));
    slider.setAttribute('aria-label', `${name} outgoing gain`);
    slider.setAttribute('aria-orientation', 'vertical');

    const pctEl = document.createElement('div');
    pctEl.className = 'vol-pct';
    pctEl.textContent = `${gainToPercent(gain)}%`;
    slider.setAttribute('aria-valuetext', pctEl.textContent);

    let saveTimer = null;
    const showGain = value => {
      slider.value = String(gainToPercent(value));
      pctEl.textContent = `${gainToPercent(value)}%`;
      slider.setAttribute('aria-valuetext', pctEl.textContent);
    };
    slider.addEventListener('input', () => {
      pctEl.textContent = `${slider.value}%`;
      slider.setAttribute('aria-valuetext', pctEl.textContent);
      clearTimeout(saveTimer);
      saveTimer = setTimeout(async () => {
        const requested = percentToGain(slider.value);
        if (await setVolumeGain(id, requested) === false && percentToGain(slider.value) === requested) {
          showGain(vol.gain);
        }
      }, 120);
    });

    sliderRow.appendChild(slider);
    sliderRow.appendChild(pctEl);
    const scale = document.createElement('div');
    scale.className = 'vol-fader-scale';
    scale.setAttribute('aria-hidden', 'true');
    for (const tick of ['200', '100', '0']) {
      const mark = document.createElement('span');
      mark.textContent = tick;
      scale.appendChild(mark);
    }
    sliderRow.appendChild(scale);
    const unityBtn = document.createElement('button');
    unityBtn.className = 'flat vol-unity';
    unityBtn.textContent = 'Unity';
    unityBtn.title = 'Reset outgoing gain to 100% (0 dB)';
    unityBtn.setAttribute('aria-label', `Reset ${name} to unity gain`);
    unityBtn.addEventListener('click', async () => {
      clearTimeout(saveTimer);
      showGain(1);
      if (await setVolumeGain(id, 1) === false && slider.value === '100') showGain(vol.gain);
    });

    const ccRow = document.createElement('div');
    ccRow.className = 'vol-cc-row';

    const ccBadge = document.createElement('span');
    ccBadge.className = `vol-cc-badge${cc !== null ? '' : ' unset'}`;
    ccBadge.textContent = cc !== null ? `CC ${cc}` : 'no MIDI CC';

    const assignBtn = document.createElement('button');
    assignBtn.textContent = isTarget ? 'Waiting\u2026' : 'Assign CC';
    if (isTarget) assignBtn.classList.add('primary');
    assignBtn.addEventListener('click', async () => {
      if (isTarget) {
        state.volAssignTarget = null;
        if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
        if (volCcStatusEl) volCcStatusEl.textContent = 'Cancelled.';
        renderVolGrid();
        return;
      }
      try {
        const r = await apiFetch('/api/midi/last_cc', { method: 'GET' });
        if (r.ok) {
          const d = JSON.parse(await r.text());
          state.volCcLastSeq = Number(d.last_seq || 0);
        }
      } catch (_) {}
      state.volAssignTarget = id;
      if (!state.volCcPollTimer) {
        state.volCcPollTimer = setInterval(pollVolCcCapture, 220);
      }
      if (volCcStatusEl) volCcStatusEl.textContent = `Move a MIDI knob or slider to assign to "${name}".`;
      renderVolGrid();
    });

    const clearBtn = document.createElement('button');
    clearBtn.className = 'flat';
    clearBtn.textContent = 'Clear CC';
    clearBtn.disabled = cc === null;
    clearBtn.addEventListener('click', async () => {
      if (cc === null) return;
      await deleteCcVolumeMapping(cc);
    });

    if (cc !== null) ccRow.appendChild(ccBadge);
    ccRow.appendChild(assignBtn);
    if (cc !== null) ccRow.appendChild(clearBtn);
    if (cc === null) ccRow.appendChild(ccBadge);

    card.appendChild(nameEl);
    const meters = document.createElement('div');
    meters.className = 'vol-meters';
    volumeMeters.set(id, meters);
    const controls = document.createElement('div');
    controls.className = 'vol-channel-controls';
    controls.appendChild(sliderRow);
    controls.appendChild(meters);
    card.appendChild(controls);
    card.appendChild(unityBtn);
    card.appendChild(ccRow);
    card.appendChild(idEl);
    volGridEl.appendChild(card);
  }
}

async function setVolumeGain(thingId, gain) {
  try {
    const res = await apiFetch('/api/audio/volume/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `id=${encodeURIComponent(thingId)}&gain=${gain.toFixed(4)}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`volume set failed: ${res.status} ${txt}`, 'warn');
      return false;
    }
    // Update local state
    const v = state.volumes.find((e) => e.id === thingId);
    if (v) v.gain = gain;
    setStatus(`${v?.name || thingId} volume set to ${gainToPercent(gain)}%`, 'ok');
    return true;
  } catch (err) {
    setStatus(`volume set error: ${err}`, 'warn');
    return false;
  }
}

async function pollVolCcCapture() {
  if (!state.volAssignTarget) {
    if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
    return;
  }
  try {
    const res = await apiFetch('/api/midi/last_cc', { method: 'GET' });
    if (!res.ok) return;
    const data = JSON.parse(await res.text());
    const seq = Number(data.last_seq || 0);
    const cc = Number(data.last_cc);

    if (!data.connected) {
      if (volCcStatusEl) volCcStatusEl.textContent = 'Waiting for MIDI device\u2026';
      return;
    }

    if (seq > state.volCcLastSeq && cc >= 0 && cc <= 127) {
      state.volCcLastSeq = seq;
      const target = state.volAssignTarget;
      state.volAssignTarget = null;
      if (state.volCcPollTimer) { clearInterval(state.volCcPollTimer); state.volCcPollTimer = null; }
      await setCcVolumeMapping(cc, target);
      if (volCcStatusEl) volCcStatusEl.textContent = `Assigned CC ${cc} to "${target}".`;
    }
  } catch (_) {}
}

async function setCcVolumeMapping(cc, thingId) {
  try {
    const res = await apiFetch('/api/midi/cc_volume/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `cc=${cc}&id=${encodeURIComponent(thingId)}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`CC volume set failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    // Update local CC map
    state.ccVolumes = state.ccVolumes.filter((e) => e.cc !== cc && e.id !== thingId);
    state.ccVolumes.push({ cc, id: thingId });
    renderVolGrid();
    setStatus(`CC ${cc} \u2192 ${thingId} volume`, 'ok');
  } catch (err) {
    setStatus(`CC volume set error: ${err}`, 'warn');
  }
}

async function deleteCcVolumeMapping(cc) {
  if (!await AudioxUI.confirm(`Remove the volume control mapping for MIDI CC ${cc}?`, {
    title: 'Remove MIDI mapping?', label: 'Remove mapping',
  })) return;
  try {
    const res = await apiFetch('/api/midi/cc_volume/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `cc=${cc}`,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`CC volume delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    state.ccVolumes = state.ccVolumes.filter((e) => e.cc !== cc);
    renderVolGrid();
    setStatus(`CC ${cc} volume mapping removed`, 'ok');
  } catch (err) {
    setStatus(`CC volume delete error: ${err}`, 'warn');
  }
}

