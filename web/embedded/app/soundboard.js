async function triggerSfx(fileName, action = 'trigger') {
  if (!fileName) {
    return;
  }

  const verb = action === 'press' ? 'pressing' : (action === 'release' ? 'releasing' : 'triggering');
  setStatus(`${verb} ${fileName}...`);
  try {
    const target = encodeURIComponent(fileName);
    const endpoint = (action === 'press')
      ? `/api/soundboard/press/${target}`
      : (action === 'release')
        ? `/api/soundboard/release/${target}`
        : `/api/soundboard/trigger/${target}`;
    const res = await apiFetch(endpoint, { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`error: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    if (action === 'release') {
      setStatus(`released ${fileName}`, 'ok');
    } else {
      setStatus(`triggered ${fileName}`, 'ok');
    }
  } catch (err) {
    setStatus(`request failed: ${err}`, 'warn');
  }
}

async function triggerStopAll() {
  setStatus('stopping all clips...');
  try {
    const res = await apiFetch('/api/soundboard/stop_all', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`error: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    setStatus('all clips stopped', 'ok');
  } catch (err) {
    setStatus(`request failed: ${err}`, 'warn');
  }
}

async function triggerSamplerToggle() {
  try {
    const res = await apiFetch('/api/midi/sampler_mode', { method: 'POST' });
    const text = await res.text();
    if (!res.ok) throw new Error(`${res.status} ${text.trim()}`);
    const data = JSON.parse(text);
    setStatus(`Sampler selection mode ${data.active ? 'on' : 'off'}.`, 'ok');
  } catch (error) {
    setStatus(`Sampler toggle failed: ${error}`, 'warn');
  }
}

function renderStopAllMidiStatus() {
  if (!sbStopAllMidiEl) {
    return;
  }
  sbStopAllMidiEl.textContent = (state.stopAllMidiNote === null || state.stopAllMidiNote === undefined)
    ? 'unassigned'
    : `note ${state.stopAllMidiNote}`;
}

function renderSamplerToggleMidiStatus() {
  if (!sbSamplerToggleMidiEl) {
    return;
  }
  sbSamplerToggleMidiEl.textContent = (state.samplerToggleMidiNote === null || state.samplerToggleMidiNote === undefined)
    ? 'unassigned'
    : `note ${state.samplerToggleMidiNote}`;
}

async function loadSoundboardModes() {
  try {
    const res = await apiFetch('/api/soundboard/modes', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      return;
    }
    const data = JSON.parse(txt);
    state.soundboardModesBySfx = {};
    const modes = Array.isArray(data?.modes) ? data.modes : [];
    for (const entry of modes) {
      const sfx = String(entry?.sfx || '');
      const mode = (entry?.mode === 'hold') ? 'hold' : 'play';
      if (sfx) {
        state.soundboardModesBySfx[sfx] = mode;
      }
    }
    renderSoundboardGrid();
  } catch (_) {}
}

function updateSoundboardLayoutState(data) {
  const rows = Number(data?.rows);
  const columns = Number(data?.columns);
  const corners = Array.isArray(data?.corners) && data.corners.length === 4
    ? data.corners.map((note) => Number.isInteger(note) ? note : null)
    : [null, null, null, null];
  state.soundboardLayout = {
    mode: data?.mode === 'grid' ? 'grid' : 'normal',
    rows: Number.isInteger(rows) && rows >= 1 && rows <= 8 ? rows : 4,
    columns: Number.isInteger(columns) && columns >= 1 && columns <= 8 ? columns : 4,
    corners,
    cells: Array.isArray(data?.cells) ? data.cells : [],
  };
  state.soundboardCellsBySfx = AudioxSoundboardGrid.cellsBySound(state.soundboardLayout,
    AudioxSoundboardGrid.elements(state.sfxFiles));
  const modeSelect = document.getElementById('sb-layout-mode');
  const rowsInput = document.getElementById('sb-grid-rows');
  const columnsInput = document.getElementById('sb-grid-columns');
  modeSelect.value = state.soundboardLayout.mode;
  rowsInput.value = String(state.soundboardLayout.rows);
  columnsInput.value = String(state.soundboardLayout.columns);
  document.getElementById('sb-grid-options').hidden = state.soundboardLayout.mode !== 'grid';
  document.querySelectorAll('.sb-grid-only').forEach((el) => { el.hidden = state.soundboardLayout.mode !== 'grid'; });
  const learned = state.soundboardLayout.corners
    .map((note, index) => Number.isInteger(note) ? `${['top-left', 'top-right', 'bottom-left', 'bottom-right'][index]} MIDI ${note}` : null)
    .filter(Boolean);
  document.getElementById('sb-grid-learn-status').textContent = learned.length
    ? `Learned: ${learned.join(', ')}.`
    : 'Learn the four physical pad corners to connect grid cells to MIDI notes.';
  document.getElementById('sb-order-status').textContent = state.soundboardLayout.mode === 'grid'
    ? 'Drag sounds to empty cells, onto other sounds to swap, or to Unassigned sounds. On touch, drag the sound name. The cell selector also supports keyboard placement. Learn MIDI corners to update controller pad mappings.'
    : 'Drag sound cards to reorder them (on touch, drag the sound name). With a keyboard, focus a name and press Space, use arrow keys, then Enter to save. Changes also update the device display.';
  renderSoundboardCellSelectors();
  renderSoundboardGrid();
}

async function loadSoundboardLayout() {
  try {
    const res = await apiFetch('/api/soundboard/layout', { method: 'GET' });
    const txt = await res.text();
    if (!res.ok) {
      if (res.status === 404) {
        updateSoundboardLayoutState({ mode: 'normal', rows: 4, columns: 4 });
        return;
      }
      throw new Error(`Could not load soundboard layout: ${res.status} ${txt}`);
    }
    updateSoundboardLayoutState(JSON.parse(txt));
  } catch (error) {
    setStatus(error, 'warn', 'soundboard-layout');
  }
}

function renderSoundboardCellSelectors() {
  const layout = state.soundboardLayout;
  const uploadSelect = document.getElementById('sb-upload-cell');
  uploadSelect.replaceChildren(new Option('Unassigned', ''));
  if (layout.mode !== 'grid') return;
  const occupied = new Set(Object.values(state.soundboardCellsBySfx)
    .map((cell) => `${cell.row}:${cell.column}`));
  for (let row = 0; row < layout.rows; row += 1) {
    for (let column = 0; column < layout.columns; column += 1) {
      const option = new Option(`Row ${row + 1}, column ${column + 1}`, `${row}:${column}`);
      if (occupied.has(option.value)) {
        option.disabled = true;
        option.textContent += ' (occupied)';
      }
      uploadSelect.appendChild(option);
    }
  }
}

async function saveSoundboardLayout(fields, statusLabel = 'Soundboard layout saved.') {
  try {
    const res = await apiFetch('/api/soundboard/layout', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: new URLSearchParams(fields).toString(),
    });
    const txt = await res.text();
    if (!res.ok) throw new Error(`${res.status} ${txt.trim()}`);
    updateSoundboardLayoutState(JSON.parse(txt));
    setStatus(statusLabel, 'ok', 'soundboard-layout');
    return true;
  } catch (error) {
    setStatus(`Soundboard layout save failed: ${error}`, 'warn', 'soundboard-layout');
    await loadSoundboardLayout();
    return false;
  }
}

async function saveSoundboardGridCell(sfx, row, column) {
  if (state.soundboardGridSaving) return false;
  const current = state.soundboardCellsBySfx[sfx];
  if (current?.row === row && current?.column === column) return true;
  state.soundboardGridSaving = true;
  soundboardGridDrag.cancel();
  try {
    const action = AudioxSoundboardGrid.actionFor(sfx);
    const element = action ? { action: action.action } : { sfx };
    const label = action?.label || sfx;
    const fields = row === null ? { ...element, unassign: '1' } :
      { ...element, row: String(row), column: String(column) };
    const saved = await saveSoundboardLayout(fields, row === null ? `Unassigned ${label}.` :
      `Moved ${label} to row ${row + 1}, column ${column + 1}.`);
    if (saved) {
      await loadMappings();
      await loadMidiActions();
    }
    return saved;
  } finally {
    state.soundboardGridSaving = false;
    renderSoundboardGrid();
  }
}

async function beginSoundboardGridLearn(corner) {
  const target = `__grid_${corner}__`;
  if (state.mappingAssignTarget === target) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    document.getElementById('sb-grid-learn-status').textContent = 'Grid MIDI learn cancelled.';
    return;
  }
  try {
    const response = await apiFetch('/api/midi/last_note', { method: 'GET' });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${text.trim()}`);
    const data = JSON.parse(text);
    const sequence = Number(data.last_seq);
    if (!Number.isInteger(sequence) || sequence < 0) throw new Error('device did not report a valid MIDI event sequence');
    state.mappingAssignTarget = target;
    state.mappingLastSeq = sequence;
    if (!state.mappingPollTimer) state.mappingPollTimer = setInterval(pollMappingCapture, 220);
    document.getElementById('sb-grid-learn-status').textContent =
      `Press the ${corner.replace('_', ' ')} pad corner. Click its Learn button again to cancel.`;
  } catch (error) {
    setStatus(`Grid MIDI learn failed: ${error}`, 'warn', 'soundboard-layout');
  }
}

async function saveSoundboardGridCorner(corner, note) {
  const saved = await saveSoundboardLayout({ corner, note: String(note) },
    `Learned ${corner.replace('_', ' ')} as MIDI note ${note}.`);
  document.getElementById('sb-grid-learn-status').textContent = saved
    ? `Learned ${corner.replace('_', ' ')} as MIDI ${note}. Assigning a sound to a calibrated cell maps its pad automatically.`
    : 'Grid MIDI learn failed. Check that the four corner notes form a unique, evenly numbered grid.';
}

async function saveSoundboardModeForSfx(sfx, mode) {
  const normalizedMode = (mode === 'hold') ? 'hold' : 'play';
  setStatus(`saving mode for ${sfx}...`);
  try {
    const res = await apiFetch('/api/soundboard/mode/set', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: `sfx=${encodeURIComponent(sfx)}&mode=${normalizedMode}`,
    });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`mode save failed: ${res.status} ${txt.trim()}`, 'warn');
      return;
    }
    if (normalizedMode === 'hold') {
      state.soundboardModesBySfx[sfx] = 'hold';
    } else {
      delete state.soundboardModesBySfx[sfx];
    }
    renderSoundboardGrid();
    setStatus(`mode for ${sfx} set to ${normalizedMode}`, 'ok');
  } catch (err) {
    setStatus(`mode save error: ${err}`, 'warn');
  }
}

function sanitizeSfxName(name) {
  const raw = String(name || '').trim();
  if (!raw) return '';
  const base = raw.split('/').pop().split('\\').pop();
  let clean = base.replace(/[^a-zA-Z0-9._-]/g, '_');
  if (!clean) {
    return '';
  }
  if (!/\.wav$/i.test(clean)) {
    clean = `${clean}.wav`;
  }
  return clean;
}

async function listSfxFiles() {
  try {
    let res = await apiFetch('/api/soundboard/order', { method: 'GET' });
    state.soundboardOrderSupported = res.status !== 404;
    if (!state.soundboardOrderSupported) {
      setStatus('Shared sound ordering is unavailable on this firmware. The file list remains playable.', 'warn', 'soundboard-order-support');
      res = await apiFetch('/api/rootfs/sfx/', { method: 'GET' });
    } else {
      AudioxUI.resolve('soundboard-order-support');
    }
    const txt = await res.text();
    if (!res.ok) throw new Error(`Could not load sounds: ${res.status} ${txt}`);

    const files = [];
    for (const line of txt.split(/\r?\n/)) {
      const t = line.trim();
      if (!t || t.startsWith('rootfs listing:')) continue;
      if (t.endsWith('/')) continue;
      files.push(t);
    }

    if (!state.soundboardOrderSupported) files.sort();
    state.sfxFiles = files;
    state.soundboardCellsBySfx = AudioxSoundboardGrid.cellsBySound(state.soundboardLayout,
      AudioxSoundboardGrid.elements(files));
    renderSoundboardGrid();
    AudioxUI.resolve('soundboard-list');
    return files;
  } catch (err) {
    setStatus(err, 'warn', 'soundboard-list');
    return [];
  }
}

async function saveSoundboardOrder(files, file) {
  if (state.soundboardOrderSaving) return;
  if (!state.soundboardOrderSupported || files.length !== state.sfxFiles.length ||
      new Set(files).size !== files.length || files.some((name) => !state.sfxFiles.includes(name))) {
    setStatus('Refresh the soundboard before changing this sound position.', 'warn', 'soundboard-order');
    return;
  }
  state.soundboardOrderSaving = true;
  renderSoundboardGrid();
  try {
    await AudioxUI.runTask('Saving sound order', async () => {
      const res = await apiFetch('/api/soundboard/order', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: `${files.join('\n')}\n`,
        signal: AbortSignal.timeout(10000),
      });
      const txt = await res.text();
      if (!res.ok) throw new Error(`Could not save sound order: ${res.status} ${txt}`);
      const saved = txt.trim().split(/\r?\n/);
      if (saved.length !== files.length || saved.some((name, i) => name !== files[i])) {
        throw new Error('The device returned an unexpected sound order. Refresh the soundboard to check it.');
      }
      state.sfxFiles = saved;
      setStatus('Sound order saved for the website and device display.', 'ok', 'soundboard-order');
    });
  } catch (error) {
    setStatus(error, 'warn', 'soundboard-order');
  } finally {
    state.soundboardOrderSaving = false;
    renderSoundboardGrid();
    const card = Array.from(sbGridEl.children).find((element) => element.dataset.file === file);
    card?.querySelector('.sb-card-name')?.focus({ preventScroll: true });
  }
}

async function reloadSoundboardBank() {
  try {
    const res = await apiFetch('/api/soundboard/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`soundboard reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    try {
      const data = JSON.parse(txt);
      const loaded = Number(data?.loaded || 0);
      const max = Number(data?.max || 0);
      setStatus(`soundboard cache loaded ${loaded}/${max}`, 'ok');
    } catch (_) {
      setStatus('soundboard cache reloaded', 'ok');
    }
    return true;
  } catch (err) {
    setStatus(`soundboard reload error: ${err}`, 'warn');
    return false;
  }
}

