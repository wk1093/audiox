const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { EventEmitter, once } = require('node:events');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { test } = require('node:test');

for (const mode of ['service', 'manual', 'disconnected']) test(`desktop update waits for Pi acknowledgement (${mode})`, {
  timeout: 15000, skip: process.env.AUDIOX_DESKTOP_WINDOWS === '1',
}, async (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-update-protocol-'));
  const helper = path.join(dir, 'helper');
  fs.writeFileSync(helper, '#!/bin/sh\nexit 0\n', { mode: 0o755 });
  const data = path.join(dir, 'data');
  fs.mkdirSync(path.join(data, 'audiox'), { recursive: true });
  fs.writeFileSync(path.join(data, 'audiox/audiox-desktop'),
    '#!/bin/sh\nprintf "%s\\n" "$*" > "$HOME/restarted"\n', { mode: 0o755 });
  const server = net.createServer();
  const frames = new EventEmitter();
  let connection;
  server.on('connection', socket => {
    connection = socket;
    let input = Buffer.alloc(0), upgraded = false;
    socket.on('data', data => {
      input = Buffer.concat([input, data]);
      if (!upgraded) {
        const end = input.indexOf('\r\n\r\n');
        if (end < 0) return;
        const key = input.subarray(0, end).toString().match(/Sec-WebSocket-Key: ([^\r\n]+)/i)[1];
        const accept = crypto.createHash('sha1')
          .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest('base64');
        socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: audiox.desktop.v1\r\n\r\n`);
        input = input.subarray(end + 4);
        upgraded = true;
      }
      while (input.length >= 2) {
        let length = input[1] & 127, offset = 2;
        if (length === 126) {
          if (input.length < 4) return;
          length = input.readUInt16BE(2);
          offset = 4;
        }
        if (input.length < offset + 4 + length) return;
        const mask = input.subarray(offset, offset + 4);
        const payload = Buffer.from(input.subarray(offset + 4, offset + 4 + length));
        for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i % 4];
        frames.emit('frame', JSON.parse(payload.toString()));
        input = input.subarray(offset + 4 + length);
      }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const first = once(frames, 'frame');
  const binary = path.resolve(__dirname, '../../out/desktop/linux/x86_64/audiox-companion');
  const child = spawn(binary, ['--host', '127.0.0.1', '--port', String(server.address().port)], {
    env: { ...process.env, XDG_RUNTIME_DIR: dir, PIPEWIRE_RUNTIME_DIR: dir,
      HOME: dir, XDG_DATA_HOME: data, AUDIOX_TRAY_PATH: helper, APPIMAGE: path.join(dir, 'image'),
      INVOCATION_ID: mode === 'manual' ? '' : 'test', PATH: `${dir}:/usr/bin:/bin` },
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  let errors = '';
  child.stderr.on('data', data => { errors += data; });
  t.after(async () => {
    connection?.destroy();
    if (child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGTERM');
      await exited;
    }
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(dir, { recursive: true, force: true });
  });
  assert.ok((await first)[0].capabilities.includes('desktop_update'));
  const send = message => {
    const bytes = Buffer.from(JSON.stringify(message));
    assert.ok(bytes.length < 126);
    connection.write(Buffer.concat([Buffer.from([0x81, bytes.length]), bytes]));
  };
  const result = once(frames, 'frame');
  send({ type: 'update_release', id: 1, offset: 0, data: Buffer.from('v1.2.3').toString('hex'), done: true });
  assert.deepEqual((await result)[0], { type: 'update_result', id: 1, ok: true });
  assert.equal(child.exitCode, null);
  if (mode === 'disconnected') {
    connection.destroy();
    for (let i = 0; i < 50 && !errors.includes('acknowledgement was lost'); ++i)
      await new Promise(resolve => setTimeout(resolve, 100));
    assert.match(errors, /update installed, but Pi acknowledgement was lost; restart the companion manually/);
    assert.equal(child.exitCode, null);
    return;
  }
  const exited = once(child, 'exit');
  send({ type: 'update_ack', id: 1 });
  assert.equal((await exited)[0], 0);
  if (mode === 'manual') {
    for (let i = 0; i < 50 && !fs.existsSync(path.join(dir, 'restarted')); ++i)
      await new Promise(resolve => setTimeout(resolve, 100));
    assert.match(fs.readFileSync(path.join(dir, 'restarted'), 'utf8'), /--host 127\.0\.0\.1 --port \d+/);
  }
});

test('companion announces detected capabilities, responds to ping, and reconnects', { timeout: 60000 }, async (t) => {
  const windows = process.env.AUDIOX_DESKTOP_WINDOWS === '1';
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'audiox-desktop-'));
  const pipewire = net.createServer();
  const server = net.createServer();
  const sockets = new Set();
  const events = new EventEmitter();
  let child;
  let shuttingDown = false;
  t.after(async () => {
    shuttingDown = true;
    const closed = server.listening
      ? new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()))
      : Promise.resolve();
    for (const socket of sockets) socket.destroy();
    if (child && child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGTERM');
      await exited;
    }
    await closed;
    if (pipewire.listening) await new Promise((resolve) => pipewire.close(resolve));
    fs.rmSync(dir, { recursive: true, force: true });
  });
  if (!windows) {
    fs.writeFileSync(path.join(dir, 'update-helper'), `#!/bin/sh
printf '%s\\n' "$*" >> "$XDG_RUNTIME_DIR/update-calls"
exit 1
`, { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'wpctl'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'pw-loopback'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'pw-metadata'), '#!/bin/sh\nexit 0\n', { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'pactl'), `#!/bin/sh
case "$1 $2 $3" in
  '-f json info')
    [ -f "$XDG_RUNTIME_DIR/json-enabled" ] || exit 1
    printf '{"cookie":"test"}\\n' ;;
  '-f json list')
    [ -f "$XDG_RUNTIME_DIR/json-enabled" ] || exit 1
    printf '[]\\n' ;;
  'get-default-sink  ')
    if [ -s "$XDG_RUNTIME_DIR/default-before" ]; then
      IFS= read -r default_sink < "$XDG_RUNTIME_DIR/default-before"
      : > "$XDG_RUNTIME_DIR/default-before"
    else
      IFS= read -r default_sink < "$XDG_RUNTIME_DIR/default-sink"
    fi
    printf '%s\\n' "$default_sink" ;;
  'list short sinks')
    printf '1\\tsink.original\\tmodule\\tformat\\tRUNNING\\n2\\tsink.audiox\\tmodule\\tformat\\tRUNNING\\n3\\tsink.external\\tmodule\\tformat\\tRUNNING\\n' ;;
  'set-default-sink '*)
    IFS= read -r default_sink < "$XDG_RUNTIME_DIR/default-sink"
    printf '%s\\n' "$default_sink" > "$XDG_RUNTIME_DIR/default-before"
    printf '%s\\n' "$2" > "$XDG_RUNTIME_DIR/default-sink" ;;
  'list short sink-inputs')
    IFS= read -r sink < "$XDG_RUNTIME_DIR/sink"
    IFS= read -r client < "$XDG_RUNTIME_DIR/client"
    printf '12\\t1\\t%s\\tprotocol\\tformat\\n' "$client"
    printf '11\\t%s\\t22\\tprotocol\\tformat\\n' "$sink" ;;
  'move-sink-input 12 '*)
    printf 'unexpected move\\n' >> "$XDG_RUNTIME_DIR/moves" ;;
  'move-sink-input 11 '*)
    if [ "$3" = 99 ]; then echo 'sink unavailable' >&2; exit 1; fi
    printf '%s\\n' "$3" > "$XDG_RUNTIME_DIR/sink" ;;
  *) exit 1 ;;
