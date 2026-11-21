const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { EventEmitter, once } = require('node:events');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { test } = require('node:test');

for (const failingPactl of [false, true]) test(
  `WirePlumber controls the default sink when pactl ${failingPactl ? 'fails' : 'is missing'}`,
  { timeout: 15000 }, async (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-wpctl-'));
  const runtime = path.join(dir, 'pipewire');
  const otherRuntime = path.join(dir, 'xdg');
  fs.mkdirSync(runtime);
  fs.mkdirSync(otherRuntime);
  const pipewire = net.createServer();
  const server = net.createServer();
  const sockets = new Set();
  const events = new EventEmitter();
  let child;
  t.after(async () => {
    for (const socket of sockets) socket.destroy();
    if (child && child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGTERM');
      await exited;
    }
    if (server.listening) await new Promise(resolve => server.close(resolve));
    if (pipewire.listening) await new Promise(resolve => pipewire.close(resolve));
    fs.rmSync(dir, { recursive: true, force: true });
  });

  fs.writeFileSync(path.join(dir, 'current'), 'sink.original\n');
  if (failingPactl) {
    fs.writeFileSync(path.join(dir, 'pactl'), '#!/bin/sh\nprintf "Connection refused\\n" >&2\nexit 1\n',
      { mode: 0o755 });
  }
  fs.writeFileSync(path.join(dir, 'wpctl'), `#!/bin/sh
case "$1:$2" in
  'inspect:@DEFAULT_AUDIO_SINK@')
    IFS= read -r name < "$TEST_DIR/current"
    printf 'id 11, type PipeWire:Interface:Node\\n  * node.name = "%s"\\n' "$name" ;;
  'inspect:22')
    if [ -f "$TEST_DIR/replaced" ]; then name=sink.other; else name=sink.audiox; fi
    printf 'id 22, type PipeWire:Interface:Node\\n  * node.name = "%s"\\n' "$name" ;;
  'set-default:22')
    printf 'sink.audiox\\n' > "$TEST_DIR/current" ;;
  *) exit 1 ;;
esac
`, { mode: 0o755 });
  fs.writeFileSync(path.join(dir, 'pw-dump'), `#!/bin/sh
IFS= read -r nodes < "$TEST_DIR/nodes"
printf '%s\\n' "$nodes"
`, { mode: 0o755 });
  const nodes = [
    { id: 11, type: 'PipeWire:Interface:Node', info: { props: {
      'node.name': 'sink.original', 'media.class': 'Audio/Sink',
    } } },
    { id: 22, type: 'PipeWire:Interface:Node', info: { props: {
      'node.name': 'sink.audiox', 'media.class': 'Audio/Sink',
    } } },
  ];
  const writeNodes = (value) => fs.writeFileSync(path.join(dir, 'nodes'), JSON.stringify(value) + '\n');
  writeNodes(nodes);
  await new Promise(resolve => pipewire.listen(path.join(runtime, 'pipewire-custom'), resolve));
  server.on('connection', socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let input = Buffer.alloc(0);
    let upgraded = false;
    socket.on('data', data => {
      input = Buffer.concat([input, data]);
      if (!upgraded) {
        const end = input.indexOf('\r\n\r\n');
        if (end < 0) return;
        const request = input.subarray(0, end).toString();
        const key = request.match(/Sec-WebSocket-Key: ([^\r\n]+)/i)[1];
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
        for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i % 4];
        events.emit('frame', JSON.parse(payload.toString()));
        input = input.subarray(offset + 4 + length);
      }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const firstFrame = once(events, 'frame');
  const binary = path.resolve(__dirname, '../../out/desktop/linux/x86_64/audiox-companion');
  child = spawn(binary, ['--host', '127.0.0.1', '--port', String(server.address().port)], {
    env: { ...process.env, PATH: dir, TEST_DIR: dir, XDG_RUNTIME_DIR: otherRuntime,
      PIPEWIRE_RUNTIME_DIR: runtime,
      PIPEWIRE_REMOTE: failingPactl ? path.join(runtime, 'pipewire-custom') : 'pipewire-custom' },
    stdio: 'ignore',
  });
  const hello = (await firstFrame)[0];
  assert.deepEqual(hello.capabilities, ['pipewire', ...(failingPactl ? ['route_stream'] : []),
    'default_sink', 'pipewire_nodes']);
  const send = async (message) => {
    const response = once(events, 'frame');
    const payload = Buffer.from(JSON.stringify(message));
    assert.ok(payload.length < 126);
    [...sockets][0].write(Buffer.concat([Buffer.from([0x81, payload.length]), payload]));
    return (await response)[0];
  };
  assert.deepEqual(await send({ type: 'default_watch' }), {
    type: 'default_snapshot', id: 1, offset: 0, data: Buffer.from('sink.original').toString('hex'), done: true,
  });
  const set = (id) => send({ type: 'default_set', id, offset: 0,
    data: Buffer.from('sink.audiox').toString('hex'), done: true });
  writeNodes([...nodes, { ...nodes[1], id: 23 }]);
  assert.equal((await set(1)).type, 'default_failed');
  assert.equal(fs.readFileSync(path.join(dir, 'current'), 'utf8'), 'sink.original\n');
  writeNodes(nodes);
  fs.writeFileSync(path.join(dir, 'replaced'), '');
  assert.equal((await set(2)).type, 'default_failed');
  assert.equal(fs.readFileSync(path.join(dir, 'current'), 'utf8'), 'sink.original\n');
  fs.unlinkSync(path.join(dir, 'replaced'));
  assert.deepEqual(await set(3), {
    type: 'default_result', id: 3, offset: 0, data: Buffer.from('sink.audiox').toString('hex'), done: true,
  });
});
