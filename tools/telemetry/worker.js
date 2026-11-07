import { dashboardRequest, refreshDashboard } from './dashboard.js';

const metricLimits = {
  usb_audio: 4096, gadget_audio: 4096, other_audio: 4096, bluetooth_audio: 4096,
  midi_connected: 1, input_devices: 4096, output_devices: 4096,
  soundboard_triggers: 0xffffffff, midi_notes: 0xffffffff, midi_cc: 0xffffffff,
  scene_recalls: 0xffffffff, sampler_enabled: 1, xruns: 0xffffffff,
  sample_rate: 384000, block_frames: 65536, temperature_c: 150, cpu_percent: 100,
};
const idPattern = /^[0-9a-f]{32}$/;
const maxBytes = 2 * 1024 * 1024;
const exactKeys = (object, keys) => object !== null && typeof object === 'object' &&
  !Array.isArray(object) && Object.keys(object).length === keys.length &&
  keys.every((key) => Object.hasOwn(object, key));
const integer = (value, max) => Number.isSafeInteger(value) && value >= 0 && value <= max;

export function validBatch(body) {
  if (!exactKeys(body, ['schema', 'batch', 'points']) || ![1, 2, 3].includes(body.schema) ||
      typeof body.batch !== 'string' || !Array.isArray(body.points) ||
      (body.schema === 3 ? body.points.length < 1 || body.points.length > 1000 : body.points.length !== 100)) return false;
  const seen = new Set();
  const pointKeys = ['id', 'version', 'board', 'metrics', 'effects'];
  if (body.schema >= 2) pointKeys.push('installation_id', 'interval_ms');
  if (body.schema === 3) pointKeys.push('public_consent');
  for (const point of body.points) {
    if (!exactKeys(point, pointKeys) ||
        typeof point.id !== 'string' || !idPattern.test(point.id) || seen.has(point.id) ||
        typeof point.version !== 'string' || !/^[0-9A-Za-z.+-]{1,96}$/.test(point.version) ||
        ![0, 4, 5].includes(point.board) ||
        !exactKeys(point.metrics, [...Object.keys(metricLimits), ...(body.schema >= 2 ? ['audio_active'] : [])]) ||
        !Array.isArray(point.effects) || point.effects.length !== 12) return false;
    if (body.schema >= 2 && (typeof point.installation_id !== 'string' ||
        !/^[0-9a-f]{64}$/.test(point.installation_id) || point.interval_ms !== 10000 ||
        point.installation_id !== body.points[0].installation_id)) return false;
    if (body.schema >= 2 && !integer(point.metrics.audio_active, 1)) return false;
    if (body.schema === 3 && point.public_consent !== 1) return false;
    seen.add(point.id);
    for (const [key, max] of Object.entries(metricLimits)) {
      const value = point.metrics[key];
      if (value === null && (key === 'temperature_c' || key === 'cpu_percent')) continue;
      if (!integer(value, max)) return false;
    }
    for (const pair of point.effects) {
      if (!Array.isArray(pair) || pair.length !== 2 ||
          !integer(pair[0], 4096) || !integer(pair[1], pair[0])) return false;
    }
  }
  return body.batch === `${body.points[0].id}:${body.points.at(-1).id}`;
}

async function boundedBody(request) {
  if (!request.body) throw new Error('Missing body');
  const reader = request.body.getReader();
  const chunks = [];
  let length = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      length += value.byteLength;
      if (length > maxBytes) {
        await reader.cancel();
        return null;
      }
      chunks.push(value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(length);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
  return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const dashboard = await dashboardRequest(request, env);
    if (dashboard) return dashboard;
    if (url.pathname === '/health' && request.method === 'GET') {
      return Response.json({ service: 'audiox-telemetry', schema: 3 }, {
        headers: { 'Cache-Control': 'no-store' },
      });
    }
    if (url.pathname !== '/v1/batches') return new Response('Not found', { status: 404 });
    const origin = request.headers.get('Origin');
    const allowed = (env.ALLOWED_ORIGINS || '').split(',');
    if (!origin || !allowed.includes(origin)) return new Response('Origin not allowed', { status: 403 });
    const headers = {
      'Access-Control-Allow-Origin': origin,
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
      'Access-Control-Max-Age': '3600',
      'Cache-Control': 'no-store',
      'Vary': 'Origin',
    };
    const reply = (value, status = 200) => Response.json(value, { status, headers });
    if (request.method === 'OPTIONS') return new Response(null, { status: 204, headers });
    if (request.method !== 'POST') return reply({ error: 'Method not allowed' }, 405);
    if (request.headers.get('Content-Type')?.split(';')[0].trim() !== 'application/json') {
      return reply({ error: 'Expected application/json' }, 415);
    }
    // IP is used transiently for abuse protection, never written to D1 or application logs.
    const { success } = await env.INGEST_LIMIT.limit({ key: request.headers.get('CF-Connecting-IP') || 'unknown' });
    if (!success) return reply({ error: 'Rate limited' }, 429);
    let body;
    try {
      const text = await boundedBody(request);
      if (text === null) return reply({ error: 'Body too large' }, 413);
      body = JSON.parse(text);
    } catch (error) {
      if (error instanceof SyntaxError || error instanceof TypeError || error.message === 'Missing body') {
        return reply({ error: 'Invalid JSON body' }, 400);
      }
      throw error;
    }
    if (!validBatch(body)) return reply({ error: 'Invalid telemetry schema' }, 400);
    try {
      // Insert the entire batch with one SQL statement (the free plan limits queries per request).
      await env.DB.prepare(
        "INSERT OR IGNORE INTO points(id, received_day, payload) " +
        "SELECT json_extract(value, '$.id'), date('now'), value FROM json_each(?)"
      ).bind(JSON.stringify(body.points)).run();
    } catch {
      console.error('Telemetry database insert failed'); // Do not log request data or headers.
      return reply({ error: 'Storage unavailable' }, 503);
    }
    return reply({ batch: body.batch });
  },
  async scheduled(_event, env) {
    await env.DB.prepare("DELETE FROM points WHERE received_day <= date('now', '-30 days')").run();
    await refreshDashboard(env);
  },
};
