const metricFields = {
  usb_audio: 'AVG', gadget_audio: 'AVG', other_audio: 'AVG', bluetooth_audio: 'AVG',
  midi_connected: 'AVG', input_devices: 'AVG', output_devices: 'AVG',
  soundboard_triggers: 'SUM', midi_notes: 'SUM', midi_cc: 'SUM', scene_recalls: 'SUM',
  xruns: 'SUM', temperature_c: 'AVG', cpu_percent: 'AVG', audio_active: 'SUM',
};
const columns = [
  'COUNT(*) AS points',
  ...Object.entries(metricFields).map(([field, aggregate]) =>
    `${aggregate}(json_extract(payload, '$.metrics.${field}')) AS ${field}`),
  ...Array.from({ length: 12 }, (_, i) => [0, 1].map((j) =>
    `AVG(json_extract(payload, '$.effects[${i}][${j}]')) AS effect_${i}_${j}`)).flat(),
  "SUM(json_extract(payload, '$.board') = 4) AS pi4",
  "SUM(json_extract(payload, '$.board') = 5) AS pi5",
  "SUM(json_extract(payload, '$.board') = 0) AS unknown_board",
  // Deliberately never export version strings, even for custom firmware.
  "SUM(json_extract(payload, '$.version') GLOB '[0-9]*.[0-9]*.[0-9]*' AND instr(json_extract(payload, '$.version'), '-') = 0) AS release_style",
  "SUM(json_extract(payload, '$.version') GLOB '[0-9]*.[0-9]*.[0-9]*' AND instr(json_extract(payload, '$.version'), '-') > 0) AS prerelease_style",
  "SUM(NOT (json_extract(payload, '$.version') GLOB '[0-9]*.[0-9]*.[0-9]*')) AS custom_style",
];

export const aggregateSql = `SELECT received_day AS day, ${columns.join(', ')}
  FROM points
  WHERE received_day >= ? AND received_day <= date('now')
    AND json_extract(payload, '$.public_consent') = 1
  GROUP BY received_day ORDER BY received_day`;

const publicFields = ['points', ...Object.keys(metricFields), ...Array.from({ length: 12 }, (_, i) =>
  [0, 1].map((j) => `effect_${i}_${j}`)).flat(),
'pi4', 'pi5', 'unknown_board', 'release_style', 'prerelease_style', 'custom_style'];

export function publicSnapshot(rows, now = new Date()) {
  const firstDay = new Date(now);
  firstDay.setUTCDate(firstDay.getUTCDate() - 29);
  const days = new Map(rows.map((row) => [row.day, row]));
  const result = [];
  for (let i = 0; i < 30; i++) {
    const date = new Date(firstDay);
    date.setUTCDate(date.getUTCDate() + i);
    const day = date.toISOString().slice(0, 10);
    const row = days.get(day);
    const entry = { day };
    for (const field of publicFields) {
      const value = row?.[field];
      if (value !== undefined && value !== null && (!Number.isFinite(value) || value < 0))
        throw new Error('Invalid public aggregate');
      entry[field] = value === undefined || value === null
        ? (field === 'temperature_c' || field === 'cpu_percent' ? null : 0)
        : Math.round(value * 100) / 100;
    }
    result.push(entry);
  }
  return { format: 'audiox-public-telemetry-v1', updated_at: now.toISOString(), days: result };
}

export async function refreshDashboard(env) {
  const now = new Date();
  const existing = await env.DB.prepare('SELECT day FROM public_daily LIMIT 1').first();
  const start = new Date(now);
  // Only receipt dates today/yesterday can acquire new rows. Reuse older daily aggregates.
  start.setUTCDate(start.getUTCDate() - (existing ? 1 : 29));
  const result = await env.DB.prepare(aggregateSql).bind(start.toISOString().slice(0, 10)).all();
  const recent = publicSnapshot(result.results, now).days.filter((day) => day.points > 0);
  await env.DB.batch([
    env.DB.prepare("DELETE FROM public_daily WHERE day <= date('now', '-30 days') OR day >= ?")
      .bind(start.toISOString().slice(0, 10)),
    ...recent.map((day) => env.DB.prepare('INSERT INTO public_daily(day, payload) VALUES(?, ?)')
      .bind(day.day, JSON.stringify(day))),
  ]);
  const retained = await env.DB.prepare('SELECT payload FROM public_daily ORDER BY day LIMIT 30').all();
  const snapshot = publicSnapshot(retained.results.map((row) => JSON.parse(row.payload)), now);
  await env.DB.prepare(
    'INSERT INTO public_dashboard(singleton, payload) VALUES(1, ?) ' +
    'ON CONFLICT(singleton) DO UPDATE SET payload = excluded.payload'
  ).bind(JSON.stringify(snapshot)).run();
}

const securityHeaders = {
  'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'",
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
};

export async function dashboardRequest(request, env) {
  const url = new URL(request.url);
  if (!['/', '/dashboard.js', '/dashboard.css', '/api/public/summary'].includes(url.pathname)) return null;
  if (request.method !== 'GET' && request.method !== 'HEAD')
    return new Response('Method not allowed', { status: 405, headers: { Allow: 'GET, HEAD' } });
  if (url.search) return new Response('Query parameters are not supported', { status: 400 });
  if (url.pathname !== '/api/public/summary') {
    const response = await env.ASSETS.fetch(request);
    const headers = new Headers(response.headers);
    for (const [name, value] of Object.entries(securityHeaders)) headers.set(name, value);
    return new Response(response.body, { status: response.status, headers });
  }
  // Visitors can only read the precomputed public snapshot, never the source reports.
  try {
    const row = await env.DB.prepare('SELECT payload FROM public_dashboard WHERE singleton = 1').first();
    if (!row) return Response.json({ error: 'Dashboard is awaiting its first hourly refresh.' },
      { status: 503, headers: { ...securityHeaders, 'Cache-Control': 'no-store', 'Retry-After': '300' } });
    const snapshot = JSON.parse(row.payload);
    const age = Date.now() - Date.parse(snapshot.updated_at);
    if (!Number.isFinite(age) || age < -60000 || age > 2 * 3600000)
      return Response.json({ error: 'Dashboard refresh is delayed; try again later.' },
        { status: 503, headers: { ...securityHeaders, 'Cache-Control': 'no-store' } });
    return new Response(request.method === 'HEAD' ? null : row.payload, {
      headers: { ...securityHeaders, 'Content-Type': 'application/json',
        'Cache-Control': 'public, max-age=300', 'Access-Control-Allow-Origin': '*' },
    });
  } catch {
    console.error('Public dashboard read failed');
    return Response.json({ error: 'Dashboard unavailable.' },
      { status: 503, headers: { ...securityHeaders, 'Cache-Control': 'no-store' } });
  }
}
