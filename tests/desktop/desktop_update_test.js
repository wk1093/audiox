const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

test('desktop-update installs the current build and restarts only after a successful install', { timeout: 60000 }, (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-make-update-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const bin = path.join(dir, 'bin');
  fs.mkdirSync(bin);
  fs.writeFileSync(path.join(bin, 'systemctl'), `#!/bin/sh
echo "$*" >> "$TEST_ROOT/systemctl"
if [ "\${TEST_FAIL_RESTART:-0}" = 1 ] && [ "$2" = restart ]; then exit 1; fi
`, { mode: 0o755 });
  const env = {
    ...process.env, HOME: dir, XDG_DATA_HOME: path.join(dir, 'data'),
    XDG_CONFIG_HOME: path.join(dir, 'config'), TEST_ROOT: dir,
    PATH: `${bin}:${process.env.PATH}`,
  };
  const run = (extra = {}) => spawnSync('make', ['desktop-update'], {
    encoding: 'utf8', env: { ...env, ...extra },
  });
  const calls = () => fs.readFileSync(path.join(dir, 'systemctl'), 'utf8');
  const installed = path.join(dir, 'data/audiox/appimages/current.AppImage');
  const unit = path.join(dir, 'config/systemd/user/audiox-companion.service');

  const first = run();
  assert.equal(first.status, 0, `${first.stdout}\n${first.stderr}`);
  assert.deepEqual(fs.readFileSync(installed), fs.readFileSync('out/desktop/audiox-companion-linux-x86_64.AppImage'));
  assert.match(calls(), /^--user restart audiox-companion\.service$/m);
  assert.doesNotMatch(calls(), /--user enable|restart wireplumber/);

  const savedUnit = fs.readFileSync(unit);
  fs.writeFileSync(unit, 'customized unit');
  const before = calls();
  const failedInstall = run();
  assert.notEqual(failedInstall.status, 0);
  assert.match(failedInstall.stderr, /refusing to overwrite customized user service/);
  assert.equal(calls(), before);
  fs.writeFileSync(unit, savedUnit);

  const failedRestart = run({ TEST_FAIL_RESTART: '1' });
  assert.notEqual(failedRestart.status, 0);
  assert.match(calls().slice(before.length), /^--user restart audiox-companion\.service$/m);
});
