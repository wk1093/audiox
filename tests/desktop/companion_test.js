const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { EventEmitter, once } = require('node:events');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { test } = require('node:test');

test('companion announces detected capabilities, responds to ping, and reconnects', { timeout: 45000 }, async (t) => {
  const windows = process.env.AUDIOX_DESKTOP_WINDOWS === '1';
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-desktop-'));
  const pipewire = net.createServer();
  const server = net.createServer();
  const sockets = new Set();
  let child;
  t.after(async () => {
    if (child && child.exitCode === null && child.signalCode === null) {
      child.kill('SIGTERM');
      await once(child, 'exit');
    }
    for (const socket of sockets) socket.destroy();
    if (server.listening) await new Promise((resolve) => server.close(resolve));
    if (pipewire.listening) await new Promise((resolve) => pipewire.close(resolve));
    fs.rmSync(dir, { recursive: true, force: true });
  });
  if (!windows) {
    fs.writeFileSync(path.join(dir, 'wpctl'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'playerctl'), `#!/bin/sh
IFS= read -r status < "$XDG_RUNTIME_DIR/media-status"
case "$status" in
  Playing) printf 'Paused\\nPlaying\\n' ;;
  Paused) printf 'Paused\\nStopped\\n' ;;
  Hang) exec /bin/sleep 10 ;;
  None) printf 'No players found\\n' >&2; exit 1 ;;
  *) printf 'D-Bus unavailable\\n' >&2; exit 1 ;;
esac
`, { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'media-status'), 'Playing\n');
    await new Promise((resolve) => pipewire.listen(path.join(dir, 'pipewire-0'), resolve));
  }
  const events = new EventEmitter();
  server.on('connection', (socket) => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let input = Buffer.alloc(0);
    let upgraded = false;
    socket.on('data', (data) => {
      input = Buffer.concat([input, data]);
      if (!upgraded) {
        const end = input.indexOf('\r\n\r\n');
        if (end < 0) return;
        const request = input.subarray(0, end).toString();
        const key = request.match(/Sec-WebSocket-Key: ([^\r\n]+)/i)?.[1];
        events.emit('request', request);
        const accept = crypto.createHash('sha1')
          .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest('base64');
        socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: audiox.desktop.v1\r\n\r\n`);
        input = input.subarray(end + 4);
        upgraded = true;
      }
      while (input.length >= 2) {
        let length = input[1] & 127;
        let offset = 2;
        if (length === 126) {
          if (input.length < 4) return;
          length = input.readUInt16BE(2);
          offset = 4;
        }
        if (input.length < offset + 4 + length) return;
        const mask = input.subarray(offset, offset + 4);
        const payload = Buffer.from(input.subarray(offset + 4, offset + 4 + length));
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
        events.emit('frame', { opcode: input[0] & 15, masked: !!(input[1] & 128), payload: payload.toString(), socket });
        input = input.subarray(offset + 4 + length);
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const request = once(events, 'request');
  const firstFrame = once(events, 'frame');
  const binary = path.resolve(__dirname, windows ? '../../out/desktop/audiox-companion-windows-x86_64.exe' :
    '../../out/desktop/linux/x86_64/audiox-companion');
  const runner = process.env.AUDIOX_DESKTOP_RUNNER;
  const args = ['--host', '127.0.0.1', '--port', String(server.address().port)];
  child = spawn(runner || binary, runner ? [binary, ...args] : args,
    { env: windows ? process.env : { ...process.env, XDG_RUNTIME_DIR: dir, PIPEWIRE_REMOTE: 'pipewire-0', PATH: dir }, stdio: 'ignore' });
  assert.match((await request)[0], /^GET \/ws\/desktop HTTP\/1\.1\r\n/);
  const first = (await firstFrame)[0];
  assert.equal(first.opcode, 1);
  assert.equal(first.masked, true);
  const firstHello = JSON.parse(first.payload);
  assert.deepEqual(firstHello.capabilities, windows ? ['default_sink'] : ['pipewire', 'default_sink']);
  if (!windows) {
    const updated = once(events, 'frame');
    const ready = Buffer.from('{"type":"ready","protocol":"audiox.desktop.v1","media_status":true}');
    first.socket.write(Buffer.concat([Buffer.from([0x81, ready.length]), ready]));
    const mediaHello = JSON.parse((await updated)[0].payload);
    assert.deepEqual(mediaHello.capabilities, ['pipewire', 'default_sink', 'mpris']);
    assert.equal(mediaHello.media, 'playing');
  }

  const pong = once(events, 'frame');
  first.socket.write(Buffer.from([0x89, 0x02, 0x6f, 0x6b]));
  const reply = (await pong)[0];
  assert.equal(reply.opcode, 10);
  assert.equal(reply.payload, 'ok');

  if (!windows) {
    let changed = once(events, 'frame');
    fs.writeFileSync(path.join(dir, 'media-status'), 'Paused\n');
    assert.equal(JSON.parse((await changed)[0].payload).media, 'paused');
    changed = once(events, 'frame');
    fs.writeFileSync(path.join(dir, 'media-status'), 'Hang\n');
    assert.equal(JSON.parse((await changed)[0].payload).media, 'error');
    changed = once(events, 'frame');
    fs.writeFileSync(path.join(dir, 'media-status'), 'None\n');
    assert.equal(JSON.parse((await changed)[0].payload).media, 'none');
    changed = once(events, 'frame');
    fs.writeFileSync(path.join(dir, 'media-status'), 'Error\n');
    assert.equal(JSON.parse((await changed)[0].payload).media, 'error');
    changed = once(events, 'frame');
    await new Promise((resolve) => pipewire.close(resolve));
    const refreshed = (await changed)[0];
    assert.deepEqual(JSON.parse(refreshed.payload).capabilities, ['mpris']);
  }

  const reconnected = once(events, 'frame');
  first.socket.destroy();
  const again = (await reconnected)[0];
  assert.equal(again.opcode, 1);
  assert.deepEqual(JSON.parse(again.payload).capabilities, windows ? ['default_sink'] : []);
  const oldReady = Buffer.from('{"type":"ready","protocol":"audiox.desktop.v1"}');
  again.socket.write(Buffer.concat([Buffer.from([0x81, oldReady.length]), oldReady]));
  const oldPong = once(events, 'frame');
  again.socket.write(Buffer.from([0x89, 0x02, 0x6f, 0x6b]));
  assert.equal((await oldPong)[0].opcode, 10);
  let unexpected = false;
  const listener = () => { unexpected = true; };
  events.on('frame', listener);
  await new Promise((resolve) => setTimeout(resolve, 5500));
  events.off('frame', listener);
  assert.equal(unexpected, false, 'legacy server must not receive the media extension');
});
