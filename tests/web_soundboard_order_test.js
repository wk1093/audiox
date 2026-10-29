const assert = require('node:assert/strict');
const { test } = require('node:test');
const { attach } = require('../web/soundboard-order.js');

function fixture() {
  const events = new EventTarget();
  global.addEventListener = events.addEventListener.bind(events);
  global.requestAnimationFrame = () => 1;
  global.cancelAnimationFrame = () => {};
  const grid = new EventTarget();
  grid.children = [];
  grid.capture = null;
  grid.setPointerCapture = (id) => { grid.capture = id; };
  grid.hasPointerCapture = (id) => grid.capture === id;
  grid.releasePointerCapture = () => { grid.capture = null; };
  grid.insertBefore = (card, target) => {
    grid.children.splice(grid.children.indexOf(card), 1);
    grid.children.splice(target ? grid.children.indexOf(target) : grid.children.length, 0, card);
  };
  grid.append = (card) => grid.insertBefore(card, null);
  for (const file of ['one.wav', 'two.wav', 'three.wav']) {
    const card = { dataset: { file }, parentElement: grid, classList: { add() {}, remove() {} } };
    const name = {
      closest: (selector) => selector === '.sb-card-name' ? name : selector === '.sb-card' ? card : null,
      setAttribute() {}, focus() {},
    };
    card.querySelector = () => name;
    card.closest = (selector) => selector === '.sb-card' ? card : null;
    Object.defineProperty(card, 'nextSibling', {
      get: () => grid.children[grid.children.indexOf(card) + 1] || null,
    });
    grid.children.push(card);
  }
  let hit = grid.children[2];
  global.document = { elementFromPoint: () => hit };
  let enabled = true;
  const saves = [];
  const controller = attach(grid, {
    enabled: () => enabled, save: (...args) => saves.push(args), status: { textContent: '' },
  });
  const fire = (type, target, details = {}, host = grid) => {
    const event = new Event(type, { cancelable: true });
    Object.defineProperty(event, 'target', { value: target });
    for (const [key, value] of Object.entries(details)) Object.defineProperty(event, key, { value });
    host.dispatchEvent(event);
  };
  return { grid, events, controller, saves, fire, order: () => grid.children.map((c) => c.dataset.file),
    setEnabled: (value) => { enabled = value; }, setHit: (card) => { hit = card; } };
}

test('dragging a card saves the complete reordered list once on pointer release', () => {
  const f = fixture();
  const first = f.grid.children[0].querySelector();
  f.fire('pointerdown', first, { button: 0, pointerId: 7, clientX: 10, clientY: 100 });
  f.fire('pointermove', first, { pointerId: 7, clientX: 100, clientY: 100 });
  assert.deepEqual(f.order(), ['two.wav', 'three.wav', 'one.wav']);
  assert.equal(f.saves.length, 0);
  f.fire('pointerup', first, { pointerId: 7 });
  assert.deepEqual(f.saves, [[['two.wav', 'three.wav', 'one.wav'], 'one.wav']]);
  assert.equal(f.grid.capture, null);
});

test('pointer cancellation, Escape, and an unchanged click restore order without saving', () => {
  for (const cancel of ['pointercancel', 'escape', 'cancel']) {
    const f = fixture();
    const name = f.grid.children[0].querySelector();
    f.fire('pointerdown', name, { button: 0, pointerId: 1, clientX: 0, clientY: 100 });
    f.fire('pointermove', name, { pointerId: 1, clientX: 100, clientY: 100 });
    if (cancel === 'escape') f.fire('keydown', name, { key: 'Escape' }, f.events);
    else if (cancel === 'cancel') f.controller.cancel();
    else f.fire(cancel, name);
    assert.deepEqual(f.order(), ['one.wav', 'two.wav', 'three.wav']);
    assert.equal(f.saves.length, 0);
  }
  const f = fixture();
  const name = f.grid.children[0].querySelector();
  f.fire('pointerdown', name, { button: 0, pointerId: 1, clientX: 0, clientY: 100 });
  f.fire('pointerup', name, { pointerId: 1 });
  assert.equal(f.saves.length, 0);
});

test('keyboard ordering uses Space, arrows and Enter; disabled Play/old-firmware modes cannot drag', () => {
  const f = fixture();
  const name = f.grid.children[0].querySelector();
  for (const key of [' ', 'ArrowRight', 'Enter']) f.fire('keydown', name, { key });
  assert.deepEqual(f.saves[0], [['two.wav', 'one.wav', 'three.wav'], 'one.wav']);
  f.setEnabled(false);
  f.fire('pointerdown', name, { button: 0, pointerId: 1, clientX: 0, clientY: 100 });
  f.fire('pointermove', name, { pointerId: 1, clientX: 100, clientY: 100 });
  f.fire('pointerup', name, { pointerId: 1 });
  assert.equal(f.saves.length, 1);
});
