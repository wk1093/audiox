const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');

test('USB settings default off and survive config load/save round trips', () => {
  const context = vm.createContext({});
  vm.runInContext(source.slice(source.indexOf('function parseConfigText('),
    source.indexOf('function parseRoutingText(')), context);
  const defaults = context.parseConfigText('');
  assert.equal(defaults.midi_passthrough, 'off');
  assert.equal(defaults.keyboard_macros, 0);
  const config = context.parseConfigText('midi_passthrough=unmapped\nkeyboard_macros=1\n');
  const output = context.formatConfigText(config);
  assert.match(output, /midi_passthrough=unmapped\nkeyboard_macros=1\n/);
  assert.equal(context.parseConfigText(output).keyboard_macros, 1);
  assert.equal(context.parseConfigText('midi_passthrough=unknown').midi_passthrough, 'off');
});

function fixture() {
  const elements = new Map();
  const confirmations = [];
  let confirmResult = true;
  function element() {
    return {
      value: '', disabled: false, children: [],
      addEventListener(name, callback) {
        if (name === 'click') this.onClick = callback;
        else this[name] = callback;
      },
      click(...args) { return this.onClick?.(...args); },
      append(...children) { this.children.push(...children); },
      appendChild(child) { this.children.push(child); },
      replaceChildren() { this.children = []; },
      setAttribute(name, value) { this[name] = value; },
      focus() { this.focused = true; },
    };
  }
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  let chords = [], fail = false, learnFailure = false;
  const calls = [], reads = [], notices = [];
  get('macro-name').value = 'Test macro';
  get('macro-midi-enabled').checked = true;
  const state = { activeTab: 'macros', mappingAssignTarget: null, mappingPollTimer: null };
  const document = { getElementById: get, createElement: element,
    addEventListener(name, callback) { this[name] = callback; }, hidden: false };
  const window = { addEventListener(name, callback) { this[name] = callback; } };
  let note = { connected: true, last_seq: 10, last_note: 60, last_channel: 0 };
  const context = vm.createContext({
    document, window, state, TextEncoder, TextDecoder,
    AudioxUI: {
      confirm: async (...args) => { confirmations.push(args); return confirmResult; },
      setUnsaved() {},
      hasUnsavedChanges: () => false,
    },
    setInterval: () => 1, clearInterval() {},
    renderSoundboardGrid() {},
    setStatus: (...args) => notices.push(args),
    fetch: async (url, options) => {
      if (url === '/api/midi/last_note') return { ok: !learnFailure, status: 503,
        json: async () => note, text: async () => learnFailure ? 'MIDI unavailable' : JSON.stringify(note) };
      assert.ok(url === '/api/macros' || url === '/api/macros/run');
      if (options.method) calls.push({ ...options, url });
      else reads.push(options);
      if (fail) return { ok: false, status: 500, text: async () => 'save failed' };
      if (options.method === 'POST' && url === '/api/macros') {
        const fields = Object.fromEntries(options.body.split('&').map((field) => field.split('=')));
        chords = [{ id: Number(fields.id || 17), name: context.chordHexDecode(fields.name_hex),
          midi_enabled: fields.midi_enabled === '1', channel: Number(fields.channel),
          note: Number(fields.note), keys: fields.keys, script_hex: fields.script_hex,
          idle_velocity: Number(fields.idle_velocity), pressed_velocity: Number(fields.pressed_velocity) }];
      }
      if (options.method === 'DELETE') chords = [];
      return { ok: true, json: async () => ({ macros: chords }) };
    },
  });
  vm.runInContext(source.slice(source.indexOf('let editingMacroId ='),
    source.indexOf('async function reloadConfig(')), context);
  vm.runInContext(source.slice(source.indexOf('async function pollMappingCapture('),
    source.indexOf('async function loadSamplerConfig(')), context);
  return { context, get, calls, reads, notices, document, window, state,
    note: (value) => { note = value; }, fail: () => { fail = true; },
    failLearn: () => { learnFailure = true; },
    confirmations,
    setConfirmResult: (value) => { confirmResult = value; } };
}

