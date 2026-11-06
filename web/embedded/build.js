#!/usr/bin/env node
'use strict';

// Compiles the browser UI from one set of pages, scripts and styles into:
//   device  - talks to the real AudioX HTTP API through api.js (staged into the rootfs)
//   demo    - replaces api.js with demo/*.js, a simulated device for the public website
// Usage: node build.js --target device|demo --out DIR [--no-minify]

const fs = require('node:fs');
const path = require('node:path');
const { stageVendor } = require('./stage-assets.js');

const sourceDir = __dirname;
const repoDir = path.resolve(sourceDir, '../..');
const tooling = new Set(['build.js', 'stage-assets.js']);
const demoScripts = ['demo/mock-data.js', 'demo/mock-api.js'];
const branding = ['logo.svg', 'logo.png', 'logo_small.svg', 'logo_small.png'];
const targets = new Set(['device', 'demo']);

function parseArgs(argv) {
  const options = { target: 'device', out: null, minify: true };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--target') options.target = argv[++i];
    else if (arg.startsWith('--target=')) options.target = arg.slice(9);
    else if (arg === '--out') options.out = argv[++i];
    else if (arg.startsWith('--out=')) options.out = arg.slice(6);
    else if (arg === '--no-minify') options.minify = false;
    else throw new Error(`Unknown argument: ${arg}`);
  }
  if (!targets.has(options.target)) throw new Error(`--target must be one of: ${[...targets].join(', ')}`);
  if (!options.out) throw new Error('--out DIR is required');
  return options;
}

function replaceOnce(text, search, replacement, what) {
  if (!text.includes(search)) throw new Error(`Demo build could not find ${what}`);
  return text.replace(search, replacement);
}

// GitHub Pages serves the demo below /audiox/demo/, so root-relative asset
// paths must become relative. /api/ paths are never requested by the demo.
function demoHtml(html) {
  html = replaceOnce(html, '<script src="/api.js"></script>',
    demoScripts.map((src) => `<script src="/${src}"></script>`).join('\n  '), 'the api.js script tag');
  html = html.replace(/<title>[^<]*<\/title>/, '<title>AudioX Live Demo</title>');
  html = html.replace(/(\s(?:src|href))="\/(?!\/)/g, '$1="./');
  return html.replace(/this\.src='\/(?!\/)/g, "this.src='./");
}

function demoCss(css) {
  return css.replace(/url\((['"]?)\/(?!\/)/g, 'url($1./');
}

function scriptSources(html) {
  return [...html.matchAll(/<script src="\/([^"]+)"><\/script>/g)].map((match) => match[1])
    .filter((src) => !src.startsWith('vendor/'));
}

function minifier(enabled) {
  if (!enabled) return (code) => code;
  const esbuild = require('esbuild');
  // Classic scripts share globals across files; esbuild keeps top-level names
  // when no output format is set, so cross-file references remain valid.
  return (code, loader, file) => {
    try {
      return esbuild.transformSync(code, { loader, minify: true, legalComments: 'none' }).code;
    } catch (error) {
      throw new Error(`Failed to compile ${file}: ${error.message}`);
    }
  };
}

function guardOutput(out) {
  const forbidden = [path.parse(out).root, repoDir, sourceDir, path.join(repoDir, 'web')];
  if (forbidden.includes(out) || sourceDir.startsWith(`${out}${path.sep}`)) {
    throw new Error(`Refusing to replace ${out}`);
  }
}

function build({ target, out, minify = true }) {
  out = path.resolve(out);
  guardOutput(out);
  const demo = target === 'demo';
  const compile = minifier(minify);
  const write = (name, contents) => {
    const file = path.join(out, name);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, contents);
  };

  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });

  const sources = fs.readdirSync(sourceDir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && /\.(?:html|css|js)$/.test(entry.name) && !tooling.has(entry.name))
    .map((entry) => entry.name)
    .filter((name) => !(demo && name === 'api.js'));
  if (demo) sources.push(...demoScripts);

  let html = fs.readFileSync(path.join(sourceDir, 'index.html'), 'utf8');
  if (demo) html = demoHtml(html);
  for (const src of scriptSources(html.replace(/src="\.\//g, 'src="/'))) {
    if (!sources.includes(src)) throw new Error(`index.html references missing script ${src}`);
  }

  for (const name of sources) {
    let contents = fs.readFileSync(path.join(sourceDir, name), 'utf8');
    if (name === 'index.html') {
      write(name, html);
      continue;
    }
    if (name.endsWith('.css')) {
      if (demo) contents = demoCss(contents);
      write(name, compile(contents, 'css', name));
    } else {
      write(name, compile(contents, 'js', name));
    }
  }

  stageVendor(path.join(out, 'vendor'));
  for (const name of branding) {
    fs.copyFileSync(path.join(repoDir, 'assets/branding', name), path.join(out, name));
  }
  return out;
}

if (require.main === module) {
  try {
    const options = parseArgs(process.argv.slice(2));
    const out = build(options);
    console.log(`Built ${options.target} web UI${options.minify ? '' : ' (unminified)'} in ${out}`);
  } catch (error) {
    console.error(`web build failed: ${error.message}`);
    process.exit(1);
  }
}

module.exports = { build, demoHtml, demoCss };
