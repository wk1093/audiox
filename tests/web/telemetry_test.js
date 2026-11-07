const assert = require('node:assert/strict');
const { test } = require('node:test');
const { createRelay, endpoint } = require('../../web/embedded/telemetry.js');
const fs = require('node:fs');
const vm = require('node:vm');

const batch = { schema: 3, batch: 'a'.repeat(32) + ':' + 'b'.repeat(32), points: Array(100).fill({
  installation_id: 'c'.repeat(64), interval_ms: 10000, public_consent: 1,
}) };

function harness({ pending = 100, consent = 'enabled', uploadOk = true, receipt = batch.batch } = {}) {
  const calls = [], statuses = [];
  let time = 0, unknown = 0;
  const state = { pending, consent };
  let currentBatch = batch;
  const deviceRequest = async (path, init) => {
    calls.push({ path, init });
    if (path === '/api/telemetry/batch') {
      currentBatch = { ...batch, points: Array(Math.min(state.pending, 1000)).fill(batch.points[0]) };
      return Response.json(currentBatch);
    }
    if (path === '/api/telemetry/ack') state.pending -= currentBatch.points.length;
    return Response.json({ ...state });
  };
  const relay = createRelay({
    deviceRequest,
    request: async (path, init) => {
      calls.push({ path, init });
      return Response.json({ batch: receipt }, { status: uploadOk ? 200 : 503 });
    },
    onStatus: (status) => statuses.push(status),
    onUnknown: () => { unknown++; },
    now: () => time,
  });

  return { relay, calls, state, statuses, advance(ms) { time += ms; }, unknown: () => unknown };
}

test('one tick drains 120 and 500 points in one request, and larger queues including the remainder', async () => {
  for (const pending of [120, 500, 1020, 8640]) {
    const h = harness({ pending });
    await h.relay.tick();
    const lengths = h.calls.filter((call) => call.path === endpoint).map((call) => JSON.parse(call.init.body).points.length);
    assert.equal(lengths.reduce((sum, size) => sum + size, 0), pending);
    assert.equal(lengths.length, Math.ceil(pending / 1000));
    assert.equal(h.state.pending, 0);
  }
});

test('a failed remainder stays queued and retries below the initial threshold without reload', async () => {
  let pending = 1020, time = 0, uploads = 0, currentBatch;
  const relay = createRelay({
    now: () => time,
    deviceRequest: async (path) => {
      if (path.endsWith('/batch')) {
        currentBatch = { ...batch, points: Array(Math.min(pending, 1000)).fill(batch.points[0]) };
        return Response.json(currentBatch);
      }
      if (path.endsWith('/ack')) pending -= currentBatch.points.length;
      return Response.json({ consent: 'enabled', pending });
    },
    request: async () => Response.json({ batch: batch.batch }, { status: ++uploads === 2 ? 503 : 200 }),
  });
  await relay.tick();
  assert.equal(pending, 20);
  await relay.tick();
  assert.equal(uploads, 2);
  time += 60000;
  await relay.tick();
  assert.equal(uploads, 3);
  assert.equal(pending, 0);
});

test('unknown, disabled and 99-point queues never upload', async () => {
  for (const options of [{ consent: 'unknown' }, { consent: 'disabled' }, { pending: 99 }]) {
    const h = harness(options);
    await h.relay.tick();
    assert.deepEqual(h.calls.map((call) => call.path), ['/api/telemetry']);
    assert.equal(h.unknown(), options.consent === 'unknown' ? 1 : 0);
  }
});

test('later polling drains a newly accumulated backlog without recreating the relay', async () => {
  const h = harness({ pending: 99 });
  await h.relay.tick();
  h.state.pending = 120;
  h.advance(60000);
  await h.relay.tick();
  assert.equal(h.state.pending, 0);
  h.state.pending = 500;
  h.advance(60000);
  await h.relay.tick();
  assert.equal(h.state.pending, 0);
  assert.deepEqual(h.calls.filter((call) => call.path === endpoint).map((call) =>
    JSON.parse(call.init.body).points.length), [120, 500]);
});

