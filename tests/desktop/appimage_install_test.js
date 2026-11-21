const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

test('packaged AppImage installs without enabling login or touching live services', { timeout: 20000 }, (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-package-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const bin = path.join(dir, 'bin');
  fs.mkdirSync(bin);
  fs.mkdirSync(path.join(dir, 'run'), { mode: 0o700 });
  const script = (name, body) => fs.writeFileSync(path.join(bin, name), `#!/bin/sh\n${body}\n`, { mode: 0o755 });
  script('wireplumber', 'echo "Compiled with libwireplumber 0.4.0"');
  script('systemctl', 'echo "$*" >> "$TEST_ROOT/systemctl"; exit 0');
  script('xdg-open', 'echo "$1" >> "$TEST_ROOT/browser"; exit 0');
  const image = path.resolve('out/desktop/audiox-companion-linux-x86_64.AppImage');
  const result = spawnSync('timeout', ['6s', 'dbus-run-session', '--', image, '--appimage-extract-and-run',
    '--host', '127.0.0.1', '--port', '1'], {
    encoding: 'utf8',
    env: { ...process.env, HOME: dir, XDG_DATA_HOME: path.join(dir, 'data'),
      XDG_CONFIG_HOME: path.join(dir, 'config'), XDG_RUNTIME_DIR: path.join(dir, 'run'),
      TEST_ROOT: dir, APPIMAGELAUNCHER_DISABLE: '1', PATH: `${bin}:/usr/bin:/bin` },
  });
  assert.equal(result.status, 124, `${result.stdout}\n${result.stderr}`);
  const install = path.join(dir, 'data/audiox');
  assert.deepEqual(fs.readFileSync(path.join(install, 'appimages/current.AppImage')), fs.readFileSync(image));
  assert.ok(fs.statSync(path.join(install, 'audiox-desktop')).mode & 0o100);
  assert.match(fs.readFileSync(path.join(dir, 'config/systemd/user/audiox-companion.service'), 'utf8'),
    /audiox-desktop/);
  const calls = fs.readFileSync(path.join(dir, 'systemctl'), 'utf8');
  assert.match(calls, /daemon-reload/);
  assert.doesNotMatch(calls, /(?:enable .*audiox-companion|restart .*wireplumber)/);
  assert.ok(fs.existsSync(path.join(dir, 'browser')), `${result.stdout}\n${result.stderr}`);
  assert.match(fs.readFileSync(path.join(dir, 'browser'), 'utf8'), /^http:\/\/127\.0\.0\.1:\d+\/.*\/login/m);
});
