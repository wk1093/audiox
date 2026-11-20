const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { test } = require('node:test');

test('WebSocket subscriptions reconnect, route events, and close when unused', () => {
  const sockets = [];
  const timers = [];
  class FakeWebSocket {
    static OPEN = 1;
    constructor(url, protocol) {
      this.url = url;
      this.protocol = protocol;
      this.readyState = 0;
      this.handlers = {};
      this.sent = [];
      sockets.push(this);
    }
    addEventListener(name, callback) { this.handlers[name] = callback; }
    send(message) { this.sent.push(JSON.parse(message)); }
    open() { this.readyState = FakeWebSocket.OPEN; this.handlers.open(); }
    receive(message) { this.handlers.message({ data: JSON.stringify(message) }); }
    close() {
      this.readyState = 3;
      this.handlers.close();
    }
  }
  const context = vm.createContext({
    WebSocket: FakeWebSocket,
    location: { protocol: 'http:', host: 'audiox.local' },
    fetch() {},
    setTimeout(callback, delay) {
      const timer = { callback, delay, cleared: false };
      timers.push(timer);
      return timer;
    },
    clearTimeout(timer) { timer.cleared = true; },
    console,
  });
  vm.runInContext(fs.readFileSync(require.resolve('../../web/embedded/api.js'), 'utf8'), context);

  const events = [];
  const unsubscribe = context.AudioxApi.subscribe(['midi.note', 'midi.cc'], (data) => events.push(data));
  assert.equal(sockets.length, 1);
  assert.equal(sockets[0].url, 'ws://audiox.local/ws');
  assert.equal(sockets[0].protocol, 'audiox.v1');
  assert.equal(context.AudioxApi.isConnected(), false);
  sockets[0].open();
  assert.equal(context.AudioxApi.isConnected(), true);
  assert.deepEqual(sockets[0].sent, [{ type: 'subscribe', topics: ['midi.note', 'midi.cc'] }]);
  sockets[0].receive({ type: 'event', topic: 'midi.note', data: { last_note: 60 } });
  assert.deepEqual(JSON.parse(JSON.stringify(events)), [{ last_note: 60 }]);

  sockets[0].close();
  assert.equal(context.AudioxApi.isConnected(), false);
  assert.equal(timers.at(-1).delay, 500);
  timers.at(-1).callback();
  assert.equal(sockets.length, 2);
  sockets[1].open();
  assert.deepEqual(sockets[1].sent, [{ type: 'subscribe', topics: ['midi.note', 'midi.cc'] }]);

  unsubscribe();
  assert.deepEqual(sockets[1].sent.at(-1), { type: 'unsubscribe', topics: ['midi.note', 'midi.cc'] });
  assert.equal(sockets[1].readyState, 3);
});
