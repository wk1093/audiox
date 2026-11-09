const assert = require('node:assert/strict');
const { test } = require('node:test');
const { readFileSync } = require('node:fs');
const vm = require('node:vm');

const source = readFileSync(require.resolve('../../web/embedded/app.js'), 'utf8');
const helpers = source.slice(source.indexOf('async function loadLegacyConfigStatus()'),
  source.indexOf('async function loadConfig()'));

function setup({ accepted = false, failCheck = false, failDelete = false } = {}) {
  const notice = { hidden: true };
  const button = { disabled: false };
  const calls = [];
  const statuses = [];
  let count = 4;
  const context = vm.createContext({
    document: { getElementById(id) {
      if (id === 'legacy-config-notice') return notice;
      if (id === 'btn-delete-legacy-config') return button;
      throw new Error(id);
    } },
    AudioxUI: { async confirm(message, options) {
      assert.match(message, /Older firmware.*original settings/);
      assert.equal(options.label, 'Delete legacy data');
      return accepted;
    } },
    async apiFetch(path, options = {}) {
      assert.equal(path, '/api/config/legacy');
      const method = options.method || 'GET';
      calls.push(method);
      if (method === 'DELETE' && !failDelete) count = 0;
      const failed = method === 'DELETE' ? failDelete : failCheck;
      return { ok: !failed, status: failed ? 500 : 200,
        text: async () => 'storage error', json: async () => ({ legacy_entries: count }) };
    },
    setStatus: (...args) => statuses.push(args),
  });
  vm.runInContext(helpers, context);
  return { context, notice, button, calls, statuses };
}

test('legacy cleanup is visible only while retained data exists', async () => {
  const state = setup({ accepted: true });
  await state.context.loadLegacyConfigStatus();
  assert.equal(state.notice.hidden, false);
  await state.context.deleteLegacyConfig();
  assert.deepEqual(state.calls, ['GET', 'DELETE', 'GET']);
  assert.equal(state.notice.hidden, true);
  assert.equal(state.button.disabled, false);
});

test('canceling legacy cleanup never sends a delete request', async () => {
  const state = setup();
  await state.context.deleteLegacyConfig();
  assert.deepEqual(state.calls, []);
});

test('legacy check and deletion failures are reported without claiming success', async () => {
  const state = setup({ accepted: true, failDelete: true });
  await state.context.loadLegacyConfigStatus();
  await state.context.deleteLegacyConfig();
  assert.equal(state.notice.hidden, false);
  assert.equal(state.button.disabled, false);
  assert.match(state.statuses.at(-1)[0], /cleanup failed.*storage error/);
  const check = setup({ failCheck: true });
  await check.context.loadLegacyConfigStatus();
  assert.match(check.statuses.at(-1)[0], /check failed.*storage error/);
});