function groupSoundboardTileSettings(actions, midi, file, label, isTarget) {
  const settings = document.createElement('details');
  settings.className = 'sb-tile-settings sb-edit-only';
  settings.open = state.soundboardOpenSettings.has(file) || isTarget;
  const summary = document.createElement('summary');
  summary.textContent = isTarget ? 'Waiting for MIDI\u2026' : 'Settings';
  summary.setAttribute('aria-label', `Settings for ${label}`);
  const controls = document.createElement('div');
  controls.className = 'sb-tile-controls';
  for (const control of [...actions.children]) {
    if (!control.classList.contains('sb-edit-only')) continue;
    if (control.tagName === 'SELECT') {
      const field = document.createElement('label');
      field.textContent = control.classList.contains('sb-grid-cell-select') ? 'Grid position' : 'Playback';
      field.appendChild(control);
      controls.appendChild(field);
    } else {
      controls.appendChild(control);
    }
  }
  const clear = midi.querySelector('.sb-midi-clear');
  if (clear) {
    clear.className = 'flat';
    clear.textContent = 'Clear MIDI mapping';
    controls.appendChild(clear);
  }
  settings.append(summary, controls);
  settings.addEventListener('toggle', () => {
    if (settings.open) state.soundboardOpenSettings.add(file);
    else state.soundboardOpenSettings.delete(file);
  });
  actions.appendChild(settings);
}

