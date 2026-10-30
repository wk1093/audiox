const assert = require('node:assert/strict');
const { readdirSync, readFileSync, existsSync, statSync } = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');
const docs = path.join(root, 'docs');

function markdownFiles(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    if (entry.name.startsWith('.') || entry.name === 'node_modules') return [];
    const file = path.join(directory, entry.name);
    return entry.isDirectory() ? markdownFiles(file) : entry.name.endsWith('.md') ? [file] : [];
  });
}

function resolvePage(link, from = docs) {
  const target = link.startsWith('/') ? path.join(docs, link) : path.resolve(from, link);
  const candidates = [target, `${target}.md`, path.join(target, 'index.md')];
  return candidates.find(file => existsSync(file) && statSync(file).isFile());
}

test('all local documentation links and navigation targets resolve', () => {
  for (const file of markdownFiles(docs)) {
    const markdown = readFileSync(file, 'utf8').replace(/```[\s\S]*?```/g, '');
    for (const match of markdown.matchAll(/!?\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g)) {
      const link = match[1].split(/[?#]/, 1)[0];
      if (!link || /^[a-z][a-z0-9+.-]*:/i.test(link)) continue;
      assert.ok(resolvePage(link, path.dirname(file)), `${path.relative(root, file)}: ${match[1]}`);
    }
  }
  const config = readFileSync(path.join(docs, '.vitepress/config.mts'), 'utf8');
  for (const match of config.matchAll(/link:\s*'(\/[^']*)'/g))
    assert.ok(resolvePage(match[1]), `Navigation target ${match[1]}`);
  assert.match(config, /themeConfig:\s*\{\s*siteTitle:[^\n]+\n\s*search:\s*\{\s*provider:\s*'local'/);
});

test('each WebUI page has a device guide', () => {
  const html = readFileSync(path.join(root, 'web/index.html'), 'utf8');
  const pages = [...html.matchAll(/data-page="([^"]+)"/g)].map(match => match[1]);
  assert.ok(pages.length >= 9);
  for (const page of pages) {
    const file = path.join(docs, 'guide', `${page}.md`);
    assert.ok(existsSync(file), `Missing guide for ${page}`);
    assert.ok(readFileSync(file, 'utf8').length > 1000, `Incomplete guide for ${page}`);
  }
});

test('public site links to documentation instead of README setup instructions', () => {
  const html = readFileSync(path.join(root, 'site/index.html'), 'utf8');
  assert.match(html, /href="\.\/docs\/">Documentation<\/a>/);
  assert.match(html, /href="\.\/docs\/guide\/getting-started">Setup instructions<\/a>/);
  assert.doesNotMatch(html, /href="[^"]*#readme"/i);
  assert.ok(resolvePage('/guide/getting-started'));
  assert.ok(resolvePage('/lua/'), 'Existing public Lua landing link must work');
});

test('device WebUI provides a persistent beginner help link without leaving the controls', () => {
  const html = readFileSync(path.join(root, 'web/index.html'), 'utf8');
  const header = html.match(/<header class="app-header">([\s\S]*?)<\/header>/)?.[1];
  assert.ok(header, 'Device header must exist');
  const help = header.match(/<a class="docs-link"([^>]+)>([^<]+)<\/a>/);
  assert.ok(help, 'Help link must be visible in the shared header, not a single page');
  assert.match(help[1], /href="https:\/\/wklab\.net\/audiox\/docs\/guide\/web-interface"/);
  assert.match(help[1], /target="_blank"/);
  assert.match(help[1], /rel="noopener noreferrer"/);
  assert.match(help[1], /title="[^"]*requires internet[^"]*"/);
  assert.equal(help[2], 'Need help? Start here');
  assert.ok(resolvePage('/guide/web-interface'));
});

test('README stays concise and its local documentation links resolve', () => {
  const readme = readFileSync(path.join(root, 'README.md'), 'utf8');
  assert.ok(readme.split('\n').length <= 120, 'Keep detailed instructions in the guides');
  for (const match of readme.matchAll(/\[[^\]]*\]\((docs\/[^)\s]+)\)/g))
    assert.ok(resolvePage(match[1], root), `README target ${match[1]}`);
});
