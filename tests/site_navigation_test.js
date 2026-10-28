const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');

const script = readFileSync(require.resolve('../site/downloads.js'), 'utf8');

function site(hash = '', status = 200, catalog = {}) {
  const elements = new Map();
  const listeners = {};
  const document = {
    title: '',
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, {
        hidden: false, attributes: {}, classes: new Set(), listeners: {},
        classList: {
          toggle(name, active) {
            if (active) elements.get(id).classes.add(name);
            else elements.get(id).classes.delete(name);
          },
        },
        setAttribute(name, value) { this.attributes[name] = value; },
        removeAttribute(name) { delete this.attributes[name]; },
        addEventListener(name, callback) { this.listeners[name] = callback; },
        replaceChildren() {},
      });
      return elements.get(id);
    },
  };
  const window = {
    location: { hash },
    addEventListener(name, callback) { listeners[name] = callback; },
  };
  vm.runInNewContext(script, {
    document, window,
    fetch: async () => ({ ok: status === 200, status, json: async () => catalog }),
  });
  return {
    document, elements,
    navigate(hash) {
      window.location.hash = hash;
      listeners.hashchange();
    },
  };
}

test('site opens on Home and recognizes a Download deep link on load or refresh', () => {
  for (const hash of ['', '#home', '#unknown', '#download']) {
    const ui = site(hash);
    const download = hash === '#download';
    assert.equal(ui.elements.get('page-home').hidden, download);
    assert.equal(ui.elements.get('page-download').hidden, !download);
    assert.equal(ui.document.title, download ? 'AudioX Downloads' : 'AudioX');
    assert.equal(ui.elements.get(download ? 'nav-download' : 'nav-home').attributes['aria-current'], 'page');
    assert.equal(ui.elements.get(download ? 'nav-home' : 'nav-download').attributes['aria-current'], undefined);
  }
});

test('hash navigation updates visibility, title and active links without resetting the download channel', async () => {
  const ui = site();
  await new Promise(resolve => setImmediate(resolve));
  ui.navigate('#download');
  ui.elements.get('channel-nightly').listeners.click();
  assert.equal(ui.elements.get('channel-nightly').attributes['aria-pressed'], 'true');
  for (const hash of ['#home', '#download', '#home', '#download']) {
    ui.navigate(hash);
    const download = hash === '#download';
    assert.equal(ui.elements.get('page-download').hidden, !download);
    assert.equal(ui.elements.get('nav-download').classes.has('active'), download);
    assert.equal(ui.document.title, download ? 'AudioX Downloads' : 'AudioX');
  }
  assert.equal(ui.elements.get('channel-nightly').attributes['aria-pressed'], 'true');
});

test('a failed download catalog does not prevent Home or Download navigation', async () => {
  const ui = site('', 503);
  await new Promise(resolve => setImmediate(resolve));
  assert.match(ui.elements.get('download-error').textContent, /HTTP 503/);
  assert.equal(ui.elements.get('page-home').hidden, false);
  ui.navigate('#download');
  assert.equal(ui.elements.get('page-download').hidden, false);
  assert.equal(ui.elements.get('download-error').hidden, false);
  ui.navigate('#home');
  assert.equal(ui.elements.get('page-download').hidden, true);
});

test('download metadata shows the firmware version when the catalog provides it', async () => {
  const ui = site('#download', 200, {
    stable: { tag: 'v1.9.2', date: '2026-10-01', release_url: 'https://example.invalid/v1.9.2', assets: {} },
    nightly: {
      tag: 'nightly-42', version: '1.10.0-beta.3.nightly+0123456789ab', date: '2026-10-27',
      release_url: 'https://example.invalid/nightly-42', assets: {},
    },
  });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(ui.elements.get('release-meta').textContent, 'v1.9.2 / 2026-10-01');
  ui.elements.get('channel-nightly').listeners.click();
  assert.equal(ui.elements.get('release-meta').textContent,
    '1.10.0-beta.3.nightly+0123456789ab / nightly-42 / 2026-10-27');
});
