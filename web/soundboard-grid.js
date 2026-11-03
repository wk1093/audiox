(function (root) {
  const hasOwn = (object, key) => Object.prototype.hasOwnProperty.call(object, key);
  const actions = [
    { id: '__stop_all__', action: 'stop_all', label: 'Stop All' },
    { id: '__sampler_toggle__', action: 'sampler_toggle', label: 'Sampler Toggle' },
  ];
  const elements = (files) => [...files, ...actions.map((action) => action.id)];
  const actionFor = (id) => actions.find((action) => action.id === id);

  function cellsBySound(layout, files) {
    const rows = Number(layout?.rows);
    const columns = Number(layout?.columns);
    const cellMap = Object.create(null);
    const occupied = new Set();
    const knownFiles = new Set(files);
    const saved = Array.isArray(layout?.cells) ? layout.cells : [];

    for (const cell of saved) {
      const sfx = String(cell?.sfx || '');
      const row = Number(cell?.row);
      const column = Number(cell?.column);
      const key = `${row}:${column}`;
      if (!knownFiles.has(sfx) || !Number.isInteger(row) || !Number.isInteger(column) ||
          row < 0 || row >= rows || column < 0 || column >= columns ||
          hasOwn(cellMap, sfx) || occupied.has(key)) continue;
      cellMap[sfx] = { row, column };
      occupied.add(key);
    }

    return cellMap;
  }

  function attach(container, { enabled, save, status }) {
    let drag = null;
    let frame = 0;
    const announce = (text) => { status.textContent = text; };
    const targetAt = (x, y) => {
      const target = root.document.elementFromPoint(x, y)?.closest('[data-grid-row], [data-grid-unassigned]');
      return target && container.contains(target) ? target : null;
    };
    const highlight = (target) => {
      drag.target?.classList.remove('grid-drop-target');
      drag.target = target;
      target?.classList.add('grid-drop-target');
    };
    const hit = () => { if (drag?.started) highlight(targetAt(drag.x, drag.y)); };
    const autoScroll = () => {
      if (!drag?.started) return;
      const distance = drag.y < 70 ? -12 : drag.y > root.innerHeight - 70 ? 12 : 0;
      if (distance) { root.scrollBy(0, distance); hit(); }
      frame = root.requestAnimationFrame(autoScroll);
    };
    const finish = (commit) => {
      if (!drag) return;
      const current = drag;
      drag = null;
      root.cancelAnimationFrame(frame);
      current.card.classList.remove('dragging');
      current.target?.classList.remove('grid-drop-target');
      if (container.hasPointerCapture(current.pointerId)) container.releasePointerCapture(current.pointerId);
      if (!commit || !current.started || !current.target || !enabled()) {
        if (current.started) announce('Grid move cancelled.');
        return;
      }
      const sfx = current.card.dataset.file;
      if (current.target.dataset.gridUnassigned !== undefined) {
        save(sfx, null);
      } else {
        save(sfx, Number(current.target.dataset.gridRow), Number(current.target.dataset.gridColumn));
      }
    };
    container.addEventListener('pointerdown', (event) => {
      if (!enabled() || drag || event.button !== 0 ||
          event.target.closest('button, input, select, a, summary, .sb-tile-settings')) return;
      const card = event.target.closest('.sb-card');
      if (!card || !container.contains(card)) return;
      if (event.pointerType === 'touch' && !event.target.closest('.sb-card-name')) return;
      drag = { card, pointerId: event.pointerId, startX: event.clientX, startY: event.clientY,
        x: event.clientX, y: event.clientY, started: false, target: null };
      container.setPointerCapture(event.pointerId);
    });
    container.addEventListener('pointermove', (event) => {
      if (!drag || drag.pointerId !== event.pointerId) return;
      drag.x = event.clientX;
      drag.y = event.clientY;
      if (!drag.started && Math.hypot(drag.x - drag.startX, drag.y - drag.startY) >= 6) {
        drag.started = true;
        drag.card.classList.add('dragging');
        frame = root.requestAnimationFrame(autoScroll);
        announce('Drop onto a cell to move or swap, or onto Unassigned sounds and actions to remove the pad mapping.');
      }
      if (drag.started) { event.preventDefault(); hit(); }
    });
    container.addEventListener('pointerup', (event) => {
      if (drag?.pointerId !== event.pointerId) return;
      drag.x = event.clientX;
      drag.y = event.clientY;
      hit();
      finish(true);
    });
    container.addEventListener('pointercancel', () => finish(false));
    container.addEventListener('lostpointercapture', () => finish(false));
    root.addEventListener('blur', () => finish(false));
    root.addEventListener('keydown', (event) => {
      if (drag && ['Escape', 'Tab'].includes(event.key)) {
        if (event.key === 'Escape') event.preventDefault();
        finish(false);
      }
    }, true);
    return { cancel: () => finish(false) };
  }

  function midiNoteAt(layout, row, column) {
    const rows = Number(layout?.rows);
    const columns = Number(layout?.columns);
    const corners = layout?.corners;
    if (!Array.isArray(corners) || corners.length !== 4 ||
        !Number.isInteger(row) || !Number.isInteger(column) ||
        rows < 2 || columns < 2 || row < 0 || row >= rows ||
        column < 0 || column >= columns ||
        corners.some((note) => !Number.isInteger(note) || note < 0 || note > 127)) return null;

    const [topLeft, topRight, bottomLeft, bottomRight] = corners;
    const horizontalDelta = topRight - topLeft;
    const verticalDelta = bottomLeft - topLeft;
    if (horizontalDelta % (columns - 1) !== 0 ||
        verticalDelta % (rows - 1) !== 0 ||
        topLeft + horizontalDelta + verticalDelta !== bottomRight) return null;
    const note = topLeft + horizontalDelta / (columns - 1) * column +
      verticalDelta / (rows - 1) * row;
    return Number.isInteger(note) && note >= 0 && note <= 127 ? note : null;
  }

  root.AudioxSoundboardGrid = { cellsBySound, midiNoteAt, attach, elements, actionFor };
})(typeof globalThis !== 'undefined' ? globalThis : window);
