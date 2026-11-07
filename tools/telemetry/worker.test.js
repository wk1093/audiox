import assert from 'node:assert/strict';
import { test } from 'node:test';
import worker, { validBatch } from './worker.js';
import { publicSnapshot, refreshDashboard } from './dashboard.js';
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';

function fixture(schema = 3, count = 100) {
  const keys = ['usb_audio', 'gadget_audio', 'other_audio', 'bluetooth_audio', 'midi_connected',
    'input_devices', 'output_devices', 'soundboard_triggers', 'midi_notes', 'midi_cc',
    'scene_recalls', 'sampler_enabled', 'xruns', 'sample_rate', 'block_frames', 'temperature_c', 'cpu_percent'];
  const points = Array.from({ length: count }, (_, index) => ({
    id: index.toString(16).padStart(32, '0'), version: '1.15.0-beta.13', board: 5,
    metrics: Object.fromEntries(keys.map((key) => [key, key === 'temperature_c' ? null : 0])),
    effects: Array.from({ length: 12 }, () => [2, 1]),
  }));
  if (schema >= 2) for (const point of points) {
    point.installation_id = 'c'.repeat(64);
    point.interval_ms = 10000;
    point.metrics.audio_active = 0;
  }
  if (schema === 3) for (const point of points) point.public_consent = 1;
  return { schema, batch: count ? `${points[0].id}:${points.at(-1).id}` : '', points };
}

function environment() {
  const points = new Map(), statements = [];
  let rateOk = true, dbOk = true;
  return {
    points, statements,
    setRate(value) { rateOk = value; }, setDb(value) { dbOk = value; },
    ALLOWED_ORIGINS: 'http://audiox.local,http://172.31.73.1',
    INGEST_LIMIT: { limit: async () => ({ success: rateOk }) },
    DB: {
      prepare(sql) {
        statements.push(sql);
        return {
          bind(value) {
            return { async run() {
              if (!dbOk) throw new Error('test failure');
              for (const point of JSON.parse(value)) points.set(point.id, point);
              return { success: true };
            } };
          },
          async run() { return { success: true }; },
        };
      },
    },
  };
}
const request = (body = fixture(), extra = {}) => new Request('https://telemetry.audiox.wklab.net/v1/batches', {
  method: 'POST', headers: { Origin: 'http://audiox.local', 'Content-Type': 'application/json' },
  body: JSON.stringify(body), ...extra,
});

test('schema accepts bounded variable public batches and fixed legacy batches, with strict field validation', () => {
  for (const count of [1, 20, 99, 100, 120, 500, 1000]) assert.equal(validBatch(fixture(3, count)), true);
  for (const count of [0, 1001]) assert.equal(validBatch(fixture(3, count)), false);
  for (const schema of [1, 2]) for (const count of [20, 120]) assert.equal(validBatch(fixture(schema, count)), false);
  assert.equal(validBatch(fixture()), true);
  assert.equal(validBatch(fixture(2)), true);
  assert.equal(validBatch(fixture(1)), true); // Older installations can still upload minute summaries.
  const changes = [
    (b) => { b.points.pop(); },
    (b) => { b.points.push(b.points[0]); },
    (b) => { b.installation_id = 'no'; },
    (b) => { b.points[0].name = 'private'; },
    (b) => { b.points[0].metrics.serial = 'private'; },
    (b) => { b.points[0].metrics.usb_audio = -1; },
    (b) => { b.points[0].metrics.cpu_percent = 101; },
    (b) => { b.points[0].metrics.midi_connected = null; },
    (b) => { b.points[0].effects[0] = [1, 2]; },
    (b) => { b.points[0].effects.push([0, 0]); },
    (b) => { b.points[1].id = b.points[0].id; },
    (b) => { b.batch = 'wrong'; },
    (b) => { b.points[0].version = 'private text'; },
    (b) => { b.points[0].board = 3; },
    (b) => { b.points[0].installation_id = 'c'.repeat(63); },
    (b) => { b.points[0].installation_id = 'not-a-hardware-id'; },
    (b) => { b.points[0].installation_id = 'd'.repeat(64); },
    (b) => { delete b.points[0].installation_id; },
    (b) => { b.points[0].interval_ms = 60000; },
    (b) => { b.points[0].metrics.audio_active = 2; },
    (b) => { b.schema = 4; },
    (b) => { delete b.points[0].public_consent; },
    (b) => { b.points[0].public_consent = 0; },
  ];
  for (const change of changes) {
    const body = fixture();
    change(body);
    assert.equal(validBatch(body), false);
  }
});

test('ingestion acknowledges after persistence and deduplicates retries using point IDs', async () => {
  const env = environment(), body = fixture();
  for (let i = 0; i < 2; i++) {
    const response = await worker.fetch(request(body), env);
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), { batch: body.batch });
    assert.equal(response.headers.get('Access-Control-Allow-Origin'), 'http://audiox.local');
  }
  assert.equal(env.points.size, 100);
  assert.match(env.statements[0], /INSERT OR IGNORE.*json_each/);
  assert.equal([...env.points.values()][0].installation_id, 'c'.repeat(64));
  assert.equal([...env.points.values()][0].interval_ms, 10000);
  assert.equal([...env.points.values()][0].public_consent, 1);
});

