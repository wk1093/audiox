const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');

const source = readFileSync(require.resolve('../../web/embedded/app.js'), 'utf8');

test('USB reload has destructive styling and a persistent accessible instability warning', () => {
  const html = readFileSync(require.resolve('../../web/embedded/index.html'), 'utf8');
  assert.match(html, /id="btn-reload" class="warn" aria-describedby="usb-reload-warning"/);
  assert.match(html, /id="usb-reload-warning"[^>]*>.*unresponsive until rebooted.*Rebooting.*recommended/);
});

test('USB reload warns before execution; cancel does nothing and acceptance preserves reload behavior', async () => {
  for (const accepted of [false, true, 'alternate']) {
    let click;
    const calls = [];
    const context = apiContext({
      document: { getElementById(id) {
        assert.equal(id, 'btn-reload');
        return { addEventListener(event, callback) {
          assert.equal(event, 'click');
          click = callback;
        } };
      } },
      AudioxUI: { async confirm(message, options) {
        assert.match(message, /instability.*unresponsive until rebooted/);
        assert.match(message, /Restart Device instead \(recommended\)/);
        assert.equal(options.label, 'Reload anyway');
        assert.equal(options.alternateLabel, 'Reboot Instead');
        calls.push('confirm');
        return accepted;
      } },
      runLoadingTask: async (_title, action) => { calls.push('loading'); await action(); },
      reloadConfig: async () => { calls.push('reload'); return true; },
      loadRoutingThings: async () => calls.push('things'),
      loadRoutingFile: async () => calls.push('routing'),
      runSystemAction: async (path, label) => {
        assert.equal(path, '/api/system/restart');
        assert.equal(label, 'restart');
        calls.push('restart');
      },
    });
    vm.runInContext(source.slice(source.indexOf("document.getElementById('btn-reload').addEventListener"),
      source.indexOf("document.getElementById('btn-route-save').addEventListener")), context);
    await click();
    assert.deepEqual(calls, accepted === 'alternate' ? ['confirm', 'restart'] : accepted
      ? ['confirm', 'loading', 'reload', 'things', 'routing'] : ['confirm']);
  }
});
