const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require.resolve('../../web/embedded/app.js'), 'utf8');

test('passthrough-only status, binding and toggle use the MIDI API and surface errors', async () => {
  const elements = new Map(), calls = [], notices = [];
  let active = false, fail = false;
  const context = apiContext({
    state: {},
    document: { getElementById(id) {
      if (!elements.has(id)) elements.set(id, { setAttribute(key, value) { this[key] = value; } });
      return elements.get(id);
    } },
    renderStopAllMidiStatus() {}, renderSamplerToggleMidiStatus() {}, renderSoundboardGrid() {},
    setStatus: (...args) => notices.push(args),
    fetch: async (url, options) => {
      calls.push({ url, ...options });
      if (url === '/api/midi/actions') return { ok: true, text: async () =>
        JSON.stringify({ actions: [{ note: 120, channel: 9, action: 'passthrough_toggle' }] }) };
      assert.equal(url, '/api/midi/passthrough_only');
      if (fail) return { ok: false, status: 409, text: async () => 'Enable USB MIDI first' };
      if (options.method === 'POST') active = !active;
      return { ok: true, text: async () => JSON.stringify({ active }) };
    },
  });
  vm.runInContext(source.slice(source.indexOf('async function loadMidiActions('),
    source.indexOf('async function saveActionMapping(')), context);
  await context.loadMidiActions();
  assert.equal(context.state.passthroughToggleMidiNote, 120);
  assert.equal(elements.get('passthrough-toggle-midi').textContent, 'channel 9, note 120');
  await context.loadPassthroughOnly();
  assert.equal(elements.get('btn-passthrough-only')['aria-pressed'], 'false');
  await context.loadPassthroughOnly(true);
  assert.equal(elements.get('btn-passthrough-only')['aria-pressed'], 'true');
  assert.equal(calls.at(-1).method, 'POST');
  fail = true;
  await context.loadPassthroughOnly(true);
  assert.match(notices.at(-1)[0], /Enable USB MIDI first/);
  assert.equal(elements.get('btn-passthrough-only').disabled, false);
});

test('USB keyboard settings remain backwards compatible', () => {
  const context = apiContext({});
  vm.runInContext(source.slice(source.indexOf('function parseConfigText('),
    source.indexOf('function parseRoutingText(')), context);
  assert.equal(context.parseConfigText('').keyboard_macros, 0);
  const config = context.parseConfigText('midi_passthrough=unmapped\nkeyboard_macros=1\n');
  const output = context.formatConfigText(config);
  assert.match(output, /midi_passthrough=unmapped\nkeyboard_macros=1\n/);
  assert.equal(context.parseConfigText(output).keyboard_macros, 1);
});

test('passthrough binding saves the learned channel without changing note-only actions', async () => {
  const calls = [];
  const context = apiContext({
    setStatus() {}, loadMappings: async () => {}, loadMidiActions: async () => {},
    fetch: async (url, options) => {
      calls.push({ url, ...options });
      return { ok: true, text: async () => 'ok' };
    },
  });
  vm.runInContext(source.slice(source.indexOf('async function saveActionMapping('),
    source.indexOf('async function saveStopAllActionMapping(')), context);
  assert.equal(await context.saveActionMapping(120, 'passthrough_toggle', 9), true);
  assert.equal(calls[0].body, 'note=120&action=passthrough_toggle&channel=9');
  await context.saveActionMapping(39, 'stop_all');
  assert.equal(calls[1].body, 'note=39&action=stop_all');
});

test('passthrough toggle learns the next note as an action and reports capture failure', async () => {
  const f = fixture(), bindings = [];
  f.context.midiActionCaptureStatusEl = f.get('midi-action-capture-status');
  f.context.saveActionMapping = async (...args) => { bindings.push(args); return true; };
  f.state.mappingAssignTarget = '__action_passthrough_toggle__';
  f.state.mappingLastSeq = 10;
  f.note({ connected: true, last_seq: 11, last_note: 120, last_channel: 9 });
  await f.context.pollMappingCapture();
  assert.deepEqual(bindings, [[120, 'passthrough_toggle', 9]]);
  assert.equal(f.state.mappingAssignTarget, null);
  assert.match(f.get('midi-action-capture-status').textContent, /Assigned channel 9, note 120/);
  f.state.mappingAssignTarget = '__action_passthrough_toggle__';
  f.failLearn();
  await f.context.pollMappingCapture();
  assert.match(f.notices.at(-1)[0], /MIDI unavailable/);
  assert.equal(f.state.mappingAssignTarget, null);
});