test('legacy points remain unlinked while new schema requires explicit random identity and cadence', async () => {
  const env = environment(), legacy = fixture(1);
  assert.equal((await worker.fetch(request(legacy), env)).status, 200);
  assert.equal([...env.points.values()][0].installation_id, undefined);
  legacy.points[0].installation_id = 'c'.repeat(64);
  assert.equal(validBatch(legacy), false);
  const linked = fixture(2);
  linked.points[0].public_consent = 1;
  assert.equal(validBatch(linked), false); // Old notice cannot authorize public reporting.
});

test('CORS preflight works for USB hostname and rejects unrelated origins', async () => {
  const env = environment();
  const preflight = await worker.fetch(request(undefined, { method: 'OPTIONS', body: undefined }), env);
  assert.equal(preflight.status, 204);
  assert.equal(preflight.headers.get('Access-Control-Allow-Headers'), 'Content-Type');
  const rejected = await worker.fetch(request(undefined, {
    headers: { Origin: 'https://unrelated.example', 'Content-Type': 'application/json' },
  }), env);
  assert.equal(rejected.status, 403);
  assert.equal(rejected.headers.get('Access-Control-Allow-Origin'), null);
});

test('oversized, malformed, rate-limited and unavailable storage requests never acknowledge', async () => {
  const env = environment();
  assert.equal((await worker.fetch(request(undefined, { body: 'x'.repeat(2 * 1024 * 1024 + 1) }), env)).status, 413);
  assert.equal((await worker.fetch(request(undefined, { body: '{broken' }), env)).status, 400);
  const bad = fixture();
  bad.points[0].private_name = 'do not retain';
  assert.equal((await worker.fetch(request(bad), env)).status, 400);
  assert.equal(env.points.size, 0);
  env.setRate(false);
  assert.equal((await worker.fetch(request(), env)).status, 429);
  env.setRate(true); env.setDb(false);
  assert.equal((await worker.fetch(request(), env)).status, 503);
});

test('health endpoint exposes no reports and raw report URLs remain unavailable', async () => {
  const env = environment();
  const response = await worker.fetch(new Request('https://telemetry.audiox.wklab.net/health'), env);
  assert.equal(response.status, 200);
  assert.equal((await response.json()).schema, 3);
  assert.equal((await worker.fetch(new Request('https://telemetry.audiox.wklab.net/points'), env)).status, 404);
});

function sqliteEnvironment() {
  const db = new DatabaseSync(':memory:');
  for (const file of ['0001_points.sql', '0002_public_dashboard.sql'])
    db.exec(readFileSync(new URL(`./migrations/${file}`, import.meta.url), 'utf8'));
  const statements = [];
  const prepare = (sql, args = []) => ({
    bind(...values) { return prepare(sql, values); },
    async first() { statements.push(sql); return db.prepare(sql).get(...args) || null; },
    async all() { statements.push(sql); return { results: db.prepare(sql).all(...args) }; },
    async run() { statements.push(sql); db.prepare(sql).run(...args); return { success: true }; },
  });
  return {
    db, statements,
    ALLOWED_ORIGINS: 'http://audiox.local',
    INGEST_LIMIT: { limit: async () => ({ success: true }) },
    DB: { prepare, async batch(queries) {
      db.exec('BEGIN');
      try { for (const query of queries) await query.run(); db.exec('COMMIT'); }
      catch (error) { db.exec('ROLLBACK'); throw error; }
    } },
    ASSETS: { fetch: async () => new Response('<html>dashboard</html>', { headers: { 'Content-Type': 'text/html' } }) },
  };
}

test('real D1 inserts variable batches atomically and retries do not duplicate points', async () => {
  for (const count of [20, 120, 500, 1000]) {
    const env = sqliteEnvironment();
    const body = fixture(3, count);
    for (let retry = 0; retry < 2; retry++) {
      const response = await worker.fetch(request(body), env);
      assert.equal(response.status, 200);
      assert.deepEqual(await response.json(), { batch: body.batch });
    }
    assert.equal(env.db.prepare('SELECT COUNT(*) AS n FROM points').get().n, count);
    env.db.close();
  }
});

test('public snapshot allowlists fields, fills 30 days and never exports IDs or raw data', () => {
  const snapshot = publicSnapshot([{ day: '2026-11-06', points: 2, xruns: 4, cpu_percent: 5.555,
    installation_id: 'SECRET', payload: 'SECRET', version: 'PRIVATE', id: 'SECRET' }], new Date('2026-11-06T12:00:00Z'));
  assert.equal(snapshot.days.length, 30);
  assert.equal(snapshot.days.at(-1).cpu_percent, 5.56);
  assert.equal(snapshot.days[0].temperature_c, null);
  assert.equal(snapshot.days[0].points, 0);
  assert.doesNotMatch(JSON.stringify(snapshot), /SECRET|PRIVATE|installation_id|payload|"id"|"version"/);
  assert.throws(() => publicSnapshot([{ day: '2026-11-06', points: -1 }], new Date('2026-11-06T12:00:00Z')));
});

