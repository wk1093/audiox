const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

test('test and benchmark rules stay available through the root Makefile', () => {
  const root = path.resolve(__dirname, '..');
  const mainMakefile = fs.readFileSync(path.join(root, 'Makefile'), 'utf8');
  const testMakefile = fs.readFileSync(path.join(root, 'tests/Makefile'), 'utf8');
  assert.match(mainMakefile, /^include \$\(CURDIR\)\/tests\/Makefile$/m);
  assert.match(mainMakefile, /^\.DEFAULT_GOAL := all$/m);
  assert.doesNotMatch(mainMakefile, /^test-[\w-]+:/m);
  const targets = [
    'test-audio-engine', 'test-midi-usb', 'test-midi-script', 'test-midi-docs',
    'test-display-hotplug', 'test-soundboard-config',
    'test-boot-recovery', 'test-config-backup', 'test-audio-pcm-recover',
    'test-system-logs', 'test-system', 'test-gadget-setup', 'test-mdns',
    'test-gadget-volume', 'test-audio-runtime', 'test-diagnostics',
    'test-watchdog', 'test-resolve-modules', 'test-dnsmasq-config',
  ];
  for (const target of targets) {
    assert.ok(testMakefile.includes(`\n${target}:`), `${target} must remain available`);
  }
  const dryRun = execFileSync('make', [
    '--no-print-directory', '-n', '-B', '-o', path.join(root, '.config'),
    '-o', path.join(root, 'out/host/audiox-menuconfig'), ...targets,
    'audio-bench', 'run-audio-bench', 'BENCH_INPUT=fixture.wav',
    'BENCH_ALSA_DEVICE=null', 'BENCH_PORT=8766',
  ], { cwd: root, encoding: 'utf8' });
  assert.match(dryRun, /audiox-audio-bench" "fixture\.wav" "null" "8766"/);
  assert.match(dryRun, /watchdog_service_test\.sh/);
  assert.match(dryRun, /resolve_modules_test\.sh/);
  assert.match(dryRun, /--test -C/);
});

test('incremental builds rebuild startup, watchdog and worker objects after header changes', () => {
  const root = path.resolve(__dirname, '..');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-build-deps-'));
  try {
    const config = path.join(directory, 'config');
    const menu = path.join(directory, 'menuconfig');
    fs.writeFileSync(config, '');
    fs.writeFileSync(menu, '');
    for (const source of ['init/init', 'init/watchdog', 'audio/setup']) {
      const object = path.join(directory, 'obj/runtime', `${source}.o`);
      const header = path.join(directory, 'context.hpp');
      fs.mkdirSync(path.dirname(object), { recursive: true });
      fs.writeFileSync(object, '');
      fs.writeFileSync(object.replace(/\.o$/, '.d'), `${object}: ${header}\n`);
      fs.writeFileSync(header, '');
      const now = Date.now() / 1000;
      fs.utimesSync(header, now - 20, now - 20);
      fs.utimesSync(object, now - 10, now - 10);
      const args = [
        '--no-print-directory', '-n', `OUT_DIR=${directory}`, `AUDIOX_CONFIG_FILE=${config}`,
        `CONFIG_MENU_BIN=${menu}`, 'RUNTIME_DEPS=', 'SBC_STATIC_LIB=', 'SBC_HEADER=',
        'KV=dependency-test', '-o', config, '-o', menu, '-o', `src/${source}.cpp`, object,
      ];
      const dryRun = () => execFileSync('make', args, { cwd: root, encoding: 'utf8' });
      assert.ok(!dryRun().includes(`-c -o ${object} `), `${source} should start up to date`);
      fs.utimesSync(header, now, now);
      assert.ok(dryRun().includes(`-c -o ${object} `), `${source} must rebuild for a newer header`);
    }
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