test('100-point threshold uploads exact batch and acknowledges only after matching receipt', async () => {
  const h = harness();
  await h.relay.tick();
  assert.deepEqual(h.calls.map((call) => call.path), [
    '/api/telemetry', '/api/telemetry/batch', '/api/telemetry', endpoint, '/api/telemetry/ack',
  ]);
  const upload = h.calls[3].init;
  assert.deepEqual(JSON.parse(upload.body), batch);
  assert.equal(upload.credentials, 'omit');
  assert.equal(upload.referrerPolicy, 'no-referrer');
  assert.equal(h.calls[4].init.body, batch.batch);
  assert.equal(h.state.pending, 0);
});

test('failed uploads and mismatched receipts retain points and use retry backoff', async () => {
  for (const options of [{ uploadOk: false }, { receipt: 'wrong' }]) {
    const h = harness(options);
    await h.relay.tick();
    assert.equal(h.state.pending, 100);
    assert.ok(h.statuses.at(-1).error);
    assert.ok(!h.calls.some((call) => call.path === '/api/telemetry/ack'));
    await h.relay.tick();
    assert.equal(h.calls.filter((call) => call.path === endpoint).length, 1);
    h.advance(60000);
    await h.relay.tick();
    assert.equal(h.calls.filter((call) => call.path === endpoint).length, 2);
  }
});

test('opting out in another tab before upload stops transmission', async () => {
  let calls = 0, uploads = 0;
  const relay = createRelay({
    deviceRequest: async (path) => {
      if (path.endsWith('/batch')) return Response.json(batch);
      return Response.json({ consent: ++calls === 1 ? 'enabled' : 'disabled', pending: 100 });
    },
    request: async () => { uploads++; },
  });
  await relay.tick();
  assert.equal(uploads, 0);
});

test('concurrent ticks are serialized and cancellation never acknowledges', async () => {
  let release, uploads = 0, acks = 0;
  const started = new Promise((resolve) => { release = resolve; });
  const relay = createRelay({
    deviceRequest: async (path) => {
      if (path.endsWith('/batch')) return Response.json(batch);
      if (path.endsWith('/ack')) acks++;
      return Response.json({ consent: 'enabled', pending: 100 });
    },
    request: async (_path, init) => {
      uploads++;
      release();
      await new Promise((_resolve, reject) => {
        init.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
      });
    },
  });
  const first = relay.tick();
  await started;
  await relay.tick();
  relay.cancel();
  await first;
  assert.equal(uploads, 1);
  assert.equal(acks, 0);
});

test('local device errors are surfaced without network uploads', async () => {
  const statuses = [];
  const relay = createRelay({
    deviceRequest: async () => Response.json({ error: 'Queue read failed' }),
    request: async () => { assert.fail('must not upload'); },
    onStatus: (status) => statuses.push(status),
  });
  await relay.tick();
  assert.match(statuses.at(-1).error, /Queue read failed/);
});