function fixture() {
  const elements = new Map(), confirmations = [], calls = [], reads = [], notices = [];
  let confirmResult = true, fail = false, learnFailure = false, scripts = [], unsaved = false;
  let activityWarning = false;
  let waitForControl = null, releaseControl;
  let note = { connected: true, last_seq: 10, last_note: 60, last_channel: 0 };
  function element(tagName = 'div') {
    return {
      tagName, value: '', disabled: false, children: [],
      set innerHTML(value) { throw new Error(`Unsafe HTML: ${value}`); },
      addEventListener(name, callback) { this[name] = callback; },
      dispatchEvent(event) { this[event.type]?.(event); },
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
  const context = apiContext({
    document, window: {}, state, TextEncoder, TextDecoder,
    AudioxLuaEditor: require('../../web/embedded/lua-editor.js'),
    AudioxUI: {
      confirm: async (...args) => { confirmations.push(args); return confirmResult; },
      setUnsaved: (key, value) => { unsaved = value; }, hasUnsavedChanges: () => unsaved,
    },
    setInterval: () => 1, clearInterval() {},
    renderSoundboardGrid() {}, startMeterPolling() {}, stopMeterPolling() {},
    setStatus: (...args) => notices.push(args),
    fetch: async (url, options) => {
      if (url === '/api/midi/last_note') return { ok: !learnFailure, status: 503,
        json: async () => note, text: async () => learnFailure ? 'MIDI unavailable' : JSON.stringify(note) };
      assert.ok(url === '/api/scripts' || url === '/api/scripts/run' || url === '/api/scripts/control');
      if (options.method) calls.push({ ...options, url });
      else reads.push(options);
      if (url === '/api/scripts/control' && waitForControl) await waitForControl;
      if (fail) return { ok: false, status: 500, text: async () => 'save failed' };
      if (url === '/api/scripts/control') {
        const fields = new URLSearchParams(options.body);
        const control = scripts.find(script => script.id === Number(fields.get('id')))?.ui
          ?.find(control => control.key === context.chordHexDecode(fields.get('key_hex')));
        if (control && fields.has('value_hex')) {
          const value = context.chordHexDecode(fields.get('value_hex'));
          control.value = control.type === 'boolean' ? value === 'true' :
            control.type === 'number' ? Number(value) : value;
        }
      }
      if (options.method === 'POST' && url === '/api/scripts') {
        const fields = Object.fromEntries(new URLSearchParams(options.body));
        if (activityWarning && fields.activity_ack !== '1') return {
          ok: false, status: 409, text: async () => 'device_active: significant CPU usage; acknowledge before saving',
        };
        scripts = [{ id: Number(fields.id || 17), name: context.chordHexDecode(fields.name_hex),
          midi_enabled: fields.midi_enabled === '1', channel: Number(fields.channel),
          note: Number(fields.note), script_hex: fields.script_hex, hook_mode: true, event_mask: 128,
          config_write_allowed: fields.config_write_ack === '1',
          idle_velocity: Number(fields.idle_velocity), pressed_velocity: Number(fields.pressed_velocity) }];
      }
      if (options.method === 'DELETE') scripts = [];
      return { ok: true, json: async () => ({ scripts }), text: async () =>
        url === '/api/scripts/control' ? 'setting saved and config_change queued' : '' };
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
    setUnsaved: value => { unsaved = value; },
    setScripts: value => {
      scripts = JSON.parse(JSON.stringify(value));
      context.renderMidiChords(value);
    },
    holdControls: () => { waitForControl = new Promise(resolve => { releaseControl = resolve; }); },
    releaseControls: () => { releaseControl(); waitForControl = null; },
    setActivityWarning: value => { activityWarning = value; },
    setConfirmResult: value => { confirmResult = value; } };
}

test('activity saves require acknowledgment, cancellation does not save, dynamic hooks retry server warning', async () => {
  const f = fixture();
  const script = { name: 'Ducking', midi_enabled: false, channel: 0, note: 0,
    script_hex: f.context.chordHexEncode('hook("device_active","mic",function(e) end)') };
  f.setConfirmResult(false);
  await f.context.saveMidiChord('POST', script, f.get('btn-chord-save'));
  assert.equal(f.calls.length, 0);
  assert.equal(f.get('btn-chord-save').disabled, false);
  f.setConfirmResult(true);
  await f.context.saveMidiChord('POST', script, f.get('btn-chord-save'));
  assert.match(f.confirmations.at(-1)[0], /significant CPU usage/);
  assert.match(f.calls.at(-1).body, /activity_ack=1/);
  f.setActivityWarning(true);
  await f.context.saveMidiChord('POST', { ...script,
    script_hex: f.context.chordHexEncode('hook("device_".."active","mic",function(e) end)') },
    f.get('btn-chord-save'));
  assert.match(f.calls.at(-2).body, /activity_ack=0/);
  assert.match(f.calls.at(-1).body, /activity_ack=1/);
});

test('Scripts is callback-only with no sequence, shortcut recorder or subscription controls', () => {
  const html = fs.readFileSync(require.resolve('../../web/embedded/index.html'), 'utf8');
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

function scriptWithControls(f, ui) {
  return { id: 23, name: 'Custom script', midi_enabled: false, channel: 0, note: 60,
    event_mask: 128, script_hex: f.context.chordHexEncode('hook("run",function() end)'), ui };
}

function scriptControls(f) {
  return f.get('chord-list').children[0].children[2].children.map(field => field.children.at(-1));
}

test('per-script UI errors render as safe text without preventing editing', async () => {
  const f = fixture(), message = 'Invalid settings: <img src=x onerror=alert(1)> & "雪"';
  f.setScripts([{ ...scriptWithControls(f),
    ui_error_hex: f.context.chordHexEncode(message) }]);
  const row = f.get('chord-list').children[0];
  assert.ok(row.children[0].textContent.includes(`Script controls/settings load failed: ${message}`));
  await row.children[1].children[0].click();
  assert.equal(f.get('macro-name').value, 'Custom script');
  assert.equal(f.get('btn-chord-save').textContent, 'Save Changes');
  f.setScripts([scriptWithControls(f)]);
  assert.doesNotMatch(f.get('chord-list').children[0].children[0].textContent,
    /controls\/settings load failed/);
});

test('script UI renders only labeled native controls with text-safe labels and finite bounds', () => {
  const f = fixture(), label = '<img src=x onerror=alert(1)> & "雪"';
  f.setScripts([scriptWithControls(f, [
    { key: 'enabled', label, type: 'boolean', value: true },
    { key: 'level', label: 'Level', type: 'number', value: 1.5, min: -2, max: 10 },
    { key: 'title', label: 'Title', type: 'string', value: 'café 雪' },
    { key: 'go', label: 'Go', type: 'button' },
    { key: 'unbounded', label: 'Unbounded', type: 'number', value: 2, min: -Infinity, max: '99' },
    { key: 'evil', label, type: 'html', value: '<script>alert(1)</script>' },
  ])]);
  const fields = f.get('chord-list').children[0].children[2].children;
  const controls = scriptControls(f);
  assert.equal(fields.length, 5);
  assert.equal(fields[0].tagName, 'label');
  assert.equal(fields[0].children[0].textContent, label);
  assert.deepEqual(controls.map(input => input.tagName), ['input', 'input', 'input', 'button', 'input']);
  assert.deepEqual(controls.map(input => input.type), ['checkbox', 'number', 'text', 'button', 'number']);
  assert.equal(controls[0].checked, true);
  assert.equal(controls[1].value, '1.5');
  assert.equal(controls[1].step, 'any');
  assert.equal(controls[1].min, '-2');
  assert.equal(controls[1].max, '10');
  assert.equal(controls[2].value, 'café 雪');
  assert.equal(controls[2].maxLength, 127);
  assert.equal(controls[3].textContent, 'Go');
  assert.equal(controls[4].min, undefined);
  assert.equal(controls[4].max, undefined);
  f.context.renderMidiChords([scriptWithControls(f, undefined)]);
  assert.equal(f.get('chord-list').children[0].children.length, 2);
});

test('all script controls send hex-encoded typed values, keep saved values and preserve unsaved source', async () => {
  for (const [type, savedValue, value] of [
    ['boolean', false, true], ['number', 1, -2.5], ['string', 'Old', 'café &=+ 雪'], ['button', undefined, undefined],
  ]) {
    const f = fixture(), key = 'key &=+ 雪';
    f.setScripts([scriptWithControls(f, [{ key, label: 'Setting', type, value: savedValue }])]);
    await f.get('chord-list').children[0].children[1].children[0].click();
    f.get('chord-script').value = 'hook("run",function() log("unsaved source") end)';
    f.get('macro-name').value = 'Unsaved name';
    f.setUnsaved(true);
    const control = scriptControls(f)[0];
    if (type === 'boolean') control.checked = value;
    else if (type !== 'button') control.value = value;
    await control[type === 'button' ? 'click' : 'change']();
    assert.equal(f.calls.length, 1);
    assert.equal(f.calls[0].url, '/api/scripts/control');
    assert.equal(f.calls[0].method, 'POST');
    assert.equal(f.calls[0].headers['Content-Type'], 'text/plain');
    const fields = new URLSearchParams(f.calls[0].body);
    assert.equal(fields.get('id'), '23');
    assert.equal(f.context.chordHexDecode(fields.get('key_hex')), key);
    assert.equal(fields.has('value_hex'), type !== 'button');
    if (type !== 'button') assert.equal(f.context.chordHexDecode(fields.get('value_hex')), String(value));
    assert.equal(control.disabled, false);
    assert.equal(f.reads.length, 0);
    assert.equal(f.confirmations.length, 0);
    assert.match(f.notices.at(-1)[0], type === 'button' ? /button queued/ : /setting saved; config_change queued/);
    assert.match(f.notices.at(-1)[0], /not confirmation of completion/);
    assert.equal(f.get('macro-name').value, 'Unsaved name');
    assert.match(f.get('chord-script').value, /unsaved source/);
    if (type === 'boolean') assert.equal(control.checked, value);
    else if (type !== 'button') assert.equal(control.value, value);
    await f.get('btn-chord-save').click();
    assert.match(f.calls[1].body, /^id=23&/);
  }
});

test('failed script control saves restore saved values and buttons report queue failures', async () => {
  for (const type of ['boolean', 'number', 'string', 'button']) {
    const f = fixture(), value = { boolean: true, number: 1.5, string: 'saved 雪' }[type];
    f.setScripts([scriptWithControls(f, [{ key: 'setting', label: 'Setting', type, value }])]);
    const control = scriptControls(f)[0];
    control.checked = false;
    control.value = type === 'number' ? '2' : 'changed';
    f.fail();
    await control[type === 'button' ? 'click' : 'change']();
    if (type === 'boolean') assert.equal(control.checked, true);
    else if (type !== 'button') assert.equal(control.value, String(value));
    assert.equal(control.disabled, false);
    assert.equal(f.reads.length, 0);
    assert.match(f.notices.at(-1)[0], /Script control failed.*500.*save failed/);
    assert.equal(f.notices.at(-1)[1], 'warn');
  }
});

test('script controls reject nonfinite/out-of-bounds numbers and text above 127 UTF-8 bytes', async () => {
  const f = fixture();
  f.setScripts([scriptWithControls(f, [
    { key: 'number', label: 'Number', type: 'number', value: 5, min: 0, max: 10 },
    { key: 'text', label: 'Text', type: 'string', value: 'Saved' },
  ])]);
  const [number, text] = scriptControls(f);
  for (const value of ['', ' ', 'Infinity', 'NaN', '-1', '11']) {
    number.value = value;
    await number.change();
    assert.equal(number.value, '5');
    assert.match(f.notices.at(-1)[0], /finite number/);
  }
  text.value = '雪'.repeat(43);
  await text.change();
  assert.equal(text.value, 'Saved');
  assert.match(f.notices.at(-1)[0], /127 UTF-8 bytes/);
  assert.equal(f.calls.length, 0);
  text.value = '雪'.repeat(42) + 'x';
  await text.change();
  assert.equal(f.calls.length, 1);
  assert.equal(f.reads.length, 1);
  assert.equal(scriptControls(f)[1].value, '雪'.repeat(42) + 'x');
});

test('pending controls prevent duplicates and reloads; other unfinished controls are preserved', async () => {
  const f = fixture();
  f.setScripts([scriptWithControls(f, [
    { key: 'enabled', label: 'Enabled', type: 'boolean', value: false },
    { key: 'text', label: 'Text', type: 'string', value: 'Saved' },
  ])]);
  const [checkbox, text] = scriptControls(f);
  text.value = 'Unfinished';
  text.input();
  checkbox.checked = true;
  f.holdControls();
  const first = checkbox.change();
  assert.equal(checkbox.disabled, true);
  await checkbox.change();
  await f.context.loadMidiChords();
  assert.equal(f.calls.length, 1);
  assert.equal(f.reads.length, 0);
  f.releaseControls();
  await first;
  assert.equal(checkbox.disabled, false);
  assert.equal(checkbox.checked, true);
  assert.equal(text.value, 'Unfinished');
  assert.equal(f.reads.length, 0);
  await text.change();
  assert.equal(f.reads.length, 1);
  assert.equal(scriptControls(f)[0].checked, true);
  assert.equal(scriptControls(f)[1].value, 'Unfinished');
});

test('custom script_config, storage and ui do not request official write permissions', async () => {
  const f = fixture();
  const script = scriptWithControls(f);
  script.script_hex = f.context.chordHexEncode(
    'script_config.get("custom"); storage.read("private"); ui.boolean("enabled", "Enabled", true)');
  await f.context.saveMidiChord('POST', script, f.get('btn-chord-save'));
  assert.equal(f.confirmations.length, 0);
  assert.match(f.calls[0].body, /config_write_ack=0/);
  for (const name of ['config', 'settings', 'system', 'bluetooth']) {
    await f.context.saveMidiChord('POST', { ...script,
      script_hex: f.context.chordHexEncode(`-- ${name} access`) }, f.get('btn-chord-save'));
  }
  assert.equal(f.confirmations.length, 4);
  assert.match(f.confirmations[0][0], /official configuration.*restart.*Bluetooth/);
  for (const call of f.calls.slice(1)) assert.match(call.body, /config_write_ack=1/);
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
  Object.assign(f.context, { stopSystemPolling() {}, loadLightDim() {}, effectsManager: null,
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
