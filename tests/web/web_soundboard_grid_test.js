const assert = require('node:assert/strict');
const { test } = require('node:test');
require('../../web/embedded/soundboard-grid.js');

const { cellsBySound, midiNoteAt, attach, elements, actionFor } = globalThis.AudioxSoundboardGrid;

test('stop and sampler actions share grid placement without becoming audio files', () => {
  const sounds = ['one.wav'];
  const files = elements(sounds);
  assert.deepEqual(sounds, ['one.wav']);
  assert.deepEqual(files, ['one.wav', '__stop_all__', '__sampler_toggle__']);
  assert.equal(actionFor('__stop_all__').action, 'stop_all');
  assert.equal(actionFor('__sampler_toggle__').label, 'Sampler Toggle');
  assert.equal(actionFor('one.wav'), undefined);
  const cells = cellsBySound({ rows: 2, columns: 2, cells: [
    { sfx: '__stop_all__', row: 0, column: 1 },
    { sfx: '__sampler_toggle__', row: 1, column: 0 },
  ] }, files);
  assert.deepEqual({ ...cells }, {
    __stop_all__: { row: 0, column: 1 }, __sampler_toggle__: { row: 1, column: 0 },
  });
});

test('grid leaves unassigned sounds out while preserving saved positions and empty spaces', () => {
  const cells = cellsBySound({
    rows: 2,
    columns: 3,
    cells: [
      { sfx: 'three.wav', row: 1, column: 2 },
      { sfx: 'missing.wav', row: 0, column: 0 },
      { sfx: 'duplicate.wav', row: 1, column: 2 },
    ],
  }, ['one.wav', 'two.wav', 'three.wav']);

  assert.deepEqual({ ...cells }, {
    'three.wav': { row: 1, column: 2 },
  });
});

test('empty, full, and resized grids do not silently assign sounds', () => {
  assert.deepEqual({ ...cellsBySound({ rows: 1, columns: 1, cells: [] }, ['one.wav']) }, {});
  assert.deepEqual({ ...cellsBySound({ rows: 1, columns: 1, cells: [
    { sfx: 'one.wav', row: 0, column: 0 }, { sfx: 'two.wav', row: 1, column: 0 },
  ] }, ['one.wav', 'two.wav']) }, { 'one.wav': { row: 0, column: 0 } });
});

function dragFixture() {
  const events = new EventTarget();
  global.addEventListener = events.addEventListener.bind(events);
  global.requestAnimationFrame = () => 1;
  global.cancelAnimationFrame = () => {};
  const container = new EventTarget();
  container.capture = null;
  container.setPointerCapture = (id) => { container.capture = id; };
  container.hasPointerCapture = (id) => id === container.capture;
  container.releasePointerCapture = () => { container.capture = null; };
  const classes = () => ({ add() {}, remove() {} });
  const card = { dataset: { file: 'one.wav', gridRow: '0', gridColumn: '0' }, classList: classes() };
  const name = { closest: (selector) => selector === '.sb-card' ? card :
    selector === '.sb-card-name' ? name : null };
  const target = { dataset: { gridRow: '1', gridColumn: '1' }, classList: classes() };
  const unassigned = { dataset: { gridUnassigned: '' }, classList: classes() };
  container.contains = (node) => [card, target, unassigned].includes(node);
  let hit = target;
  global.document = { elementFromPoint: () => hit && { closest: () => hit } };
  let enabled = true;
  const saves = [];
  const controller = attach(container, {
    enabled: () => enabled, save: (...args) => saves.push(args), status: { textContent: '' },
  });
  const fire = (type, details = {}, host = container, source = name) => {
    const event = new Event(type, { cancelable: true });
    Object.defineProperty(event, 'target', { value: source });
    for (const [key, value] of Object.entries(details)) Object.defineProperty(event, key, { value });
    host.dispatchEvent(event);
  };
  const down = () => fire('pointerdown', { button: 0, pointerId: 7, pointerType: 'touch', clientX: 10, clientY: 100 });
  const move = () => fire('pointermove', { pointerId: 7, clientX: 100, clientY: 100 });
  const up = () => fire('pointerup', { pointerId: 7, clientX: 100, clientY: 100 });
  return { container, card, target, unassigned, events, controller, saves, fire, down, move, up,
    setHit: (value) => { hit = value; }, disable: () => { enabled = false; } };
}

test('pointer dragging assigned and unassigned sounds saves a target cell only on release', () => {
  for (const assigned of [true, false]) {
    const f = dragFixture();
    if (!assigned) f.card.dataset = { file: 'one.wav' };
    f.down();
    f.move();
    assert.equal(f.saves.length, 0);
    f.up();
    assert.deepEqual(f.saves, [['one.wav', 1, 1]]);
    assert.equal(f.container.capture, null);
  }
});

test('dropping into unassigned uses an explicit unassignment', () => {
  const f = dragFixture();
  f.setHit(f.unassigned);
  f.down(); f.move(); f.up();
  assert.deepEqual(f.saves, [['one.wav', null]]);
});

test('cancelled, outside, disabled, and click-only grid drags never save', () => {
  for (const action of ['pointercancel', 'escape', 'blur', 'outside', 'disabled', 'click', 'cancel']) {
    const f = dragFixture();
    f.down();
    if (action !== 'click') f.move();
    if (action === 'pointercancel') f.fire('pointercancel');
    if (action === 'escape') f.fire('keydown', { key: 'Escape' }, f.events);
    if (action === 'blur') f.fire('blur', {}, f.events);
    if (action === 'outside') f.setHit(null);
    if (action === 'disabled') f.disable();
    if (action === 'cancel') f.controller.cancel();
    f.up();
    assert.equal(f.saves.length, 0, action);
  }
});
test('four learned corners interpolate an 8x8 pad grid in either orientation', () => {
  const normal = { rows: 8, columns: 8, corners: [36, 43, 92, 99] };
  assert.equal(midiNoteAt(normal, 0, 0), 36);
  assert.equal(midiNoteAt(normal, 0, 7), 43);
  assert.equal(midiNoteAt(normal, 7, 0), 92);
  assert.equal(midiNoteAt(normal, 7, 7), 99);
  assert.equal(midiNoteAt(normal, 3, 4), 64);

  const reversed = { rows: 8, columns: 8, corners: [99, 92, 43, 36] };
  assert.equal(midiNoteAt(reversed, 3, 4), 71);
});

test('invalid, incomplete, or non-linear MIDI corner learning does not produce notes', () => {
  assert.equal(midiNoteAt({ rows: 4, columns: 4, corners: [36, 39, 48, null] }, 1, 1), null);
  assert.equal(midiNoteAt({ rows: 4, columns: 4, corners: [36, 41, 48, 53] }, 1, 1), null);
  assert.equal(midiNoteAt({ rows: 4, columns: 4, corners: [36, 39, 48, 52] }, 1, 1), null);
  assert.equal(midiNoteAt({ rows: 1, columns: 4, corners: [36, 39, 48, 51] }, 0, 0), null);
});
