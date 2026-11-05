const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');

const html = fs.readFileSync(require.resolve('../web/index.html'), 'utf8');
const css = fs.readFileSync(require.resolve('../web/styles.css'), 'utf8');

test('Scripts keeps its editor and save actions outside collapsed reference panels', () => {
  const scripts = html.split('data-page="scripts"')[1].split('<!-- MIDI page -->')[0];
  const references = [...scripts.matchAll(/<details class="reference-panel">([\s\S]*?)<\/details>/g)];
  assert.equal(references.length, 4);
  assert.ok(references.some(match => match[1].includes('Lua API quick reference and execution limits')));
  assert.ok(references.some(match => match[1].includes('Pad lighting')));
  for (const reference of references) {
    assert.doesNotMatch(reference[1], /id="chord-script"|id="btn-chord-save"|id="macro-config-write"/);
  }
  assert.match(scripts, /id="macro-config-write"/);
  assert.match(scripts, /id="chord-script"/);
  assert.match(scripts, /id="btn-chord-save"/);
});

test('shared controls include editor focus, native file-picker styling and global reduced motion', () => {
  assert.match(css, /textarea:focus-visible/);
  assert.match(css, /input\[type="file"\]::file-selector-button/);
  assert.match(css, /@media \(pointer: coarse\)/);
  assert.match(css, /@media \(prefers-reduced-motion: reduce\) \{\s*\.page, \.notification \{ animation: none; \}\s*\}/);
});

test('System resource meters use severity gradients without decorating static identity metrics', () => {
  const identityMetrics = html.slice(html.indexOf('id="sysinfo-version"'), html.indexOf('id="sysinfo-temperature"'));
  assert.doesNotMatch(identityMetrics, /resource-meter/);
  for (const kind of ['temperature', 'memory', 'storage', 'load', 'cpu']) {
    assert.match(html, new RegExp(`class="resource-meter resource-meter--${kind}"`));
  }
  assert.match(css, /linear-gradient\(180deg, var\(--meter-color-soft/);
  assert.match(css, /transition: width 350ms ease, background 350ms ease/);
  assert.match(css, /\.resource-meter\[data-state="critical"\]/);
  assert.match(html, /id="sysinfo-load"[\s\S]*id="sysinfo-cpu-utilization"/);
});

test('fan-curve editor is a large standalone dialog launched from temperature with Pi 4 preview warning', () => {
  const temperature = html.slice(html.indexOf('id="sysinfo-temperature"') - 450,
    html.indexOf('id="sysinfo-uptime"'));
  assert.match(temperature, /id="btn-pi5-fan-open"/);
  assert.match(html, /<dialog id="pi5-fan-dialog"/);
  assert.match(html, /id="pi5-fan-graph"/);
  assert.match(html, /Preview only on this Raspberry Pi 4[\s\S]*requires a Raspberry Pi 5/);
  assert.match(css, /\.fan-dialog\s*\{[^}]*width: min\(1120px/);
  assert.match(css, /\.fan-dialog-body\s*\{[^}]*grid-template-columns/);
  assert.match(html, /id="btn-pi5-fan-save"[^>]*type="button"/);
});

test('WebUI copy documents current operation without migration or roadmap narration', () => {
  const visible = html.replace(/<!--[\s\S]*?-->/g, '');
  assert.doesNotMatch(visible, /legacy scripts|migration wrappers|automatically converted|planned later/i);
  assert.match(visible, /Parameter changes save automatically; names require Save name/);
  assert.match(visible, /Back up first: file contents are not format-validated/);
  assert.match(visible, /Archives contain Bluetooth keys; store them securely/);
  assert.match(visible, /17,408 UTF-8 source bytes/);
});