esac
`, { mode: 0o755 });
    fs.writeFileSync(path.join(dir, 'sink'), '1\n');
    fs.writeFileSync(path.join(dir, 'default-sink'), 'sink.original\n');
    fs.writeFileSync(path.join(dir, 'client'), '-\n');
    fs.writeFileSync(path.join(dir, 'pw-dump'), `#!/bin/sh
IFS= read -r data < "$XDG_RUNTIME_DIR/nodes"
printf '%s' "$data"
`, { mode: 0o755 });
    const nodes = [
      { id: 21, type: 'PipeWire:Interface:Node', info: { props: {
        'node.name': 'player"\\名字', 'application.name': 'Music ♪', 'media.class': 'Stream/Output/Audio',
        'object.serial': 555, 'node.description': 'a'.repeat(500),
      } } },
      { id: 22, type: 'PipeWire:Interface:Node', info: { props: { 'node.name': 'usb.sink', 'media.class': 'Audio/Sink' } } },
      { id: 23, type: 'PipeWire:Interface:Port', info: { props: { 'node.name': 'ignored' } } },
    ];
    fs.writeFileSync(path.join(dir, 'nodes'),
      JSON.stringify(nodes).replace('"object.serial":555', '"object.serial":18446744073709551615') + '\n');
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
  server.on('connection', (socket) => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.on('error', (error) => {
      if (!shuttingDown || error.code !== 'ECONNRESET') events.emit('error', error);
    });
    if (shuttingDown) {
      socket.destroy();
      return;
    }
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
    { env: windows ? process.env : { ...process.env, XDG_RUNTIME_DIR: dir, PIPEWIRE_RUNTIME_DIR: dir,
      PIPEWIRE_REMOTE: 'pipewire-0', AUDIOX_TRAY_PATH: path.join(dir, 'update-helper'),
      APPIMAGE: path.join(dir, 'image.AppImage'), PATH: dir }, stdio: 'ignore' });
  assert.match((await request)[0], /^GET \/ws\/desktop HTTP\/1\.1\r\n/);
  const first = (await firstFrame)[0];
  assert.equal(first.opcode, 1);
  assert.equal(first.masked, true);
  const firstHello = JSON.parse(first.payload);
  assert.deepEqual(firstHello.capabilities, windows ? [] :
    ['pipewire', 'route_stream', 'default_sink', 'pipewire_nodes', 'auto_route', 'desktop_update']);
  if (!windows) {
    const sendText = (text) => {
      const payload = Buffer.from(text);
      const header = payload.length < 126 ? Buffer.from([0x81, payload.length]) :
        Buffer.from([0x81, 126, payload.length >> 8, payload.length & 255]);
      first.socket.write(Buffer.concat([header, payload]));
    };
    const updateResult = once(events, 'frame');
    sendText(JSON.stringify({ type: 'update_release', id: 8, offset: 0,
      data: Buffer.from('v1.2.3+4').toString('hex'), done: true }));
    assert.deepEqual(JSON.parse((await updateResult)[0].payload), { type: 'update_result', id: 8, ok: false });
    assert.match(fs.readFileSync(path.join(dir, 'update-calls'), 'utf8'), /--release-update v1\.2\.3\+4/);
    const snapshot = once(events, 'frame');
    sendText('{"type":"default_watch"}');
    assert.deepEqual(JSON.parse((await snapshot)[0].payload), {
      type: 'default_snapshot', id: 1, offset: 0, data: Buffer.from('sink.original').toString('hex'), done: true,
    });
    const externallyChanged = once(events, 'frame');
    fs.writeFileSync(path.join(dir, 'default-sink'), 'sink.external\n');
    assert.deepEqual(JSON.parse((await externallyChanged)[0].payload), {
      type: 'default_snapshot', id: 1, offset: 0, data: Buffer.from('sink.external').toString('hex'), done: true,
    });
    const setResult = once(events, 'frame');
    sendText(JSON.stringify({ type: 'default_set', id: 7, offset: 0,
      data: Buffer.from('sink.audiox').toString('hex'), done: true }));
    assert.deepEqual(JSON.parse((await setResult)[0].payload), {
      type: 'default_result', id: 7, offset: 0, data: Buffer.from('sink.audiox').toString('hex'), done: true,
    });
    assert.equal(fs.readFileSync(path.join(dir, 'default-sink'), 'utf8'), 'sink.audiox\n');
    const rejectedDefault = once(events, 'frame');
    sendText(JSON.stringify({ type: 'default_set', id: 8, offset: 0,
      data: Buffer.from('sink.missing').toString('hex'), done: true }));
    assert.deepEqual(JSON.parse((await rejectedDefault)[0].payload), {
      type: 'default_failed', id: 8, offset: 0, data: Buffer.from('sink.audiox').toString('hex'), done: true,
    });
    const sendRules = async (id, text) => {
      const bytes = Buffer.from(text);
      for (let offset = 0; offset < bytes.length;) {
        const chunk = bytes.subarray(offset, offset + 400);
        const done = offset + chunk.length === bytes.length;
        const response = once(events, 'frame');
        sendText(JSON.stringify({ type: 'rules_chunk', id, offset, data: chunk.toString('hex'), done }));
        offset += chunk.length;
        assert.deepEqual(JSON.parse((await response)[0].payload), { type: 'rules_ack', id, offset, ok: true });
      }
    };
    const rule = `p\te\te\t1\t${Buffer.from('app'.repeat(20)).toString('hex')}\t${Buffer.from('sink').toString('hex')}\n`;
    await sendRules(1, '# audiox desktop routes v1\n' + rule.repeat(4));
    const failedRules = once(events, 'frame');
    assert.deepEqual(JSON.parse((await failedRules)[0].payload), { type: 'auto_status', ok: false });
    fs.writeFileSync(path.join(dir, 'json-enabled'), '');
    await sendRules(2, '# audiox desktop routes v1\n');
    const clearedRules = once(events, 'frame');
    assert.deepEqual(JSON.parse((await clearedRules)[0].payload), { type: 'auto_status', ok: true });
    const badRules = once(events, 'frame');
    sendText(JSON.stringify({ type: 'rules_chunk', id: 3, offset: 0,
      data: Buffer.from('unsupported').toString('hex'), done: true }));
    assert.deepEqual(JSON.parse((await badRules)[0].payload), { type: 'rules_ack', id: 3, offset: 0, ok: false });
    let offset = 0;
    const chunks = [];
    do {
      const response = once(events, 'frame');
      const payload = Buffer.from(`{"type":"inventory","id":9,"offset":${offset}}`);
      first.socket.write(Buffer.concat([Buffer.from([0x81, payload.length]), payload]));
      const result = JSON.parse((await response)[0].payload);
      assert.equal(result.type, 'inventory_chunk');
      assert.equal(result.id, 9);
      assert.equal(result.offset, offset);
      const bytes = Buffer.from(result.data, 'hex');
      chunks.push(bytes);
      offset += bytes.length;
      if (result.done) break;
    } while (offset < 10000);
    const inventory = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    assert.equal(inventory.length, 2);
    assert.equal(inventory[0]['node.name'], 'player"\\名字');
    assert.equal(inventory[0]['application.name'], 'Music ♪');
    assert.equal(inventory[0]['object.serial'], '18446744073709551615');
    assert.equal(inventory[1]['node.name'], 'usb.sink');
    assert.ok(offset > 400, 'inventory crosses a frame boundary');
    fs.writeFileSync(path.join(dir, 'nodes'), '[invalid]\n');
    const failure = once(events, 'frame');
    const invalidRequest = Buffer.from('{"type":"inventory","id":10,"offset":0}');
    first.socket.write(Buffer.concat([Buffer.from([0x81, invalidRequest.length]), invalidRequest]));
    assert.deepEqual(JSON.parse((await failure)[0].payload), { type: 'inventory_error', id: 10 });
    const route = async (id, sink, ok, stream = 11) => {
      const response = once(events, 'frame');
      const payload = Buffer.from(`{"type":"route","id":${id},"stream":${stream},"sink":${sink}}`);
      first.socket.write(Buffer.concat([Buffer.from([0x81, payload.length]), payload]));
      assert.deepEqual(JSON.parse((await response)[0].payload), { type: 'route_result', id, ok });
    };
    await route(1, 2, true);
    assert.equal(fs.readFileSync(path.join(dir, 'sink'), 'utf8'), '2\n');
    await route(2, 99, false);
    assert.equal(fs.readFileSync(path.join(dir, 'sink'), 'utf8'), '2\n');
    await route(4, 2, false, 12);
    assert.equal(fs.existsSync(path.join(dir, 'moves')), false);
    fs.writeFileSync(path.join(dir, 'client'), '12x\n');
    await route(5, 4, false);
    assert.equal(fs.readFileSync(path.join(dir, 'sink'), 'utf8'), '2\n');
    fs.writeFileSync(path.join(dir, 'client'), '-\n');
    fs.writeFileSync(path.join(dir, 'sink'), '3\n');
    await route(3, 4, true);
    const updated = once(events, 'frame');
    const ready = Buffer.from('{"type":"ready","protocol":"audiox.desktop.v1","media_status":true}');
    first.socket.write(Buffer.concat([Buffer.from([0x81, ready.length]), ready]));
    const mediaHello = JSON.parse((await updated)[0].payload);
    assert.deepEqual(mediaHello.capabilities, ['pipewire', 'route_stream', 'default_sink', 'pipewire_nodes', 'auto_route', 'desktop_update', 'mpris']);
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
    assert.deepEqual(JSON.parse(refreshed.payload).capabilities, ['desktop_update', 'mpris']);
  }

  const reconnected = once(events, 'frame');
  first.socket.destroy();
  const again = (await reconnected)[0];
  if (!windows) assert.equal(fs.readFileSync(path.join(dir, 'sink'), 'utf8'), '3\n');
  assert.equal(again.opcode, 1);
  assert.deepEqual(JSON.parse(again.payload).capabilities, windows ? [] : ['desktop_update']);
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
