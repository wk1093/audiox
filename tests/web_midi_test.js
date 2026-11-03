const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');

test('USB keyboard settings remain backwards compatible', () => {
  const context = vm.createContext({});
  vm.runInContext(source.slice(source.indexOf('function parseConfigText('),
    source.indexOf('function parseRoutingText(')), context);
  assert.equal(context.parseConfigText('').keyboard_macros, 0);
  const config = context.parseConfigText('midi_passthrough=unmapped\nkeyboard_macros=1\n');
  const output = context.formatConfigText(config);
  assert.match(output, /midi_passthrough=unmapped\nkeyboard_macros=1\n/);
  assert.equal(context.parseConfigText(output).keyboard_macros, 1);
});

function fixture() {
  const elements = new Map(), confirmations = [], calls = [], reads = [], notices = [];
  let confirmResult = true, fail = false, learnFailure = false, scripts = [];
  let note = { connected: true, last_seq: 10, last_note: 60, last_channel: 0 };
  function element() {
    return {
      value: '', disabled: false, children: [],
      addEventListener(name, callback) { this[name] = callback; },
      append(...children) { this.children.push(...children); },
      appendChild(child) { this.children.push(child); },
      replaceChildren() { this.children = []; },
      setAttribute(name, value) { this[name] = value; },
      focus() { this.focused = true; },
    };
  }
  const get = id => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  const state = { activeTab: 'scripts', mappingAssignTarget: null, mappingPollTimer: null };
  const document = { getElementById: get, createElement: element, hidden: false,
    addEventListener(name, callback) { this[name] = callback; }, querySelectorAll: () => [] };
  const context = vm.createContext({
    document, window: {}, state, TextEncoder, TextDecoder,
    AudioxUI: {
      confirm: async (...args) => { confirmations.push(args); return confirmResult; },
      setUnsaved() {}, hasUnsavedChanges: () => false,
    },
    setInterval: () => 1, clearInterval() {},
    renderSoundboardGrid() {}, startMeterPolling() {}, stopMeterPolling() {},
    setStatus: (...args) => notices.push(args),
    fetch: async (url, options) => {
      if (url === '/api/midi/last_note') return { ok: !learnFailure, status: 503,
        json: async () => note, text: async () => learnFailure ? 'MIDI unavailable' : JSON.stringify(note) };
      assert.ok(url === '/api/scripts' || url === '/api/scripts/run');
      if (options.method) calls.push({ ...options, url });
      else reads.push(options);
      if (fail) return { ok: false, status: 500, text: async () => 'save failed' };
      if (options.method === 'POST' && url === '/api/scripts') {
        const fields = Object.fromEntries(new URLSearchParams(options.body));
        scripts = [{ id: Number(fields.id || 17), name: context.chordHexDecode(fields.name_hex),
          midi_enabled: fields.midi_enabled === '1', channel: Number(fields.channel),
          note: Number(fields.note), script_hex: fields.script_hex, hook_mode: true, event_mask: 128,
          config_write_allowed: fields.config_write_ack === '1',
          idle_velocity: Number(fields.idle_velocity), pressed_velocity: Number(fields.pressed_velocity) }];
      }
      if (options.method === 'DELETE') scripts = [];
      return { ok: true, json: async () => ({ scripts }) };
    },
  });
  vm.runInContext(source.slice(source.indexOf('let editingMacroId ='),
    source.indexOf('async function reloadConfig(')), context);
  vm.runInContext(source.slice(source.indexOf('async function pollMappingCapture('),
    source.indexOf('async function loadSamplerConfig(')), context);
  context.resetMacroEditor();
  get('macro-name').value = 'Test script';
  return { context, get, calls, reads, notices, document, state, confirmations,
    note: value => { note = value; }, fail: () => { fail = true; },
    failLearn: () => { learnFailure = true; },
    setConfirmResult: value => { confirmResult = value; } };
}

