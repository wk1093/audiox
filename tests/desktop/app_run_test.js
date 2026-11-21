const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

test('AppImage installs, activates, and upgrades its policy without repeated restarts', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-apprun-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const app = path.join(dir, 'app');
  const bin = path.join(dir, 'bin');
  const policy = path.join(app, 'usr/share/audiox/wireplumber');
  fs.mkdirSync(policy, { recursive: true });
  fs.mkdirSync(bin);
  fs.copyFileSync('desktop/AppRun', path.join(app, 'AppRun'));
  for (const name of ['audiox-routing.lua', '90-audiox-routing.conf'])
    fs.copyFileSync(path.join('desktop/wireplumber', name), path.join(policy, name));
  const script = (name, body, directory = bin) =>
    fs.writeFileSync(path.join(directory, name), `#!/bin/sh\n${body}\n`, { mode: 0o755 });
  script('audiox-companion', 'echo companion "$@"', app);
  script('wireplumber', 'echo "Compiled with libwireplumber 0.5.19"');
  script('pw-metadata', '[ ! -f "$TEST_ROOT/active" ] || echo "key:\'audiox.policy\' value:\'1\'"');
  script('systemctl', `case "$*" in
  '--user is-active --quiet wireplumber.service') exit 0 ;;
  '--user restart wireplumber.service')
    echo restart >> "$TEST_ROOT/restarts"
    [ ! -f "$TEST_ROOT/fail" ] || exit 1
    touch "$TEST_ROOT/active" ;;
  *) exit 1 ;;
esac`);
  const env = {
    ...process.env, HOME: path.join(dir, 'home'), XDG_DATA_HOME: path.join(dir, 'data'),
    XDG_CONFIG_HOME: path.join(dir, 'config'), TEST_ROOT: dir,
    PATH: `${bin}:/usr/bin:/bin`,
  };
  const run = (...args) => {
    const result = spawnSync('/bin/sh', [path.join(app, 'AppRun'), ...args], { env, encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /companion/);
    return result;
  };
  const restarts = () => fs.existsSync(path.join(dir, 'restarts'))
    ? fs.readFileSync(path.join(dir, 'restarts'), 'utf8').trim().split('\n').length : 0;
  run('--help');
  assert.equal(fs.existsSync(env.XDG_CONFIG_HOME), false);
  run('--host', '127.0.0.1');
  assert.equal(restarts(), 1);
  assert.equal(fs.readFileSync(path.join(env.XDG_DATA_HOME, 'wireplumber/scripts/audiox-routing.lua'), 'utf8'),
    fs.readFileSync(path.join(policy, 'audiox-routing.lua'), 'utf8'));
  assert.equal(fs.readFileSync(path.join(env.XDG_CONFIG_HOME, 'wireplumber/wireplumber.conf.d/90-audiox-routing.conf'), 'utf8'),
    fs.readFileSync(path.join(policy, '90-audiox-routing.conf'), 'utf8'));
  run();
  assert.equal(restarts(), 1);
  fs.unlinkSync(path.join(dir, 'active'));
  run();
  assert.equal(restarts(), 2);
  fs.appendFileSync(path.join(policy, 'audiox-routing.lua'), '\n-- upgrade\n');
  fs.writeFileSync(path.join(dir, 'fail'), '');
  assert.match(run().stderr, /could not restart WirePlumber/);
  assert.equal(restarts(), 3);
  fs.unlinkSync(path.join(dir, 'fail'));
  run();
  assert.equal(restarts(), 4);
});

test('AppImage reports policy installation failures and still starts the companion', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-apprun-error-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  fs.copyFileSync('desktop/AppRun', path.join(dir, 'AppRun'));
  fs.writeFileSync(path.join(dir, 'audiox-companion'), '#!/bin/sh\necho companion\n', { mode: 0o755 });
  fs.writeFileSync(path.join(dir, 'wireplumber'), '#!/bin/sh\necho "Compiled with libwireplumber 0.5.19"\n', { mode: 0o755 });
  fs.writeFileSync(path.join(dir, 'not-a-directory'), '');
  const result = spawnSync('/bin/sh', [path.join(dir, 'AppRun')], {
    encoding: 'utf8',
    env: { ...process.env, HOME: dir, XDG_DATA_HOME: path.join(dir, 'not-a-directory'),
      XDG_CONFIG_HOME: path.join(dir, 'config'), PATH: `${dir}:/usr/bin:/bin` },
  });
  assert.equal(result.status, 0);
  assert.match(result.stdout, /companion/);
  assert.match(result.stderr, /could not create WirePlumber configuration directories/);
});