test('named macro editor validates input, sends literal plus separators, and removes by stable id', async () => {
  const f = fixture();
  await f.context.loadMidiChords();
  assert.equal(f.reads[0].cache, 'no-store');
  f.get('chord-channel').value = '0';
  f.get('chord-note').value = '60';
  f.get('chord-keys').value = 'CTRL+A&note=99';
  f.get('btn-chord-save').click();
  assert.equal(f.calls.length, 0);
  assert.equal(f.notices.at(-1)[1], 'warn');
  await f.context.saveMidiChord('POST', { name: 'Test macro', midi_enabled: true,
    channel: 0, note: 60, keys: 'CTRL+F13', idle_velocity: 2, pressed_velocity: 5 }, f.get('btn-chord-save'));
  assert.equal(f.calls.at(-1).body, 'name_hex=54657374206d6163726f&midi_enabled=1&channel=0&note=60&keys=CTRL+F13&idle_velocity=2&pressed_velocity=5&script_hex=');
  const row = f.get('chord-list').children[0];
  assert.equal(row.children[0].textContent, 'Test macro — Channel 0, note 60: CTRL+F13 (LED idle 2, pressed 5)');
  await row.children[1].children[0].click();
  assert.equal(f.get('chord-idle-velocity').value, 2);
  assert.equal(f.get('chord-pressed-velocity').value, 5);
  f.setConfirmResult(false);
  await row.children[1].children[1].click();
  assert.equal(f.calls.at(-1).method, 'POST');
  assert.match(f.confirmations.at(-1)[0], /Delete the saved macro/);
  f.setConfirmResult(true);
  await row.children[1].children[1].click();
  assert.equal(f.calls.at(-1).method, 'DELETE');
  assert.equal(f.calls.at(-1).body, 'id=17');
  assert.equal(f.get('chord-list').children.length, 0);
});

function key(code, extra = {}) {
  return { code, preventDefault() { this.prevented = true; }, stopPropagation() {},
    ...extra };
}

test('recording captures physical keys, modifiers, multiple keys and suppresses repeat', () => {
  const f = fixture();
  const input = f.get('chord-keys');
  f.get('btn-chord-record').click();
  assert.equal(input.readOnly, true);
  assert.equal(input.focused, true);
  const event = key('KeyQ', { key: 'a', ctrlKey: true, shiftKey: true, metaKey: true });
  input.keydown(event);
  assert.equal(event.prevented, true);
  input.keydown(key('KeyQ', { repeat: true }));
  input.keydown(key('KeyW', { ctrlKey: true, shiftKey: true, metaKey: true }));
  assert.equal(input.value, 'CTRL+SHIFT+GUI+Q+W');
  input.keyup(key('KeyQ'));
  assert.equal(input.readOnly, true);
  input.keyup(key('KeyW'));
  assert.equal(input.readOnly, false);
  assert.equal(f.get('btn-chord-record')['aria-pressed'], 'false');
  assert.match(f.get('chord-capture-status').textContent, /Captured/);
});

test('recording cancels on blur, hidden page and explicit cancel, and rejects unsupported keys', () => {
  const f = fixture();
  const input = f.get('chord-keys');
  for (const cancel of [
    () => input.blur(), () => f.window.blur(),
    () => { f.document.hidden = true; f.document.visibilitychange(); },
    () => f.get('btn-chord-record').click(),
    () => input.keydown(key('NumpadAdd')),
  ]) {
    f.document.hidden = false;
    input.value = 'ALT+F4';
    f.get('btn-chord-record').click();
    input.keydown(key('KeyA'));
    cancel();
    assert.equal(input.value, 'ALT+F4');
    assert.equal(input.readOnly, false);
  }
});

test('navigation keys are capturable; saving waits until capture finishes', async () => {
  const f = fixture();
  const input = f.get('chord-keys');
  f.get('chord-channel').value = '0';
  f.get('chord-note').value = '60';
  f.get('chord-idle-velocity').value = '3';
  f.get('chord-pressed-velocity').value = '8';
  f.get('btn-chord-record').click();
  input.keydown(key('Tab', { altKey: true }));
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, 0);
  input.keyup(key('Tab'));
  await f.get('btn-chord-save').click();
  assert.match(f.calls.at(-1).body, /keys=ALT\+TAB&idle_velocity=3&pressed_velocity=8/);
});

