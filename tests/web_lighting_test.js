const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

function fixture(initiallyDimmed = false) {
  const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');
  const script = source.slice(source.indexOf("const lightDimButton ="),
    source.indexOf('async function loadLightConfig()'));
  const button = {
    disabled: true, textContent: 'Dim lights', pressed: 'false',
    setAttribute(name, value) { assert.equal(name, 'aria-pressed'); this.pressed = value; },
    getAttribute(name) { assert.equal(name, 'aria-pressed'); return this.pressed; },
    addEventListener(name, callback) { assert.equal(name, 'click'); this.click = callback; },
  };
  let dimmed = initiallyDimmed;
  let failure = false;
  const calls = [], notices = [];
  const context = vm.createContext({
    document: { getElementById: (id) => {
      assert.equal(id, 'btn-light-dim');
      return button;
    } },
    AbortSignal,
    AudioxUI: { resolve() {} },
    setStatus: (...args) => notices.push(args),
    fetch: async (url, options) => {
      assert.equal(url, '/api/midi/light_dim');
      calls.push(options);
      if (failure) return { ok: false, status: 503, text: async () => 'MIDI unavailable' };
      if (options.method === 'POST') dimmed = options.body === 'dimmed=1';
      return { ok: true, json: async () => ({ ok: true, dimmed }) };
    },
  });
  vm.runInContext(script, context);
  return {
    button, calls, notices, load: () => context.loadLightDim(),
    fail: () => { failure = true; },
    reboot: () => { dimmed = false; },
  };
}

test('dim and restore use runtime API without saving settings; reload reflects device state', async () => {
  const f = fixture();
  await f.load();
  assert.equal(f.button.disabled, false);
  assert.equal(f.button.pressed, 'false');
  await f.button.click();
  assert.equal(f.button.textContent, 'Restore lights');
  assert.equal(f.button.pressed, 'true');
  assert.equal(f.calls.at(-1).body, 'dimmed=1');
  await f.load();
  assert.equal(f.calls.at(-1).cache, 'no-store');
  assert.equal(f.button.pressed, 'true');
  await f.button.click();
  assert.equal(f.button.textContent, 'Dim lights');
  assert.equal(f.calls.at(-1).body, 'dimmed=0');
  assert.match(f.notices.at(-1)[0], /restored/);
});

test('already dimmed device loads correctly and reboot restores the normal button state', async () => {
  const f = fixture(true);
  await f.load();
  assert.equal(f.button.textContent, 'Restore lights');
  f.reboot();
  await f.load();
  assert.equal(f.button.textContent, 'Dim lights');
  assert.equal(f.button.pressed, 'false');
});

test('failed changes preserve state, show errors, and permit retry; failed loads disable control', async () => {
  const f = fixture();
  await f.load();
  f.fail();
  await f.button.click();
  assert.equal(f.button.pressed, 'false');
  assert.equal(f.button.disabled, false);
  assert.match(f.notices.at(-1)[0], /change failed.*503/);
  assert.equal(f.notices.at(-1)[1], 'warn');
  await f.load();
  assert.equal(f.button.disabled, true);
  assert.match(f.notices.at(-1)[0], /load failed.*503/);
});

test('pending requests disable controls and ignore duplicate clicks', async () => {
  const f = fixture();
  await f.load();
  const changing = f.button.click();
  assert.equal(f.button.disabled, true);
  await f.button.click();
  await changing;
  assert.equal(f.calls.filter((call) => call.method === 'POST').length, 1);
  assert.equal(f.button.disabled, false);
});
