const assert = require('node:assert/strict');
const { test } = require('node:test');
const { create } = require('../../web/embedded/desktop.js');

test('desktop page follows connection and displays detected capabilities as text', () => {
  const tab = { hidden: true };
  const host = { textContent: '' };
  const os = { textContent: '' };
  const capabilities = { textContent: '' };
  const media = { textContent: '' };
  let event, connection, disconnected = 0, unsubscribed = 0;
  const api = {
    subscribe(topic, listener) {
      assert.equal(topic, 'desktop.status');
      event = listener;
      return () => { unsubscribed++; };
    },
    onConnectionChange(listener) {
      connection = listener;
      return () => { unsubscribed++; };
    },
  };
  const dispose = create({ api, tab, host, os, capabilities, media, onDisconnect: () => { disconnected++; } });
  event({ connected: true, hello: { hostname: '<host>', os: 'linux', capabilities: ['pipewire', 'default_sink'], media: 'playing' } });
  assert.equal(tab.hidden, false);
  assert.equal(host.textContent, '<host>');
  assert.equal(os.textContent, 'linux');
  assert.equal(capabilities.textContent, 'pipewire, default_sink');
  assert.equal(media.textContent, 'Playing');
  event({ connected: true, hello: { hostname: 'other', os: 'windows', capabilities: [] } });
  assert.equal(capabilities.textContent, 'None detected');
  assert.equal(media.textContent, 'Unavailable');
  connection(false);
  assert.equal(tab.hidden, true);
  assert.equal(host.textContent, '');
  assert.equal(media.textContent, '');
  assert.equal(disconnected, 1);
  event({ connected: true, hello: { hostname: 'bad', os: 'linux', capabilities: [null] } });
  assert.equal(tab.hidden, true);
  dispose();
  assert.equal(unsubscribed, 2);
});