async function consentUI(state, { demo = false, failSave = false } = {}) {
  class Control extends EventTarget {
    checked = false;
    disabled = false;
    textContent = '';
    open = false;
    showModal() { this.open = true; }
    close() { this.open = false; }
    focus() { this.focused = true; }
    click() { this.dispatchEvent(new Event('click')); }
  }
  const controls = new Map();
  const element = (id) => {
    if (!controls.has(id)) controls.set(id, new Control());
    return controls.get(id);
  };
  let requests = 0, uploads = 0;
  const notifications = [], timers = [];
  const context = vm.createContext({
    console, AbortController, AbortSignal, Response, setTimeout, clearTimeout,
    setInterval(fn) { timers.push(fn); },
    addEventListener() {},
    document: { getElementById: element, querySelector: () => element('telemetry-dialog').open ? {} : null },
    AudioxApi: { demo },
    AudioxUI: { isBusy: () => false, notify: (error) => notifications.push(error.message) },
    apiFetch: async (_path, init = {}) => {
      requests++;
      if (init.method === 'POST') {
        if (failSave) return Response.json({ error: 'Cannot save consent' }, { status: 500 });
        assert.ok(init.body === 'enabled=1&consent_version=3' || init.body === 'enabled=0');
        state.consent = init.body === 'enabled=1&consent_version=3' ? 'enabled' : 'disabled';
        state.pending = 0;
      }
      return Response.json({ capacity: 8640, interval_ms: 10000, error: '', ...state });
    },
    fetch: async () => { uploads++; assert.fail('Consent UI must not upload without a ready batch'); },
  });
  vm.runInContext(fs.readFileSync(require.resolve('../../web/embedded/telemetry.js'), 'utf8'), context);
  context.AudioxTelemetry.init();
  const settle = async () => { await new Promise(setImmediate); await new Promise(setImmediate); };
  await settle();
  return { element, requests: () => requests, uploads: () => uploads, notifications, timers, settle };
}

test('fresh install and legacy upgrade prompt; declining persists across browsers without collecting', async () => {
  const state = { consent: 'unknown', pending: 0 };
  const first = await consentUI(state);
  assert.equal(first.element('telemetry-dialog').open, true);
  const notice = first.element('telemetry-dialog-description').textContent;
  assert.match(notice, /Daily aggregate statistics will be PUBLIC/);
  assert.match(notice, /few participants even daily totals can reveal individual activity/);
  assert.match(notice, /cannot recall uploaded reports or copies/);
  assert.equal(first.element('btn-telemetry-decline').focused, true);
  first.element('btn-telemetry-decline').click();
  await first.settle();
  assert.equal(state.consent, 'disabled');
  assert.equal(first.element('telemetry-dialog').open, false);
  assert.equal(first.uploads(), 0);
  const anotherBrowser = await consentUI(state);
  assert.equal(anotherBrowser.element('telemetry-dialog').open, false);
  assert.equal(anotherBrowser.element('telemetry-enabled').checked, false);
});

test('explicit acceptance enables device preference; System opt-out clears pending reports', async () => {
  const state = { consent: 'unknown', pending: 0 };
  const h = await consentUI(state);
  h.element('btn-telemetry-accept').click();
  await h.settle();
  assert.equal(state.consent, 'enabled');
  assert.equal(h.element('telemetry-enabled').checked, true);
  assert.equal(h.element('telemetry-dialog').open, false);
  state.pending = 99;
  h.element('telemetry-enabled').checked = false;
  h.element('telemetry-enabled').dispatchEvent(new Event('change'));
  await h.settle();
  assert.equal(state.consent, 'disabled');
  assert.equal(state.pending, 0);
});

test('failed consent persistence leaves prompt open and surfaces the failure', async () => {
  const state = { consent: 'unknown', pending: 0 };
  const h = await consentUI(state, { failSave: true });
  h.element('btn-telemetry-accept').click();
  await h.settle();
  assert.equal(state.consent, 'unknown');
  assert.equal(h.element('telemetry-dialog').open, true);
  assert.match(h.notifications[0], /Cannot save consent/);
});

test('enabled queue errors retain a checked switch so users can opt out to recover', async () => {
  const h = await consentUI({ consent: 'enabled', pending: 0, error: 'Corrupt queue; disable telemetry to clear it' });
  assert.equal(h.element('telemetry-enabled').checked, true);
  assert.match(h.element('telemetry-status').textContent, /Corrupt queue/);
  assert.equal(h.uploads(), 0);
});

test('public demo makes no telemetry requests, prompts or upload timers', async () => {
  const h = await consentUI({ consent: 'unknown', pending: 100 }, { demo: true });
  assert.equal(h.requests(), 0);
  assert.equal(h.uploads(), 0);
  assert.equal(h.element('telemetry-dialog').open, false);
  assert.equal(h.element('telemetry-enabled').disabled, true);
  assert.equal(h.timers.length, 0);
});
