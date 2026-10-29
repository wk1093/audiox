const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

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