test('Scripts is callback-only with no sequence, shortcut recorder or subscription controls', () => {
  const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
  assert.match(html, /data-tab="scripts">Scripts/);
  assert.match(html, /data-page="scripts"/);
  assert.match(html, />Save Script</);
  assert.doesNotMatch(html, /id="(?:chord-mode|chord-keys|btn-chord-record|macro-event-options)"/);
  assert.doesNotMatch(source, /stopChordRecording|chordRecording|readMacroEventMask/);
  const f = fixture();
  assert.match(f.get('chord-script').value, /hook\("run", function\(event\)/);
});

test('callback saves use Scripts API, preserve source and restore editing identity', async () => {
  const f = fixture();
  await f.context.loadMidiChords();
  assert.equal(f.reads[0].cache, 'no-store');
  const code = 'hook("run",function(e) tap("CTRL+A"); log("café & = + 雪") end)';
  f.get('chord-script').value = code;
  await f.get('btn-chord-save').click();
  const fields = new URLSearchParams(f.calls[0].body);
  assert.equal(f.context.chordHexDecode(fields.get('script_hex')), code);
  for (const removed of ['keys', 'event_mask', 'hook_mode']) assert.equal(fields.has(removed), false);
  const row = f.get('chord-list').children[0];
  await row.children[1].children[0].click();
  assert.equal(f.get('chord-script').value, code);
  f.get('macro-name').value = 'Renamed';
  await f.get('btn-chord-save').click();
  assert.match(f.calls[1].body, /^id=17&/);
  assert.equal(f.get('btn-chord-save').textContent, 'Save Script');
});

test('Run is available only for run hooks and guards duplicate requests', async () => {
  const f = fixture();
  const script = { id: 99, name: 'Observer', midi_enabled: false, channel: 0, note: 60,
    event_mask: 16, script_hex: f.context.chordHexEncode('hook("sound_finish",function(e) log(e.sound) end)') };
  f.context.renderMidiChords([script]);
  assert.equal(f.get('chord-list').children[0].children[1].children[2].disabled, true);
  f.context.renderMidiChords([{ ...script, event_mask: 144 }]);
  const button = f.get('chord-list').children[0].children[1].children[2];
  const first = button.click();
  await button.click();
  await first;
  assert.equal(f.calls.length, 1);
  assert.equal(f.calls[0].url, '/api/scripts/run');
  assert.equal(f.calls[0].body, 'id=99');
  assert.match(f.notices.at(-1)[0], /Script queued.*not confirmation/);
  f.fail();
  await button.click();
  assert.match(f.notices.at(-1)[0], /Script run failed.*500/);
  assert.equal(button.disabled, false);
});

test('configuration permissions require consent and cancellation sends no request', async () => {
  const f = fixture();
  const script = { name: 'Config', midi_enabled: false, channel: 0, note: 60,
    script_hex: f.context.chordHexEncode('hook("run",function() log(config.read("config.txt")) end)') };
  f.setConfirmResult(false);
  await f.context.saveMidiChord('POST', script, f.get('btn-chord-save'));
  assert.equal(f.calls.length, 0);
  assert.equal(f.get('btn-chord-save').disabled, false);
  f.setConfirmResult(true);
  await f.context.saveMidiChord('POST', script, f.get('btn-chord-save'));
  assert.match(f.calls[0].body, /config_write_ack=1/);
  await f.get('chord-list').children[0].children[1].children[0].click();
  assert.equal(f.get('macro-config-write').checked, true);
  f.setConfirmResult(false);
  await f.context.saveMidiChord('POST', { ...script, config_write_allowed: true,
    script_hex: f.context.chordHexEncode('hook("run",function() log("dynamic") end)') }, f.get('btn-chord-save'));
  assert.equal(f.confirmations.length, 3);
  assert.equal(f.calls.length, 1);
});

test('UTF-8 source, names, NUL and LED bounds are checked before saving', async () => {
  const f = fixture();
  for (const code of ['', ' ', 'x\0y', 'é'.repeat(8704) + 'x']) {
    f.get('chord-script').value = code;
    await f.get('btn-chord-save').click();
  }
  assert.equal(f.calls.length, 0);
  f.get('chord-script').value = '--' + 'é'.repeat(8703);
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, 1);
  f.context.resetMacroEditor();
  for (const name of ['', 'é'.repeat(32), 'a'.repeat(64), 'bad\0name']) {
    f.get('macro-name').value = name;
    await f.get('btn-chord-save').click();
  }
  f.get('macro-name').value = 'Valid';
  f.get('chord-pressed-velocity').value = '128';
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, 1);
});

