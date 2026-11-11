const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const source = require('./support/app-source.js').appSource();

function fixture() {
  class Element {
    constructor(tag) {
      this.tagName = tag.toUpperCase();
      this.children = [];
      this.className = '';
      this.classList = { contains: (name) => this.className.split(' ').includes(name) };
      this.listeners = {};
    }
    appendChild(child) {
      if (child.parent) child.parent.children = child.parent.children.filter((item) => item !== child);
      child.parent = this;
      this.children.push(child);
    }
    append(...children) { children.forEach((child) => this.appendChild(child)); }
    setAttribute(name, value) { this[name] = value; }
    addEventListener(name, callback) { this.listeners[name] = callback; }
  }
  const document = { createElement: (tag) => new Element(tag) };
  const state = { soundboardOpenSettings: new Set() };
  const context = apiContext({ document, state });
  vm.runInContext(source.slice(source.indexOf('function groupSoundboardTileSettings('),
    source.indexOf('function renderSoundboardGrid(')), context);
  const actions = new Element('div');
  const preview = new Element('button');
  preview.className = 'primary sb-play';
  const position = new Element('select');
  position.className = 'sb-edit-only sb-grid-cell-select';
  const remove = new Element('button');
  remove.className = 'warn sb-edit-only';
  const clear = new Element('button');
  clear.className = 'sb-midi-clear sb-edit-only';
  const midi = new Element('div');
  midi.append(clear);
  midi.querySelector = () => midi.children.find((item) => item === clear);
  actions.append(position, preview, remove);
  const group = (target = false) => {
    context.groupSoundboardTileSettings(actions, midi, 'one.wav', 'One', target);
    return actions.children.at(-1);
  };
  return { state, actions, preview, position, remove, clear, midi, group };
}

test('tile settings collapse management controls but keep playback outside', () => {
  const f = fixture();
  const settings = f.group();
  assert.equal(settings.open, false);
  assert.deepEqual(f.actions.children, [f.preview, settings]);
  assert.equal(settings.className, 'sb-tile-settings sb-edit-only');
  assert.equal(settings.children[0]['aria-label'], 'Settings for One');
  const controls = settings.children[1];
  assert.equal(controls.children[0].textContent, 'Grid position');
  assert.equal(controls.children[0].children[0], f.position);
  assert.ok(controls.children.includes(f.remove));
  assert.ok(controls.children.includes(f.clear));
  assert.equal(f.clear.textContent, 'Clear MIDI mapping');
  assert.equal(f.midi.children.length, 0);
});

test('open settings survive rerenders and MIDI learning opens the target', () => {
  const f = fixture();
  const settings = f.group();
  settings.open = true;
  settings.listeners.toggle();
  assert.equal(f.state.soundboardOpenSettings.has('one.wav'), true);
  settings.open = false;
  settings.listeners.toggle();
  assert.equal(f.state.soundboardOpenSettings.has('one.wav'), false);
  const restored = fixture();
  restored.state.soundboardOpenSettings.add('one.wav');
  assert.equal(restored.group().open, true);
  const learning = fixture();
  const target = learning.group(true);
  assert.equal(target.open, true);
  assert.match(target.children[0].textContent, /Waiting for MIDI/);
});

test('setup and upload are collapsed Edit-only panels', () => {
  const html = fs.readFileSync(require.resolve('../../web/embedded/index.html'), 'utf8');
  assert.match(html, /class="sb-edit-tools sb-edit-only"/);
  for (const id of ['sb-grid-settings', 'sb-add-sound']) {
    const panel = html.match(new RegExp(`<details[^>]*id="${id}"[^>]*>`));
    assert.ok(panel);
    assert.doesNotMatch(panel[0], /\bopen\b/);
  }
});
