#!/usr/bin/env node
'use strict';

// Compiles the browser UI from one set of pages, scripts and styles into:
//   device  - talks to the real AudioX HTTP API through api.js (staged into the rootfs)
//   demo    - replaces api.js with demo/*.js, a simulated device for the public website
// Usage: node build.js --target device|demo --out DIR [--no-minify] [--index FILE]

const fs = require('node:fs');
const path = require('node:path');
const { stageVendor } = require('./stage-assets.js');

const sourceDir = __dirname;
const repoDir = path.resolve(sourceDir, '../..');
const demoScripts = ['demo/mock-data.js', 'demo/mock-api.js'];
const branding = ['logo.svg', 'logo.png', 'logo_small.svg', 'logo_small.png'];
const targets = new Set(['device', 'demo']);

function parseArgs(argv) {
  const options = { target: 'device', out: null, minify: true, index: path.join(sourceDir, 'index.html') };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--target') options.target = argv[++i];
    else if (arg.startsWith('--target=')) options.target = arg.slice(9);
    else if (arg === '--out') options.out = argv[++i];
    else if (arg.startsWith('--out=')) options.out = arg.slice(6);
    else if (arg === '--index') options.index = argv[++i];
    else if (arg.startsWith('--index=')) options.index = arg.slice(8);
    else if (arg === '--no-minify') options.minify = false;
    else throw new Error(`Unknown argument: ${arg}`);
  }
  if (!targets.has(options.target)) throw new Error(`--target must be one of: ${[...targets].join(', ')}`);
  if (!options.out) throw new Error('--out DIR is required');
  if (!options.index) throw new Error('--index FILE is required');
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

function minifier(enabled) {
  if (!enabled) return (code) => code;
  const esbuild = require('esbuild');
  // Keep classic-script globals available to inline handlers and other scripts.
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

async function build({ target, out, minify = true, index = path.join(sourceDir, 'index.html') }) {
  if (!targets.has(target)) throw new Error(`Unknown web build target: ${target}`);
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

  stageVendor(path.join(out, 'vendor'));
  const canonicalHtml = fs.readFileSync(path.join(sourceDir, 'index.html'), 'utf8');
  let html = fs.readFileSync(index, 'utf8');
  if (path.resolve(index) !== path.join(sourceDir, 'index.html')) {
    // Older index overrides know only app.js, not its new source files.
    const appScripts = [...canonicalHtml.matchAll(/<script src="\/app\/[^"]+"><\/script>/g)]
      .map(([tag]) => tag).join('\n');
    html = html.replace(/<script src="\/app\/[^"]+"><\/script>/g, '');
    html = html.replace('<script src="/app.js"></script>', `${appScripts}\n<script src="/app.js"></script>`);
  }
  if (demo) html = demoHtml(html);
  const bundle = (pattern, name, loader) => {
    const matches = [...html.matchAll(pattern)];
    if (!matches.length) throw new Error(`index.html has no ${loader} resources`);
    const chunks = matches.map(([, url]) => {
      const file = url.replace(/^(?:\/|\.\/)/, '');
      const base = file.startsWith('vendor/') ? out : sourceDir;
      const source = path.resolve(base, file);
      if (!source.startsWith(`${base}${path.sep}`)) throw new Error(`Invalid asset path: ${url}`);
      let contents = fs.readFileSync(source, 'utf8');
      if (demo && loader === 'css') contents = demoCss(contents);
      return minify ? contents : `/* ${file} */\n${contents}`;
    });
    write(name, compile(chunks.join(loader === 'js' ? '\n;\n' : '\n'), loader, name));
    const url = `${demo ? './' : '/'}${name}`;
    const tag = loader === 'js' ? `<script src="${url}"></script>` : `<link rel="stylesheet" href="${url}">`;
    let first = true;
    html = html.replace(pattern, () => {
      if (!first) return '';
      first = false;
      return tag;
    });
  };
  bundle(/<script src="([^"]+)"><\/script>/g, 'app.js', 'js');
  bundle(/<link rel="stylesheet" href="([^"]+)">/g, 'styles.css', 'css');
  if (minify) {
    const { minify: minifyHtml } = require('html-minifier-terser');
    html = await minifyHtml(html, {
      collapseWhitespace: true,
      conservativeCollapse: true,
      removeComments: true,
      minifyJS: true,
      minifyCSS: true,
    });
  }
  write('index.html', html);

  // The graph library is now inside the bundles; keep fonts and license files.
  for (const name of ['litegraph.js', 'litegraph.css']) {
    fs.unlinkSync(path.join(out, 'vendor', name));
  }
  for (const name of branding) {
    const source = path.join(repoDir, 'assets/branding', name);
    if (minify && name.endsWith('.svg')) {
      const { optimize } = require('svgo');
      write(name, optimize(fs.readFileSync(source, 'utf8'), { path: source, multipass: true }).data);
    } else {
      fs.copyFileSync(source, path.join(out, name));
    }
  }
  return out;
}

if (require.main === module) {
  try {
    const options = parseArgs(process.argv.slice(2));
    build(options).then((out) => {
      console.log(`Built ${options.target} web UI${options.minify ? '' : ' (unminified)'} in ${out}`);
    }).catch((error) => {
      console.error(`web build failed: ${error.message}`);
      process.exitCode = 1;
    });
  } catch (error) {
    console.error(`web build failed: ${error.message}`);
    process.exit(1);
  }
}

module.exports = { build, demoHtml, demoCss };