function renderSoundboardGrid() {
  soundboardOrder.cancel();
  soundboardGridDrag.cancel();
  const gridMode = state.soundboardLayout.mode === 'grid';
  const rows = state.soundboardLayout.rows;
  const columns = state.soundboardLayout.columns;
  state.soundboardCellsBySfx = AudioxSoundboardGrid.cellsBySound(state.soundboardLayout,
    AudioxSoundboardGrid.elements(state.sfxFiles));
  sbGridEl.classList.toggle('grid-mode', gridMode);
  sbGridEl.style.setProperty('--sb-columns', String(columns));
  sbGridEl.classList.toggle('can-reorder', !gridMode && state.soundboardEditing && state.soundboardOrderSupported &&
    !state.soundboardOrderSaving);
  sbGridEl.innerHTML = '';
  const unassignedList = document.getElementById('sb-unassigned-list');
  unassignedList.replaceChildren();
  document.getElementById('sb-unassigned').hidden = !gridMode;
  if (!state.sfxFiles.length) {
    const empty = document.createElement('div');
    empty.className = 'small';
    empty.textContent = 'No sounds uploaded yet.';
    if (!gridMode) {
      sbGridEl.appendChild(empty);
      return;
    }
    empty.style.gridColumn = `1 / ${columns + 1}`;
    empty.style.gridRow = String(rows + 1);
    sbGridEl.appendChild(empty);
  }

  const occupiedCells = new Set();
  for (const file of gridMode ? AudioxSoundboardGrid.elements(state.sfxFiles) : state.sfxFiles) {
    const action = AudioxSoundboardGrid.actionFor(file);
    const label = action?.label || file;
    const mappedNote = action ? (action.action === 'stop_all' ? state.stopAllMidiNote : state.samplerToggleMidiNote)
      : state.midiMappingsBySfx[file];
    const hasMidi = mappedNote !== undefined && mappedNote !== null;
    const isTarget = state.mappingAssignTarget === file;
    const mode = (state.soundboardModesBySfx[file] === 'hold') ? 'hold' : 'play';

    const card = document.createElement('div');
    card.className = `sb-card${isTarget ? ' assign-target' : ''}`;
    card.dataset.file = file;
    if (gridMode) {
      const cell = state.soundboardCellsBySfx[file];
      if (cell) {
        card.dataset.gridRow = String(cell.row);
        card.dataset.gridColumn = String(cell.column);
        card.style.gridColumn = String(cell.column + 1);
        card.style.gridRow = String(cell.row + 1);
        occupiedCells.add(`${cell.row}:${cell.column}`);
      }
    }

    const nameEl = document.createElement('div');
    nameEl.className = 'sb-card-name';
    nameEl.title = label;
    nameEl.textContent = label;
    nameEl.tabIndex = state.soundboardEditing && !gridMode && state.soundboardOrderSupported ? 0 : -1;
    if (state.soundboardEditing && !gridMode && state.soundboardOrderSupported) {
      nameEl.setAttribute('role', 'button');
      nameEl.setAttribute('aria-label', `Reorder ${file}`);
      nameEl.setAttribute('aria-pressed', 'false');
    }
    nameEl.setAttribute('aria-describedby', 'sb-order-status');

    const midiEl = document.createElement('div');
    midiEl.className = `sb-card-midi${hasMidi ? ' mapped' : ''}`;
    if (hasMidi) {
      const noteSpan = document.createElement('span');
      noteSpan.textContent = `MIDI ${mappedNote}`;
      const clearBtn = document.createElement('button');
      clearBtn.className = 'sb-midi-clear sb-edit-only';
      clearBtn.textContent = '\u00d7';
      clearBtn.title = 'Remove MIDI mapping';
      clearBtn.addEventListener('click', (ev) => {
        ev.stopPropagation();
        if (action) {
          const remove = action.action === 'stop_all' ? deleteStopAllActionMapping : deleteSamplerToggleActionMapping;
          remove().then(renderSoundboardGrid);
        } else deleteMapping(mappedNote);
      });
      midiEl.appendChild(noteSpan);
      midiEl.appendChild(clearBtn);
    } else {
      midiEl.textContent = 'no MIDI';
    }

    const actionsEl = document.createElement('div');
    actionsEl.className = 'sb-card-actions';

    if (gridMode) {
      const cellSelect = document.createElement('select');
      cellSelect.className = 'sb-edit-only sb-grid-cell-select';
      cellSelect.setAttribute('aria-label', `Grid cell for ${label}`);
      cellSelect.appendChild(new Option('Unassigned', ''));
      cellSelect.disabled = state.soundboardGridSaving;
      for (let row = 0; row < rows; row += 1) {
        for (let column = 0; column < columns; column += 1) {
          const option = new Option(`Row ${row + 1}, column ${column + 1}`, `${row}:${column}`);
          const occupiedByOther = Object.entries(state.soundboardCellsBySfx)
            .some(([otherSfx, otherCell]) => otherSfx !== file &&
              otherCell.row === row && otherCell.column === column);
          if (occupiedByOther) {
            option.textContent += ' (swap)';
          }
          cellSelect.appendChild(option);
        }
      }
      const cell = state.soundboardCellsBySfx[file];
      if (cell) cellSelect.value = `${cell.row}:${cell.column}`;
      cellSelect.addEventListener('change', async () => {
        if (!cellSelect.value) {
          await saveSoundboardGridCell(file, null);
          return;
        }
        const [row, column] = cellSelect.value.split(':').map(Number);
        await saveSoundboardGridCell(file, row, column);
      });
      actionsEl.appendChild(cellSelect);
    }

    if (action) {
      const trigger = document.createElement('button');
      trigger.className = `sb-play ${action.action === 'stop_all' ? 'warn' : 'primary'}`;
      trigger.textContent = label;
      trigger.addEventListener('click', action.action === 'stop_all' ? triggerStopAll : triggerSamplerToggle);
      actionsEl.appendChild(trigger);
      groupSoundboardTileSettings(actionsEl, midiEl, file, label, isTarget);
      card.classList.add('sb-action-card');
      card.append(nameEl, midiEl, actionsEl);
      (state.soundboardCellsBySfx[file] ? sbGridEl : unassignedList).appendChild(card);
      continue;
    }

    const modeSelect = document.createElement('select');
    modeSelect.className = 'sb-edit-only';
    modeSelect.title = 'Playback mode for this sound';
    const optPlay = document.createElement('option');
    optPlay.value = 'play';
    optPlay.textContent = 'Play';
    const optHold = document.createElement('option');
    optHold.value = 'hold';
    optHold.textContent = 'Hold';
    modeSelect.appendChild(optPlay);
    modeSelect.appendChild(optHold);
    modeSelect.value = mode;
    modeSelect.addEventListener('change', () => {
      saveSoundboardModeForSfx(file, modeSelect.value);
    });

    const playBtn = document.createElement('button');
    playBtn.className = 'primary sb-play';
    playBtn.setAttribute('aria-label', `${mode === 'hold' ? 'Hold' : 'Play'} ${file}`);
    playBtn.textContent = (mode === 'hold') ? 'Hold' : '\u25b6 Play';

    let holdDown = false;
    const releaseHold = () => {
      if (!holdDown) {
        return;
      }
      holdDown = false;
      triggerSfx(file, 'release');
    };

    playBtn.addEventListener('pointerdown', (ev) => {
      if (mode !== 'hold') {
        return;
      }
      ev.preventDefault();
      holdDown = true;
      triggerSfx(file, 'press');
    });
    playBtn.addEventListener('pointerup', releaseHold);
    playBtn.addEventListener('pointercancel', releaseHold);
    playBtn.addEventListener('pointerleave', releaseHold);
    playBtn.addEventListener('keydown', (event) => {
      if (mode !== 'hold' || ![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      if (!holdDown) {
        holdDown = true;
        triggerSfx(file, 'press');
      }
    });
    playBtn.addEventListener('keyup', (event) => {
      if (mode !== 'hold' || ![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      releaseHold();
    });
    playBtn.addEventListener('blur', releaseHold);
    playBtn.addEventListener('click', (ev) => {
      if (mode === 'hold') {
        ev.preventDefault();
        return;
      }
      triggerSfx(file, 'trigger');
    });

    const assignBtn = document.createElement('button');
    assignBtn.className = 'sb-edit-only';
    assignBtn.textContent = isTarget ? 'Waiting\u2026' : 'Assign MIDI';
    if (isTarget) assignBtn.classList.add('primary');
    assignBtn.addEventListener('click', async () => {
      if (isTarget) {
        state.mappingAssignTarget = null;
        if (state.mappingPollTimer) {
          clearInterval(state.mappingPollTimer);
          state.mappingPollTimer = null;
        }
        sbMappingStatusEl.textContent = 'Assignment cancelled.';
      } else {
        // Snapshot the current seq so only presses AFTER this click are captured.
        try {
          const r = await apiFetch('/api/midi/last_note', { method: 'GET' });
          if (r.ok) {
            const d = JSON.parse(await r.text());
            state.mappingLastSeq = Number(d.last_seq || 0);
          }
        } catch (_) {}
        state.mappingAssignTarget = file;
        if (!state.mappingPollTimer) {
          state.mappingPollTimer = setInterval(pollMappingCapture, 220);
        }
        sbMappingStatusEl.textContent = `Press a MIDI button to assign to "${file}".`;
      }
      renderSoundboardGrid();
    });

    actionsEl.appendChild(modeSelect);
    actionsEl.appendChild(playBtn);
    actionsEl.appendChild(assignBtn);

    const delBtn = document.createElement('button');
    delBtn.className = 'warn sb-edit-only';
    delBtn.textContent = 'Remove';
    delBtn.addEventListener('click', () => deleteSfxFile(file));
    actionsEl.appendChild(delBtn);

    const replaceInput = document.createElement('input');
    replaceInput.type = 'file';
    replaceInput.accept = 'audio/*';
    replaceInput.style.display = 'none';
    replaceInput.addEventListener('change', async () => {
      const selected = replaceInput.files && replaceInput.files[0] ? replaceInput.files[0] : null;
      if (!selected) {
        return;
      }
      await runLoadingTask('Replacing sound', () => replaceSfxFile(file, selected));
      replaceInput.value = '';
    });

    const replaceBtn = document.createElement('button');
    replaceBtn.className = 'sb-edit-only';
    replaceBtn.textContent = 'Replace';
    replaceBtn.addEventListener('click', () => {
      replaceInput.click();
    });
    actionsEl.appendChild(replaceBtn);
    actionsEl.appendChild(replaceInput);
    groupSoundboardTileSettings(actionsEl, midiEl, file, label, isTarget);

    card.appendChild(nameEl);
    card.appendChild(midiEl);
    card.appendChild(actionsEl);
    (gridMode && !state.soundboardCellsBySfx[file] ? unassignedList : sbGridEl).appendChild(card);
  }

  if (gridMode) {
    for (let row = 0; row < rows; row += 1) {
      for (let column = 0; column < columns; column += 1) {
        if (occupiedCells.has(`${row}:${column}`)) continue;
        const placeholder = document.createElement('div');
        placeholder.className = 'sb-grid-empty';
        placeholder.dataset.gridRow = String(row);
        placeholder.dataset.gridColumn = String(column);
        placeholder.textContent = `Empty · ${row + 1}, ${column + 1}`;
        placeholder.setAttribute('aria-label', `Empty grid cell, row ${row + 1}, column ${column + 1}`);
        placeholder.style.gridColumn = String(column + 1);
        placeholder.style.gridRow = String(row + 1);
        sbGridEl.appendChild(placeholder);
      }
    }
    if (!unassignedList.children.length) {
      const empty = document.createElement('p');
      empty.className = 'small';
      empty.textContent = 'No unassigned sounds or actions. Drop a tile here to unassign it.';
      unassignedList.appendChild(empty);
    }
  }
}

async function deleteSfxFile(filename) {
  if (!await AudioxUI.confirm(`Delete "${filename}" from the soundboard? This cannot be undone.`, {
    title: 'Delete sound?', label: 'Delete sound',
  })) return;
  setStatus(`deleting ${filename}...`);
  try {
    const res = await apiFetch(`/api/rootfs/sfx/${encodeURIComponent(filename)}`, {
      method: 'DELETE',
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`delete failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    await listSfxFiles();
    await reloadSoundboardBank();
    await loadMappings();
    setStatus(`deleted ${filename}`, 'ok');
  } catch (err) {
    setStatus(`delete error: ${err}`, 'warn');
  }
}

async function replaceSfxFile(filename, fileObj) {
  if (!filename || !fileObj) {
    return;
  }

  setStatus(`replacing ${filename}...`);
  try {
    const buf = await fileObj.arrayBuffer();
    const res = await apiFetch(`/api/rootfs/sfx/${encodeURIComponent(filename)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: buf,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`replace failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await reloadSoundboardBank();
    await listSfxFiles();
    setStatus(`replaced ${filename}`, 'ok');
  } catch (err) {
    setStatus(`replace error: ${err}`, 'warn');
  }
}

async function uploadSfxFile() {
  const file = document.getElementById('upload_file').files[0];
  const manualName = document.getElementById('upload_name').value;
  const selectedCell = document.getElementById('sb-upload-cell').value;
  if (!file) {
    setStatus('pick a file first', 'warn');
    return;
  }

  const targetName = sanitizeSfxName(manualName || file.name);
  if (!targetName) {
    setStatus('invalid target filename', 'warn');
    return;
  }

  setStatus(`uploading ${targetName} (streaming)...`);
  try {
    const buf = await file.arrayBuffer();
    const res = await apiFetch(`/api/rootfs/sfx/${targetName}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: buf,
    });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`upload failed: ${res.status} ${txt}`, 'warn');
      return;
    }

    await listSfxFiles();
    await loadSoundboardLayout();
    let cellSaved = true;
    if (selectedCell && state.soundboardLayout.mode === 'grid') {
      const [row, column] = selectedCell.split(':').map(Number);
      cellSaved = await saveSoundboardGridCell(targetName, row, column);
    }
    await reloadSoundboardBank();
    AudioxUI.setUnsaved('sound-upload', false);
    setStatus(cellSaved ? `uploaded ${targetName}` :
      `uploaded ${targetName}, but the selected grid cell could not be assigned. Choose a free cell.`,
    cellSaved ? 'ok' : 'warn');
  } catch (err) {
    setStatus(`upload error: ${err}`, 'warn');
  }
}

function setSoundboardEditing(editing) {
  soundboardOrder.cancel();
  soundboardGridDrag.cancel();
  state.soundboardEditing = editing;
  document.getElementById('soundboard-layout').classList.toggle('play-mode', !editing);
  sbGridEl.querySelectorAll('.sb-card-name').forEach((name) => {
    name.tabIndex = editing && state.soundboardLayout.mode !== 'grid' && state.soundboardOrderSupported ? 0 : -1;
    if (editing && state.soundboardLayout.mode !== 'grid' && state.soundboardOrderSupported) {
      name.setAttribute('role', 'button');
      name.setAttribute('aria-label', `Reorder ${name.textContent}`);
      name.setAttribute('aria-pressed', 'false');
    } else {
      name.removeAttribute('role');
      name.removeAttribute('aria-label');
      name.removeAttribute('aria-pressed');
    }
  });
  sbGridEl.classList.toggle('can-reorder', editing && state.soundboardLayout.mode !== 'grid' &&
    state.soundboardOrderSupported);
  for (const [id, active] of [['btn-sb-play-mode', !editing], ['btn-sb-edit-mode', editing]]) {
    const button = document.getElementById(id);
    button.setAttribute('aria-pressed', String(active));
    button.classList.toggle('primary', active);
    button.classList.toggle('flat', !active);
  }
  if (!editing && state.sfxFiles.includes(state.mappingAssignTarget)) {
    state.mappingAssignTarget = null;
    clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    renderSoundboardGrid();
  }
  if (!editing && String(state.mappingAssignTarget || '').startsWith('__grid_')) {
    state.mappingAssignTarget = null;
    if (state.mappingPollTimer) clearInterval(state.mappingPollTimer);
    state.mappingPollTimer = null;
    document.getElementById('sb-grid-learn-status').textContent = 'Grid MIDI learn cancelled.';
  }
  sbMappingStatusEl.textContent = editing
    ? (state.soundboardLayout.mode === 'grid'
      ? 'Manage sounds, grid positions, playback modes, and MIDI assignments.'
      : 'Manage sounds, playback modes, and MIDI assignments.')
    : 'Tap a sound to play. Switch to Edit to manage sounds and MIDI.';
}
