const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { test } = require('node:test');
const vm = require('node:vm');

const script = readFileSync(require.resolve('../../web/public/downloads.js'), 'utf8');
const releasesScript = readFileSync(require.resolve('../../web/embedded/releases.js'), 'utf8');

function site(hash = '', status = 200, catalog = {}) {
  const elements = new Map();
  const listeners = {};
  const document = {
    title: '',
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, {
        hidden: false, attributes: {}, classes: new Set(), listeners: {}, children: [],
        content: {
          cloneNode() {
            const fields = new Map();
            return { querySelector(selector) {
              if (!fields.has(selector)) fields.set(selector, {});
              return fields.get(selector);
            } };
          },
        },
        classList: {
          toggle(name, active) {
            if (active) elements.get(id).classes.add(name);
            else elements.get(id).classes.delete(name);
          },
        },
        setAttribute(name, value) { this.attributes[name] = value; },
        removeAttribute(name) { delete this.attributes[name]; },
        addEventListener(name, callback) { this.listeners[name] = callback; },
        replaceChildren() { this.children = []; },
        appendChild(child) { this.children.push(child); },
      });
      return elements.get(id);
    },
  };
  const window = {
    location: { hash },
    addEventListener(name, callback) { listeners[name] = callback; },
  };
  const context = vm.createContext({
    document, window,
    fetch: async () => ({ ok: status === 200, status, json: async () => catalog }),
  });
  vm.runInContext(releasesScript, context);
  vm.runInContext(script, context);
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
      tag: 'v1.10.0-beta.3.nightly+42', version: '1.10.0-beta.3.nightly+0123456789ab', date: '2026-10-27',
      release_url: 'https://example.invalid/v1.10.0-beta.3.nightly+42', assets: {},
    },
  });

  await new Promise(resolve => setImmediate(resolve));
  assert.equal(ui.elements.get('release-meta').textContent, 'v1.9.2 / 2026-10-01');
  ui.elements.get('channel-nightly').listeners.click();
  assert.equal(ui.elements.get('release-meta').textContent,
    '1.10.0-beta.3.nightly+0123456789ab / v1.10.0-beta.3.nightly+42 / 2026-10-27');
});

test('nightly tab links to stable when nightly is older or absent, but retains newer nightlies', async () => {
  function release(tag) {
    return { tag, release_url: `https://example.invalid/${tag}`, assets: Object.fromEntries(
      ['pi4', 'pi5'].map(board => [board, {
        url: `updates/${tag}/audiox-update-${board}.axu`, size: 1024, sha256: 'axu-hash',
        checksum_url: `https://example.invalid/${tag}/${board}.axu.sha256`,
        image: { url: `https://example.invalid/${tag}/${board}.img.xz`, size: 2048, sha256: 'image-hash' },
      }])) };
  }
  const stable = release('v1.14.1');
  for (const tag of [null, 'v1.12.3-beta.1.nightly+42', 'v1.14.1-beta.1.nightly+42', 'v1.15.0-beta.1.nightly+42']) {
    const nightly = tag && release(tag);
    const ui = site('#download', 200, { stable, nightly });
    await new Promise(resolve => setImmediate(resolve));
    ui.elements.get('channel-nightly').listeners.click();
    const expected = tag?.startsWith('v1.15') ? nightly : stable;
    assert.equal(ui.elements.get('release-link').href, expected.release_url);
    assert.equal(ui.elements.get('release-meta').textContent, `${expected.tag} / date unavailable`);
    assert.equal(ui.elements.get('channel-nightly').attributes['aria-pressed'], 'true');
    assert.match(ui.elements.get('channel-description').textContent,
      expected === stable ? /latest stable release/ : /experimental/);
    const cards = ui.elements.get('download-cards').children;
    assert.equal(cards.length, 2);
    for (const [index, board] of ['pi4', 'pi5'].entries()) {
      assert.equal(cards[index].querySelector('.axu-link').href, expected.assets[board].url);
      assert.equal(cards[index].querySelector('.image-link').href, expected.assets[board].image.url);
      assert.equal(cards[index].querySelector('.axu-checksum-link').href, expected.assets[board].checksum_url);
    }
    ui.elements.get('channel-stable').listeners.click();
    assert.equal(ui.elements.get('release-link').href, stable.release_url);
  }
});

test('nightly tab works before the first stable release', async () => {
  const nightly = { tag: 'v1.0.0-beta.1.nightly+42', release_url: 'https://example.invalid/nightly', assets: {} };
  const ui = site('#download', 200, { nightly });
  await new Promise(resolve => setImmediate(resolve));
  ui.elements.get('channel-nightly').listeners.click();
  assert.equal(ui.elements.get('release-link').href, nightly.release_url);
});

test('website and device load shared release selection before their download scripts', () => {
  for (const [file, script] of [['../../web/public/index.html', 'downloads.js'], ['../../web/embedded/index.html', 'updates.js']]) {
    const html = readFileSync(require.resolve(file), 'utf8');
    assert.ok(html.indexOf('src="./releases.js"') >= 0 || html.indexOf('src="/releases.js"') >= 0);
    assert.ok(html.indexOf('releases.js') < html.indexOf(script));
  }
  const workflow = readFileSync(require.resolve('../../.github/workflows/update-site.yml'), 'utf8');
  assert.match(workflow, /- 'web\/embedded\/releases\.js'/);
});