test('Lua source uses UTF-8 hex without corrupting form separators; size and LED bounds validated', async () => {
  const f = fixture();
  f.get('chord-channel').value = '1';
  f.get('chord-note').value = '61';
  f.get('chord-idle-velocity').value = '2';
  f.get('chord-pressed-velocity').value = '5';
  f.get('chord-mode').value = 'script';
  f.get('chord-mode').change();
  assert.equal(f.get('chord-capture-editor').hidden, true);
  const script = 'tap("CTRL+A")\nwait(200)\n-- café & = +\nlight(5)';
  f.get('chord-script').value = script;
  await f.get('btn-chord-save').click();
  const hex = f.calls.at(-1).body.split('script_hex=')[1];
  assert.equal(f.context.chordHexDecode(hex), script);
  assert.match(f.calls.at(-1).body, /keys=&idle_velocity=2&pressed_velocity=5/);
  const count = f.calls.length;
  f.get('macro-name').value = 'Test source bounds';
  f.get('chord-mode').value = 'script';
  f.get('chord-script').value = script;
  f.get('chord-pressed-velocity').value = '128';
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, count);
  f.get('chord-pressed-velocity').value = '5';
  f.get('chord-script').value = 'é'.repeat(1024);
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, count);
  f.get('chord-script').value = '--' + 'é'.repeat(1022) + 'a';
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, count + 1);
});

test('execution failures from live Lua are shown as text and do not break editing', () => {
  const f = fixture();
  const message = 'macro:3: bad <value> & "argument"';
  f.context.renderMidiChords([{ id: 21, name: '<img src=x>', midi_enabled: false,
    channel: 0, note: 60, keys: '', idle_velocity: 2,
    pressed_velocity: 5, script_hex: f.context.chordHexEncode('type("Hello",80)'),
    runtime_error_hex: f.context.chordHexEncode(message) }]);
  const row = f.get('chord-list').children[0];
  assert.match(row.children[0].textContent, /Last execution failed/);
  assert.ok(row.children[0].textContent.includes(message));
  assert.equal(row.children[0].innerHTML, undefined);
  assert.ok(row.children[0].textContent.startsWith('<img src=x> — Web only:'));
  row.children[1].children[0].click();
  assert.equal(f.get('chord-script').value, 'type("Hello",80)');
  assert.equal(f.get('chord-mode').value, 'script');
});

test('MIDI learning snapshots the last event and selects both channel and note without saving', async () => {
  const f = fixture();
  await f.get('btn-chord-learn').click();
  assert.equal(f.state.mappingAssignTarget, '__macro__');
  await f.context.pollMappingCapture();
  assert.equal(f.get('chord-note').value, '');
  f.note({ connected: true, last_seq: 11, last_note: 42, last_channel: 9 });
  await f.context.pollMappingCapture();
  assert.equal(f.get('chord-channel').value, 9);
  assert.equal(f.get('chord-note').value, 42);
  assert.equal(f.state.mappingAssignTarget, null);
  assert.equal(f.state.mappingPollTimer, null);
  assert.equal(f.calls.length, 0);
  await f.get('btn-chord-learn').click();
  f.document.hidden = true;
  f.document.visibilitychange();
  assert.equal(f.state.mappingAssignTarget, null);
});

test('MIDI learn errors and malformed snapshots are visible and do not leave a polling timer', async () => {
  for (const fail of [(f) => f.failLearn(), (f) => f.note({ connected: true })]) {
    const f = fixture();
    fail(f);
    await f.get('btn-chord-learn').click();
    assert.equal(f.get('btn-chord-learn').disabled, false);
    assert.equal(f.state.mappingAssignTarget, null);
    assert.equal(f.state.mappingPollTimer, null);
    assert.match(f.notices.at(-1)[0], /MIDI learn failed/);
  }
});

test('chord failures are visible and re-enable the save button', async () => {
  const f = fixture();
  f.fail();
  const button = f.get('btn-chord-save');
  await f.context.saveMidiChord('POST', { name: 'Test', channel: 0, note: 60, keys: 'CTRL+F13' }, button);
  assert.equal(button.disabled, false);
  assert.match(f.notices.at(-1)[0], /Macro save failed.*500/);
});

