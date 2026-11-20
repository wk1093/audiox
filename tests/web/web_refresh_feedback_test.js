const assert = require('node:assert/strict');
const fs = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');
const { apiContext } = require('./support/api-context.js');

function source(file, start, end) {
  const text = fs.readFileSync(`web/embedded/app/${file}.js`, 'utf8');
  return text.slice(text.indexOf(start), text.indexOf(end));
}

test('MIDI load failures preserve existing settings and successful retries clear scoped warnings', async () => {
  const warnings = [], resolved = [], unsaved = [];
  const fields = new Map();
  const state = { midiMappingsByNote: { 60: 'old.wav' }, midiMappingsBySfx: { 'old.wav': 60 } };
  let response = { ok: false, status: 503, text: async () => '{"error":"Unavailable"}' };
  const context = apiContext({
    fetch: async () => response,
    state,
    document: { getElementById(id) {
      if (!fields.has(id)) fields.set(id, { value: '12' });
      return fields.get(id);
    } },
    renderSoundboardGrid() {},
    setStatus: (...args) => warnings.push(args),
    AudioxUI: { resolve: (key) => resolved.push(key), setUnsaved: (...args) => unsaved.push(args) },
  });
  vm.runInContext(source('midi', 'async function loadMappings()', 'async function loadMidiActions()'), context);
  vm.runInContext(source('midi', 'async function loadLightConfig()', 'async function saveLightConfig()'), context);
  await context.loadMappings();
  context.document.getElementById('light_channel');
  response = { ok: true, text: async () => '{"channel":2}' };
  await context.loadLightConfig();
  assert.equal(state.midiMappingsByNote[60], 'old.wav');
  assert.equal(fields.get('light_channel').value, '12');
  assert.deepEqual(unsaved, []);
  assert.deepEqual(warnings.map(([, tone, key]) => [tone, key]),
    [['warn', 'midi-mappings-load'], ['warn', 'midi-light-config-load']]);
  response = { ok: true, text: async () => JSON.stringify({
    mappings: [{ note: 61, sfx: 'new.wav' }], channel: 2, mapped_vel: 3, stop_all_vel: 4, playing_vel: 5,
  }) };
  await context.loadMappings();
  await context.loadLightConfig();
  assert.equal(state.midiMappingsByNote[61], 'new.wav');
  assert.equal(state.midiMappingsByNote[60], undefined);
  assert.equal(fields.get('light_channel').value, '2');
  assert.deepEqual(resolved, ['midi-mappings-load', 'midi-light-config-load']);
});

test('Effects refresh warns once after three failures and clears only after a real refresh', async () => {
  const warnings = [], resolved = [];
  const state = { activeTab: 'effects', effectsPollFailures: 0 };
  let result = 'fail';
  const context = vm.createContext({
    state, document: { visibilityState: 'visible' },
    effectsManager: { async loadEffects() {
      if (result === 'fail') throw new Error('device unavailable');
      return result;
    } },
    console: { warn() {} },
    AudioxUI: { resolve: (key) => resolved.push(key) },
    setStatus: (...args) => warnings.push(args),
  });
  vm.runInContext(source('routing-model', 'async function pollEffectsIfNeeded()', 'async function loadRoutingFile()'), context);
  for (let i = 0; i < 2; i++) await context.pollEffectsIfNeeded();
  assert.equal(warnings.length, 0);
  await context.pollEffectsIfNeeded();
  await context.pollEffectsIfNeeded();
  assert.equal(warnings.length, 1);
  assert.match(warnings[0][0], /out of date/);
  assert.equal(warnings[0][2], 'effects-stale');
  result = false;
  await context.pollEffectsIfNeeded();
  assert.deepEqual(resolved, []);
  result = true;
  await context.pollEffectsIfNeeded();
  assert.deepEqual(resolved, ['effects-stale']);
  result = 'fail';
  await context.pollEffectsIfNeeded();
  assert.equal(warnings.length, 1);
});