test('full lists permit edits, deletion requires confirmation, New resets binding', async () => {
  const f = fixture();
  const scripts = Array.from({ length: 16 }, (_, i) => ({ id: i + 1, name: `Script ${i}`,
    midi_enabled: true, channel: 0, note: 60, event_mask: 128, idle_velocity: 0,
    pressed_velocity: 0, script_hex: f.context.chordHexEncode('hook("run",function() end)') }));
  f.context.renderMidiChords(scripts);
  await f.get('btn-chord-save').click();
  assert.equal(f.calls.length, 0);
  await f.get('chord-list').children[0].children[1].children[0].click();
  await f.get('btn-chord-save').click();
  assert.match(f.calls[0].body, /^id=1&/);
  const remove = f.get('chord-list').children[0].children[1].children[1];
  f.setConfirmResult(false);
  await remove.click();
  assert.equal(f.calls.length, 1);
  f.setConfirmResult(true);
  await remove.click();
  assert.equal(f.calls[1].body, 'id=1');
  f.get('btn-macro-new').click();
  assert.equal(f.get('macro-midi-enabled').checked, false);
  assert.equal(f.get('chord-idle-velocity').value, '0');
  assert.match(f.get('chord-script').value, /hook\("run"/);
});

test('runtime errors render as text and save/load failures remain visible', async () => {
  const f = fixture();
  const message = 'script:3: bad <value> & "argument"';
  f.context.renderMidiChords([{ id: 1, name: '<img src=x>', midi_enabled: false,
    event_mask: 128, script_hex: f.context.chordHexEncode('hook("run",function() end)'),
    runtime_error_hex: f.context.chordHexEncode(message) }]);
  const label = f.get('chord-list').children[0].children[0];
  assert.ok(label.textContent.includes(message));
  assert.equal(label.innerHTML, undefined);
  f.fail();
  await f.get('btn-chord-save').click();
  assert.match(f.notices.at(-1)[0], /Script save failed.*500/);
  assert.equal(f.get('btn-chord-save').disabled, false);
  await f.context.loadMidiChords();
  assert.match(f.notices.at(-1)[0], /Script load failed.*500/);
});

test('MIDI learning matches the next event and unbinding cancels learning', async () => {
  const f = fixture();
  f.get('macro-midi-enabled').checked = true;
  f.get('macro-midi-enabled').change();
  await f.get('btn-chord-learn').click();
  assert.equal(f.state.mappingAssignTarget, '__macro__');
  await f.context.pollMappingCapture();
  assert.equal(f.get('chord-note').value, '60');
  f.note({ connected: true, last_seq: 11, last_note: 42, last_channel: 9 });
  await f.context.pollMappingCapture();
  assert.equal(f.get('chord-note').value, 42);
  assert.equal(f.get('chord-channel').value, 9);
  assert.equal(f.calls.length, 0);
  await f.get('btn-chord-learn').click();
  f.get('macro-midi-enabled').checked = false;
  f.get('macro-midi-enabled').change();
  assert.equal(f.state.mappingAssignTarget, null);
  assert.equal(f.get('btn-chord-learn').disabled, true);
  f.get('macro-midi-enabled').checked = true;
  f.get('macro-midi-enabled').change();
  f.failLearn();
  await f.get('btn-chord-learn').click();
  assert.match(f.notices.at(-1)[0], /MIDI learn failed.*503/);
});

test('leaving Scripts cancels MIDI learning; returning loads Scripts API', async () => {
  const f = fixture();
  Object.assign(f.context, { stopSystemPolling() {}, loadLightDim() {},
    renderStopAllMidiStatus() {}, renderSamplerToggleMidiStatus() {} });
  vm.runInContext(source.slice(source.indexOf('function setActiveTab('),
    source.indexOf('\nfunction ', source.indexOf('function setActiveTab(') + 1)), f.context);
  f.get('macro-midi-enabled').checked = true;
  f.get('macro-midi-enabled').change();
  await f.get('btn-chord-learn').click();
  f.context.setActiveTab('midi');
  assert.equal(f.state.mappingAssignTarget, null);
  f.context.setActiveTab('scripts');
  assert.equal(f.reads.length, 1);
});