test('real aggregate SQL excludes older private reports, groups days and retains only coarse public fields', async () => {
  const env = sqliteEnvironment();
  const day = new Date().toISOString().slice(0, 10);
  const yesterday = new Date(Date.now() - 86400000).toISOString().slice(0, 10);
  for (const schema of [1, 2, 3]) {
    const point = fixture(schema).points[0];
    point.metrics.xruns = schema === 3 ? 5 : 999999;
    point.metrics.soundboard_triggers = 7;
    point.metrics.cpu_percent = 50;
    point.metrics.temperature_c = null;
    env.db.prepare('INSERT INTO points VALUES(?, ?, ?)').run(`point-${schema}`, day, JSON.stringify(point));
  }
  const point = fixture(3).points[1];
  point.version = 'private-custom-version';
  env.db.prepare('INSERT INTO points VALUES(?, ?, ?)').run('yesterday', yesterday, JSON.stringify(point));
  env.db.prepare('INSERT INTO points VALUES(?, ?, ?)').run('expired', '2000-01-01', JSON.stringify(point));
  await worker.scheduled({}, env);
  const snapshot = JSON.parse(env.db.prepare('SELECT payload FROM public_dashboard').get().payload);
  assert.equal(snapshot.days.length, 30);
  assert.equal(snapshot.days.at(-1).points, 1);
  assert.equal(snapshot.days.at(-1).xruns, 5);
  assert.equal(snapshot.days.at(-1).temperature_c, null);
  assert.equal(snapshot.days.at(-1).effect_3_0, 2);
  assert.equal(snapshot.days.at(-1).effect_3_1, 1);
  assert.equal(snapshot.days.at(-1).prerelease_style, 1);
  assert.equal(snapshot.days.at(-2).custom_style, 1);
  assert.equal(env.db.prepare('SELECT COUNT(*) AS n FROM points').get().n, 4);
  assert.doesNotMatch(JSON.stringify(snapshot), /installation_id|private-custom-version|cccccccc|"payload"/);
  await refreshDashboard(env); // Incremental refresh must not double count.
  const again = JSON.parse(env.db.prepare('SELECT payload FROM public_dashboard').get().payload);
  assert.equal(again.days.at(-1).points, 1);
  const before = env.statements.length;
  const response = await worker.fetch(new Request('https://telemetry.audiox.wklab.net/api/public/summary'), env);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('Access-Control-Allow-Origin'), '*');
  assert.deepEqual(env.statements.slice(before), ['SELECT payload FROM public_dashboard WHERE singleton = 1']);
  assert.doesNotMatch(await response.text(), /installation_id|private-custom-version/);
  const head = await worker.fetch(new Request('https://test/api/public/summary', { method: 'HEAD' }), env);
  assert.equal(head.status, 200);
  assert.equal(await head.text(), '');
  env.db.close();
});

test('database read failure returns an explicit unavailable response without leaking storage details', async () => {
  const env = { DB: { prepare() { throw new Error('private storage detail'); } } };
  const response = await worker.fetch(new Request('https://test/api/public/summary'), env);
  assert.equal(response.status, 503);
  assert.equal(response.headers.get('Cache-Control'), 'no-store');
  assert.deepEqual(await response.json(), { error: 'Dashboard unavailable.' });
});

test('dashboard rejects query filters, raw endpoints and stale snapshots; missing snapshot is explicit', async () => {
  const env = sqliteEnvironment();
  for (const query of ['?installation_id=secret', '?sql=SELECT', '?from=2000-01-01']) {
    assert.equal((await worker.fetch(new Request(`https://test/api/public/summary${query}`), env)).status, 400);
  }
  assert.equal((await worker.fetch(new Request('https://test/api/public/summary'), env)).status, 503);
  assert.equal((await worker.fetch(new Request('https://test/api/public/summary', { method: 'POST' }), env)).status, 405);
  env.db.prepare('INSERT INTO public_dashboard VALUES(1, ?)').run(JSON.stringify({
    format: 'audiox-public-telemetry-v1', updated_at: '2000-01-01T00:00:00Z', days: [],
  }));
  assert.equal((await worker.fetch(new Request('https://test/api/public/summary'), env)).status, 503);
  const page = await worker.fetch(new Request('https://test/'), env);
  assert.equal(page.status, 200);
  assert.match(page.headers.get('Content-Security-Policy'), /default-src 'none'/);
  for (const path of ['/api/public/raw', '/api/public/installations', '/points', '/dashboard/secret']) {
    assert.equal((await worker.fetch(new Request(`https://test${path}`), env)).status, 404);
  }
  env.db.close();
});