test('editing changes the binding without changing identity; New creates an unbound macro', async () => {
  const f = fixture();
  f.context.renderMidiChords([{ id: 42, name: 'Old', midi_enabled: true, channel: 1,
    note: 63, keys: 'A', idle_velocity: 0, pressed_velocity: 0 }]);
  f.get('chord-list').children[0].children[1].children[0].click();
  f.get('macro-name').value = 'café & = + "雪"';
  f.get('chord-channel').value = '5';
  f.get('chord-note').value = '66';
  f.get('macro-midi-enabled').checked = false;
  f.get('macro-midi-enabled').change();
  assert.equal(f.get('macro-midi-binding').hidden, true);
  await f.get('btn-chord-save').click();
  assert.match(f.calls[0].body, /^id=42&name_hex=[a-f0-9]+&midi_enabled=0&channel=5&note=66&/);
  assert.equal(f.context.chordHexDecode(f.calls[0].body.split('name_hex=')[1].split('&')[0]), 'café & = + "雪"');
  f.get('btn-macro-new').click();
  assert.equal(f.get('macro-midi-enabled').checked, false);
  assert.equal(f.get('chord-idle-velocity').value, '0');
  assert.equal(f.get('chord-pressed-velocity').value, '0');
  f.get('macro-name').value = 'No controller';
  f.get('chord-mode').value = 'script';
  f.get('chord-script').value = 'log(event.source)';
  await f.get('btn-chord-save').click();
  assert.match(f.calls.at(-1).body, /^name_hex=.*&midi_enabled=0&/);
});

test('Run queues saved id only, guards double clicks, and displays non-200 errors', async () => {
  const f = fixture();
  f.context.renderMidiChords([{ id: 99, name: 'Run me', midi_enabled: false, channel: 0, note: 60, keys: 'A' }]);
  const button = f.get('chord-list').children[0].children[1].children[2];
  const first = button.click();
  await button.click();
  await first;
  assert.equal(f.calls.length, 1);
  assert.equal(f.calls[0].url, '/api/macros/run');
  assert.equal(f.calls[0].body, 'id=99');
  assert.equal(button.disabled, false);
  assert.match(f.notices.at(-1)[0], /queued.*not confirmation/);
  f.fail();
  await button.click();
  assert.match(f.notices.at(-1)[0], /Macro run failed.*500/);
  assert.equal(button.disabled, false);
});

test('name limits count UTF-8 bytes and full lists allow edits but not creates', async () => {
  const f = fixture();
  f.get('btn-macro-new').click();
  f.get('chord-keys').value = 'A';
  for (const name of ['', 'é'.repeat(32), 'a'.repeat(64), 'bad\0name']) {
    f.get('macro-name').value = name;
    await f.get('btn-chord-save').click();
    assert.equal(f.calls.length, 0);
  }
  const macros = Array.from({ length: 16 }, (_, n) => ({ id: n + 1, name: `Macro ${n}`,
    midi_enabled: false, channel: 0, note: 60, keys: 'A', idle_velocity: 0, pressed_velocity: 0 }));
  f.context.renderMidiChords(macros);
  f.get('macro-name').value = 'é'.repeat(31) + 'a';
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, 0);
  f.get('chord-list').children[0].children[1].children[0].click();
  f.get('macro-name').value = 'é'.repeat(31) + 'a';
  await f.get('btn-chord-save').click();
  assert.match(f.calls[0].body, /^id=1&/);
});

test('unbinding cancels MIDI learn and navigation exposes the standalone editor', async () => {
  const f = fixture();
  await f.get('btn-chord-learn').click();
  f.get('macro-midi-enabled').checked = false;
  f.get('macro-midi-enabled').change();
  assert.equal(f.state.mappingAssignTarget, null);
  assert.equal(f.state.mappingPollTimer, null);
  assert.equal(f.get('btn-chord-learn').disabled, true);
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  assert.match(html, /data-tab="macros">Macros/);
  const macrosPage = html.split('data-page="macros"')[1].split('data-page="midi"')[0];
  assert.match(macrosPage, /id="chord-list"/);
  assert.doesNotMatch(html.split('data-page="midi"')[1], /id="chord-list"/);
});

test('switching from Macros to MIDI cancels recording and learn; Macros loads its own API', async () => {
  const f = fixture();
  f.document.querySelectorAll = () => [];
  Object.assign(f.context, {
    stopSystemPolling() {}, loadLightDim() {}, renderStopAllMidiStatus() {},
    renderSamplerToggleMidiStatus() {},
  });
  vm.runInContext(source.slice(source.indexOf('function setActiveTab('),
    source.indexOf('\nfunction ', source.indexOf('function setActiveTab(') + 1)), f.context);
  f.get('btn-chord-record').click();
  await f.get('btn-chord-learn').click();
  f.context.setActiveTab('midi');
  assert.equal(f.get('chord-keys').readOnly, false);
  assert.equal(f.state.mappingAssignTarget, null);
  assert.equal(f.reads.length, 0);
  f.context.setActiveTab('macros');
  assert.equal(f.reads.length, 1);
});
