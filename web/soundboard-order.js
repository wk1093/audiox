(function (root) {
  'use strict';

  function attach(grid, { enabled, save, status }) {
    let drag = null;
    let frame = 0;
    const cards = () => Array.from(grid.children).filter((card) => card.dataset.file);
    const files = () => cards().map((card) => card.dataset.file);
    const announce = (text) => { status.textContent = text; };
    const move = (target) => {
      if (!drag || target === drag.card || !target?.dataset.file) return;
      const list = cards();
      grid.insertBefore(drag.card, list.indexOf(target) > list.indexOf(drag.card) ? target.nextSibling : target);
      announce(`${drag.card.dataset.file}, position ${cards().indexOf(drag.card) + 1} of ${list.length}.`);
    };
    const finish = (commit) => {
      if (!drag) return;
      const current = drag;
      drag = null;
      cancelAnimationFrame(frame);
      current.card.classList.remove('dragging');
      current.card.querySelector('.sb-card-name').setAttribute('aria-pressed', 'false');
      if (current.pointerId !== undefined && grid.hasPointerCapture(current.pointerId)) {
        grid.releasePointerCapture(current.pointerId);
      }
      const next = files();
      for (const file of current.original) {
        const card = cards().find((item) => item.dataset.file === file);
        grid.append(card);
      }
      if (commit && next.some((file, i) => file !== current.original[i])) {
        save(next, current.card.dataset.file);
      } else {
        announce(commit ? 'Sound order unchanged.' : 'Reordering cancelled.');
        current.card.querySelector('.sb-card-name').focus({ preventScroll: true });
      }
    };
    const begin = (card, extras = {}) => {
      drag = { card, original: files(), started: false, ...extras };
      card.querySelector('.sb-card-name').setAttribute('aria-pressed', 'true');
    };
    const hit = () => {
      if (!drag?.started || drag.keyboard) return;
      const target = document.elementFromPoint(drag.x, drag.y)?.closest('.sb-card');
      if (target?.parentElement === grid) move(target);
    };
    const autoScroll = () => {
      if (!drag?.started || drag.keyboard) return;
      const distance = drag.y < 70 ? -12 : drag.y > innerHeight - 70 ? 12 : 0;
      if (distance) { root.scrollBy(0, distance); hit(); }
      frame = requestAnimationFrame(autoScroll);
    };
    grid.addEventListener('pointerdown', (event) => {
      if (!enabled() || drag || event.button !== 0 ||
          event.target.closest('button, input, select, a')) return;
      const card = event.target.closest('.sb-card');
      if (!card || card.parentElement !== grid) return;
      if (event.pointerType === 'touch' && !event.target.closest('.sb-card-name')) return;
      begin(card, { pointerId: event.pointerId, startX: event.clientX, startY: event.clientY,
        x: event.clientX, y: event.clientY });
      grid.setPointerCapture(event.pointerId);
    });
    grid.addEventListener('pointermove', (event) => {
      if (!drag || drag.pointerId !== event.pointerId) return;
      drag.x = event.clientX;
      drag.y = event.clientY;
      if (!drag.started && Math.hypot(drag.x - drag.startX, drag.y - drag.startY) >= 6) {
        drag.started = true;
        drag.card.classList.add('dragging');
        frame = requestAnimationFrame(autoScroll);
      }
      if (drag.started) { event.preventDefault(); hit(); }
    });
    grid.addEventListener('pointerup', (event) => {
      if (drag?.pointerId === event.pointerId) finish(true);
    });
    grid.addEventListener('pointercancel', () => finish(false));
    grid.addEventListener('lostpointercapture', () => { if (drag && !drag.keyboard) finish(false); });
    grid.addEventListener('keydown', (event) => {
      const name = event.target.closest('.sb-card-name');
      if (!name || !enabled()) return;
      if (event.key === 'Escape' && drag) {
        event.preventDefault();
        event.stopPropagation();
        finish(false);
      } else if ([' ', 'Enter'].includes(event.key)) {
        event.preventDefault();
        event.stopPropagation();
        if (drag) finish(true);
        else {
          begin(name.closest('.sb-card'), { keyboard: true, started: true });
          drag.card.classList.add('dragging');
          announce('Use arrow keys to move this sound, Enter to save, or Escape to cancel.');
        }
      } else if (drag?.keyboard && ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key)) {
        event.preventDefault();
        event.stopPropagation();
        const list = cards();
        const step = event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? -1 : 1;
        move(list[list.indexOf(drag.card) + step]);
        name.focus({ preventScroll: true });
      }
    }, true);
    root.addEventListener('blur', () => finish(false));
    root.addEventListener('keydown', (event) => {
      if (drag && ['Escape', 'Tab'].includes(event.key)) {
        if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); }
        finish(false);
      }
    }, true);
    return { cancel: () => finish(false) };
  }

  if (typeof module !== 'undefined' && module.exports) module.exports = { attach };
  else root.AudioxSoundboardOrder = { attach };
})(globalThis);
