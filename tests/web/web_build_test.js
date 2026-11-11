'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { build } = require('../../web/embedded/build.js');

const embedded = path.resolve(__dirname, '../../web/embedded');
const read = (dir, name) => fs.readFileSync(path.join(dir, name), 'utf8');

test('device and demo builds bundle and minify all text resources without changing page content', async (t) => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-web-build-'));
  t.after(() => fs.rmSync(work, { recursive: true, force: true }));
  const sourceHtml = read(embedded, 'index.html');
  const preservedContent = (html) => [...html.matchAll(/<(pre|textarea)\b[^>]*>[\s\S]*?<\/\1>/g)]
    .map(([element]) => element.slice(element.indexOf('>') + 1, element.lastIndexOf('</')));
  const sourceScripts = [...sourceHtml.matchAll(/<script src="\/([^"]+)"><\/script>/g)]
    .map(([, name]) => name);

  for (const target of ['device', 'demo']) {
    const debug = path.join(work, `${target}-debug`);
    const prod = path.join(work, target);
    await build({ target, out: debug, minify: false });
    await build({ target, out: prod });
    const html = read(prod, 'index.html');
    const js = read(prod, 'app.js');
    const css = read(prod, 'styles.css');
    const prefix = target === 'demo' ? './' : '/';

    assert.deepEqual([...html.matchAll(/<script src="([^"]+)"><\/script>/g)].map(([, src]) => src),
      [`${prefix}app.js`]);
    assert.deepEqual([...html.matchAll(/<link rel="stylesheet" href="([^"]+)">/g)].map(([, href]) => href),
      [`${prefix}styles.css`]);
    assert.deepEqual(preservedContent(html), preservedContent(sourceHtml));
    assert.doesNotMatch(html, /<!--/);
    assert.match(read(debug, 'index.html'), /<!--/);
    assert.match(read(debug, 'app.js'), /\/\* app\/state\.js \*\//);
    assert.match(read(debug, 'styles.css'), /\/\* vendor\/litegraph\.css \*\//);
    assert.doesNotThrow(() => new vm.Script(js, { filename: `${target}/app.js` }));
    assert.doesNotThrow(() => new vm.Script(read(debug, 'app.js')));

    const debugJs = read(debug, 'app.js');
    const expectedScripts = target === 'demo'
      ? sourceScripts.flatMap(name => name === 'api.js' ? ['demo/mock-data.js', 'demo/mock-api.js'] : [name])
      : sourceScripts;
    let previous = -1;
    for (const name of expectedScripts) {
      const position = debugJs.indexOf(`/* ${name} */`);
      assert.ok(position > previous, `${target}: ${name} must appear once in source order`);
      assert.equal(debugJs.indexOf(`/* ${name} */`, position + 1), -1);
      previous = position;
    }
    if (target === 'demo') {
      assert.doesNotMatch(html, /\s(?:src|href)="\/(?!\/)/);
      assert.doesNotMatch(css, /url\(["']?\/(?!\/)/);
      assert.match(html, /<title>AudioX Live Demo<\/title>/);
      assert.match(debugJs, /\/\* demo\/mock-api\.js \*\//);
      assert.doesNotMatch(debugJs, /\/\* api\.js \*\//);
    } else {
      assert.match(debugJs, /\/\* api\.js \*\//);
      assert.doesNotMatch(debugJs, /\/\* demo\/mock-api\.js \*\//);
    }

    for (const [name, maximumRatio] of [
      ['app.js', 0.6], ['styles.css', 0.85], ['index.html', 0.9],
      ['logo.svg', 0.65], ['logo_small.svg', 0.65],
    ]) {
      assert.ok(fs.statSync(path.join(prod, name)).size < fs.statSync(path.join(debug, name)).size * maximumRatio,
        `${target}: ${name} must be substantially smaller in production`);
    }
    for (const name of ['logo.png', 'logo_small.png', 'vendor/litegraph-LICENSE.txt',
      'vendor/space-grotesk-LICENSE.txt', ...[400, 500, 600, 700].map(weight => `vendor/space-grotesk-${weight}.woff2`)]) {
      assert.deepEqual(fs.readFileSync(path.join(prod, name)), fs.readFileSync(path.join(debug, name)));
    }
    for (const name of ['app', 'api.js', 'demo', 'vendor/litegraph.js', 'vendor/litegraph.css',
      'scenes.css', 'build.js', 'stage-assets.js', 'node_modules', 'package.json']) {
      assert.equal(fs.existsSync(path.join(prod, name)), false, `${name} should not be shipped separately`);
    }
    for (const [, url] of html.matchAll(/\s(?:src|href)="((?:\/|\.\/)[^"]+)"/g)) {
      assert.ok(fs.existsSync(path.join(prod, url.replace(/^(?:\/|\.\/)/, ''))), `${url} must exist`);
    }
    for (const [, url] of css.matchAll(/url\(["']?((?:\/|\.\/)[^"')]+)["']?\)/g)) {
      assert.ok(fs.existsSync(path.join(prod, url.replace(/^(?:\/|\.\/)/, ''))), `${url} must exist`);
    }
  }
});

test('custom index builds include the application features behind the original app.js entry', async (t) => {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-web-index-'));
  t.after(() => fs.rmSync(work, { recursive: true, force: true }));
  const index = path.join(work, 'custom.html');
  fs.writeFileSync(index, read(embedded, 'index.html')
    .replace(/<script src="\/app\/[^"]+"><\/script>/g, '')
    .replace(/<title>[^<]*<\/title>/, '<title>Custom AudioX</title>'));
  const out = path.join(work, 'output');
  await build({ target: 'device', out, index, minify: false });
  assert.match(read(out, 'index.html'), /<title>Custom AudioX<\/title>/);
  assert.match(read(out, 'app.js'), /\/\* app\/bluetooth\.js \*\//);
  assert.match(read(out, 'app.js'), /\/\* app\/state\.js \*\//);
  assert.doesNotThrow(() => new vm.Script(read(out, 'app.js')));
});

test('invalid build targets fail explicitly', async () => {
  await assert.rejects(build({ target: 'typo', out: '/unused' }), /Unknown web build target/);
});
