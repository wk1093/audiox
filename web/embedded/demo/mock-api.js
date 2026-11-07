// Live Demo replacement for api.js. Implements the same AudioxApi gateway, but
// answers every /api/ request from an in-browser simulated AudioX device.
(function (root) {
  'use strict';

  const data = root.AudioxDemoData;
  const S = data.create();
  const startedAt = Date.now();
  const typeLabels = {
    gain: 'Gain', distortion: 'Distortion', pitch: 'Pitch Shift', reverb: 'Reverb', gate: 'Noise Gate',
    cut: 'Mute', compressor: 'Compressor', limiter: 'Limiter', eq: 'Parametric EQ',
    multiband: 'Multiband Compressor', noise_reduction: 'Noise Reduction',
  };

  // ---------------------------------------------------------------- helpers
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const rand = (lo, hi) => lo + Math.random() * (hi - lo);
  const pick = (list) => list[Math.floor(Math.random() * list.length)];
  const dbToLin = (db) => 10 ** (db / 20);
  const linToDb = (v) => 20 * Math.log10(Math.max(v, 1e-6));
  const hex = (text) => Array.from(new TextEncoder().encode(text), (b) => b.toString(16).padStart(2, '0')).join('');
  const unhex = (h) => new TextDecoder().decode(Uint8Array.from(String(h || '').match(/../g) || [], (b) => parseInt(b, 16)));
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const uptime = () => Math.max(0, Math.floor((Date.now() - S.bootAt) / 1000));

  function reply(body, status = 200, type) {
    const isJson = typeof body === 'object' && body !== null;
    return new Response(isJson ? `${JSON.stringify(body)}\n` : String(body ?? ''), {
      status,
      headers: { 'Content-Type': type || (isJson ? 'application/json; charset=utf-8' : 'text/plain; charset=utf-8') },
    });
  }
  const ok = (extra = {}) => reply({ ok: true, ...extra });
  const fail = (message, status = 400) => reply(`${message}\n`, status);

  async function bodyText(body) {
    if (body === undefined || body === null) return '';
    if (typeof body === 'string') return body;
    if (body instanceof URLSearchParams) return body.toString();
    if (body instanceof Blob) return body.text();
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return new TextDecoder().decode(body);
    return String(body);
  }
  const bodySize = (body) => body?.size ?? body?.byteLength ?? String(body ?? '').length;
  const form = (text) => Object.fromEntries(new URLSearchParams(text));

  // ---------------------------------------------------------------- logs
  const logRows = [];
  let logId = 0;
  function log(tag, level, line, source = level === 'error' || level === 'warn' ? 'stderr' : 'stdout') {
    logRows.push({ id: String(++logId), source, level, line: `[${tag}] [${level.toUpperCase()}] ${line}`, truncated: false });
    if (logRows.length > 4000) logRows.splice(0, logRows.length - 4000);
  }
  [
    ['MAIN', 'info', `AudioX ${S.version} starting (board pi5, Raspberry Pi 5 Model B Rev 1.0)`],
    ['CONFIG', 'info', 'loaded /data/config.txt (8 keys)'],
    ['USB', 'info', 'configured gadget: UAC2 2ch/2ch 48000 Hz s16le + MIDI + HID keyboard'],
    ['AUDIO', 'info', 'scanning /dev/snd: 3 PCM devices'],
    ['AUDIO', 'info', 'capture stream opened alsa_usb-1235-8210-s3f9a1c2d7e4b6a80_dev0_in (2ch, 48000 Hz, MMAP period=128)'],
    ['AUDIO', 'info', 'playback stream opened alsa_usb-1235-8210-s3f9a1c2d7e4b6a80_dev0_out (2ch, 48000 Hz, MMAP period=128)'],
    ['AUDIO', 'info', 'playback stream opened alsa_vc4hdmi0_dev0_out (2ch, 48000 Hz, MMAP period=256)'],
    ['PLUGIN', 'info', 'loaded tape_echo 1.3.0 (6 parameters)'],
    ['AUDIO', 'info', 'routing graph ready: 15 things, 28 edges, 0 skipped'],
    ['SOUNDBOARD', 'info', `preloaded ${S.sounds.length} clips (31.4 MiB)`],
    ['MIDI', 'info', 'controller connected: Akai MPD218 (hw:4,0,0)'],
    ['BT', 'info', 'adapter hci0 ready (BlueZ-free stack, 1 link)'],
    ['BT', 'info', 'connected JBL Flip 6 (F8:5C:7D:12:A4:3B) A2DP sink, SBC 48000 Hz bitpool 53'],
    ['HTTP', 'info', 'listening on 0.0.0.0:80'],
    ['SCRIPT', 'info', 'compiled 3 scripts (6 hooks)'],
    ['AUDIO', 'warn', 'xrun on usb_gadget_out; raising period to 128 frames'],
    ['SCRIPT', 'info', 'Script 3 (Startup Volume): boot hook queued 1 action'],
    ['SYSTEM', 'info', 'fan curve active: 50/60/67/75 °C → PWM 75/125/175/250'],
  ].forEach(([tag, level, line]) => log(tag, level, line));

  // ---------------------------------------------------------------- routing model
  const routingHasHelper = () => /^node=routing_helper\s*$/m.test(S.routingText);
  const effectNumber = (id) => Number(String(id).replace(/^fx_/, ''));
  const plugin = (type) => S.plugins.find((p) => p.id === type);
  const effectLabel = (fx) => fx.name || `${plugin(fx.type)?.name || typeLabels[fx.type] || 'Gain'} ${effectNumber(fx.id)}`;

  function specFor(type) {
    const p = plugin(type);
    if (p) return p.params.map((param) => [param.name, param.label, param.min, param.max, param.default, param]);
    return data.effectSpecs[type] || data.effectSpecs.gain;
  }
  function paramValue(fx, name) {
    const spec = specFor(fx.type).find(([n]) => n === name);
    return Number.isFinite(fx.params[name]) ? fx.params[name] : spec?.[4] ?? 0;
  }

  function bluetoothThings() {
    const out = [];
    for (const device of S.bluetooth) {
      if (!device.connected) continue;
      if (device.audio_source) out.push({ id: data.btThing(device.address, 'in'), name: `From ${device.name} (Bluetooth)`, inputs: 0, outputs: 2 });
      if (device.audio_sink) out.push({ id: data.btThing(device.address, 'out'), name: `To ${device.name} (Bluetooth)`, inputs: 2, outputs: 0 });
    }
    return out;
  }

  function things() {
    const list = [S.devices[0]];
    if (routingHasHelper()) list.push({ id: 'routing_helper', name: 'Audio Mixer', inputs: 2, outputs: 2 });
    for (const fx of S.effects) {
      const channels = fx.type === 'cut' ? clamp(Math.round(paramValue(fx, 'channels')), 1, 16) : 2;
      list.push({ id: fx.id, name: effectLabel(fx), inputs: channels, outputs: channels });
    }
    list.push(...S.devices.slice(1), ...bluetoothThings());
    return list;
  }

  function parsedEdges() {
    const byId = new Map(things().map((t) => [t.id, t]));
    const edges = [];
    let skipped = 0;
    for (const line of S.routingText.split(/\r?\n/)) {
      if (!line.startsWith('edge=')) continue;
      const [src, dst, sc, dc] = line.slice(5).split(',').map((s) => s.trim());
      const a = byId.get(src), b = byId.get(dst);
      const srcCh = Number(sc), dstCh = Number(dc);
      if (!a || !b || !(srcCh < a.outputs) || !(dstCh < b.inputs)) { skipped += 1; continue; }
      edges.push({ src, dst, srcCh, dstCh });
    }
    return { edges, skipped };
  }

  // ---------------------------------------------------------------- signal simulation
  const voices = [];
  const tails = new Map();
  let lastMeterAt = performance.now() / 1000;

  function sourceLevel(kind, t, ch) {
    return 0.45 * rawSourceLevel(kind, t, ch);
  }
  function rawSourceLevel(kind, t, ch) {
    const jitter = rand(0.85, 1.15);
    switch (kind) {
      case 'voice': case 'voice2': case 'podcast': {
        const o = kind === 'voice' ? 0 : kind === 'voice2' ? 17 : 41;
        const phrase = Math.sin(0.45 * t + o) + 0.6 * Math.sin(1.13 * t + 2 * o) + 0.35 * Math.sin(2.7 * t + o);
        if (phrase < 0.15) return rand(0.004, 0.012);
        const syllable = 0.55 + 0.45 * Math.abs(Math.sin(5.3 * t + o + ch));
        const scale = kind === 'podcast' ? 0.42 : kind === 'voice2' ? 0.5 : 0.62;
        return clamp(scale * syllable * Math.min(1, phrase) * jitter, 0, 1);
      }
      case 'guitar': {
        const beat = (t * 1.1) % 2;
        const strum = Math.exp(-2.2 * (beat % 1)) * (beat < 1 ? 1 : 0.7);
        const section = Math.sin(0.11 * t) > -0.35 ? 1 : 0.05;
        return clamp((0.08 + 0.55 * strum) * section * jitter, 0, 1);
      }
      case 'music': {
        const kick = Math.exp(-7 * ((t * 2) % 1));
        return clamp((0.32 + 0.22 * kick + 0.06 * Math.sin(0.3 * t)) * jitter * (ch ? 0.96 : 1), 0, 1);
      }
      default: return 0;
    }
  }

  function soundboardLevel(now) {
    for (let i = voices.length - 1; i >= 0; i -= 1) if (voices[i].end <= now) voices.splice(i, 1);
    return voices.reduce((sum, v) => {
      const age = (now - v.start) / 1000;
      return sum + 0.3 * Math.min(1, age * 12) * (0.75 + 0.25 * Math.exp(-age)) * rand(0.88, 1.08);
    }, 0);
  }

  function processEffect(fx, input, dt) {
    const p = (name) => paramValue(fx, name);
    if (fx.type === 'cut') return input.map(() => (fx.enabled ? 0 : null));
    if (!fx.enabled) return input.map(() => null);
    const tail = tails.get(fx.id) || input.map(() => 0);
    const out = input.map((x, ch) => {
      switch (fx.type) {
        case 'gain': return x * p('level');
        case 'distortion': return Math.tanh(x * p('input_gain') * (1 + p('drive'))) * 0.75 * p('output_gain');
        case 'pitch': return x * p('input_gain') * p('output_gain');
        case 'gate': return linToDb(x) < p('threshold_db') ? x * dbToLin(-p('range_db')) : x;
        case 'noise_reduction': return (linToDb(x) < p('noise_floor_db') + 12 ? x * dbToLin(-p('reduction_db')) : x) * p('output_gain');
        case 'eq': return x * p('output_gain') * dbToLin((p('low_gain_db') + p('mid_gain_db') + p('high_gain_db')) / 6);
        case 'limiter': return Math.min(x * p('input_gain'), dbToLin(p('ceiling_db')));
        case 'compressor': case 'multiband': {
          const threshold = fx.type === 'compressor' ? p('threshold_db') : p('mid_threshold_db');
          const db = linToDb(x);
          const over = Math.max(0, db - threshold);
          return dbToLin(db - over + over / p('ratio') + p('makeup_db'));
        }
        case 'reverb': case 'tape_echo': {
          const feedback = p('feedback');
          const wet = fx.type === 'reverb' ? p('wet_mix') : p('mix');
          tail[ch] = Math.max(x, (tail[ch] || 0) * feedback ** (dt * 3));
          return (1 - wet) * x + wet * tail[ch];
        }
        default: return x;
      }
    });
    tails.set(fx.id, tail);
    return out;
  }

  function meters() {
    const nowS = performance.now() / 1000;
    const dt = clamp(nowS - lastMeterAt, 0.01, 1);
    lastMeterAt = nowS;
    const list = things();
    const { edges } = parsedEdges();
    const effects = new Map(S.effects.map((fx) => [fx.id, fx]));
    const out = new Map();
    const inputs = new Map(list.map((t) => [t.id, new Array(t.inputs).fill(0)]));
    for (const thing of list) {
      if (thing.inputs) continue;
      const kinds = S.signals[thing.id];
      const sb = thing.id === 'soundboard_out' ? soundboardLevel(Date.now()) : 0;
      out.set(thing.id, Array.from({ length: thing.outputs }, (_, ch) =>
        thing.id === 'soundboard_out' ? sb : kinds ? sourceLevel(kinds[ch] || kinds[0], nowS, ch) : 0));
    }
    // Relax the (acyclic) graph a few times; deep chains settle within the pass count.
    for (let pass = 0; pass < 12; pass += 1) {
      for (const level of inputs.values()) level.fill(0);
      for (const e of edges) {
        const source = out.get(e.src);
        if (!source) continue;
        // Uncorrelated signals add in power, not amplitude.
        inputs.get(e.dst)[e.dstCh] += ((source[e.srcCh] || 0) * (S.gains[e.src] ?? 1)) ** 2;
      }
      for (const level of inputs.values()) level.forEach((x, ch) => { level[ch] = Math.sqrt(x); });
      for (const thing of list) {
        if (!thing.inputs || !thing.outputs) continue;
        const input = inputs.get(thing.id);
        const fx = effects.get(thing.id);
        const processed = fx ? processEffect(fx, input, pass === 11 ? dt : 0) : input;
        out.set(thing.id, input.map((x, ch) => processed[ch] ?? x));
      }
    }
    return list.map((thing) => {
      const raw = thing.outputs ? out.get(thing.id) : inputs.get(thing.id);
      return { id: thing.id, levels: (raw || []).slice(0, 16).map((v) => Number(clamp(v || 0, 0, 1).toFixed(4))) };
    });
  }

  // ---------------------------------------------------------------- MIDI simulation
  const midi = { note: -1, velocity: 0, channel: 9, noteSeq: 0, cc: -1, ccValue: 0, ccSeq: 0 };
  const knobs = { 1: 64, 2: 90, 3: 92, 4: 101, 5: 127, 6: 48, 7: 41, 8: 88 };

  function playSound(name, now = Date.now()) {
    const sound = S.sounds.find((s) => s.name === name);
    if (!sound) return false;
    voices.push({ name, start: now, end: now + sound.seconds * 1000 });
    log('SOUNDBOARD', 'info', `play ${name} (${S.modes[name] || 'play'})`);
    return true;
  }

  function simulatePad() {
    const mapped = S.mappings.filter((m) => S.modes[m.sfx] !== 'hold');
    if (mapped.length) {
      const m = pick(mapped);
      midi.note = m.note;
      midi.velocity = Math.floor(rand(70, 127));
      midi.noteSeq += 1;
      playSound(m.sfx);
    }
    setTimeout(simulatePad, rand(9000, 16000));
  }
  function simulateKnob() {
    const cc = pick(Object.keys(knobs).map(Number));
    knobs[cc] = clamp(knobs[cc] + Math.round(rand(-9, 9)), 0, 127);
    midi.cc = cc;
    midi.ccValue = knobs[cc];
    midi.ccSeq += 1;
    setTimeout(simulateKnob, rand(2500, 6000));
  }
  setTimeout(simulatePad, 5000);
  setTimeout(simulateKnob, 3000);

  // ---------------------------------------------------------------- system telemetry
  const history = [];
  const runtimeId = String(Math.floor(rand(1e15, 9e15)));
  let historyId = 0;
  let xruns = 3;
  let cpuTicks = { total: 7_215_400_000, idle: 6_204_000_000 };
  let stress = { mode: 'idle', until: 0, workers: 0 };
  const monotonic = () => Math.round(performance.now()) + 9_000_000;

  function cpuNow() {
    const stressed = stress.mode !== 'idle' && Date.now() < stress.until;
    return clamp(16 + 5 * Math.sin(Date.now() / 47000) + rand(-3, 3) + (stressed ? 55 : 0), 2, 99);
  }
  const temperatureNow = () => 49 + 3 * Math.sin(Date.now() / 90000) + cpuNow() * 0.12 + rand(-0.4, 0.4);

  function seedHistory() {
    const now = monotonic();
    for (let i = 599; i >= 0; i -= 1) {
      const t = Date.now() - i * 1000;
      const cpu = clamp(16 + 5 * Math.sin(t / 47000) + rand(-3, 3) + (i > 260 && i < 290 ? 30 : 0), 2, 99);
      if (i === 275 || i === 274) xruns += 1;
      history.push({ id: ++historyId, now: now - i * 1000,
        temperature: Number((49 + 3 * Math.sin(t / 90000) + cpu * 0.12).toFixed(1)), cpu: Number(cpu.toFixed(1)), xruns: i < 275 ? xruns : xruns - 2 });
    }
  }
  seedHistory();
  setInterval(() => {
    if (stress.mode !== 'idle' && Date.now() >= stress.until) {
      log('SYSTEM', 'info', `stress test (${stress.mode}) finished`);
      stress = { mode: 'idle', until: 0, workers: 0 };
    }
    if (stress.mode === 'audio' && Math.random() < 0.5) {
      xruns += 1;
      log('AUDIO', 'warn', 'xrun on usb_gadget_out (audio thread overrun)');
    }
    const cpu = cpuNow();
    history.push({ id: ++historyId, now: monotonic(), temperature: Number(temperatureNow().toFixed(1)), cpu: Number(cpu.toFixed(1)), xruns });
    if (history.length > 600) history.shift();
    const delta = 400;
    cpuTicks = { total: cpuTicks.total + delta, idle: cpuTicks.idle + Math.round(delta * (1 - cpu / 100)) };
    if (Math.random() < 0.12) {
      log(...pick([
        ['HTTP', 'debug', 'GET /api/audio/meters 200 (1.1 KiB)'],
        ['MIDI', 'debug', `cc ${midi.cc} = ${midi.ccValue} (ch 10)`],
        ['AUDIO', 'info', 'adaptive buffer: holding 128 frames (no xruns in 60 s)'],
        ['BT', 'debug', 'JBL Flip 6: A2DP link quality 92%, rssi -54 dBm'],
        ['SCENE', 'debug', `active scene: ${S.scenes.find((s) => s.id === S.activeScene)?.name || 'none'}`],
        ['SYSTEM', 'debug', `cpu ${cpu.toFixed(1)}% temp ${temperatureNow().toFixed(1)} °C fan ${fanRpm()} RPM`],
      ]));
    }
  }, 1000);

  const fanRpm = () => Math.round(2950 + 300 * Math.sin(Date.now() / 30000) + rand(-60, 60));

  function systemInfo() {
    const totalStorage = 29_542_043_648;
    const reserved = 1_476_395_008;
    const used = 4_812_734_464 + S.sounds.length * 2_400_000;
    const curve = (c) => ({ status: 'available', temperatures_c: c.temperatures, speeds_pwm: c.speeds, hysteresis_c: c.hysteresis });
    const stressed = stress.mode !== 'idle' && Date.now() < stress.until;
    return {
      version: S.version,
      kernel: '6.12.34-v8-16k+ #1 SMP PREEMPT',
      board: 'pi5',
      model: 'Raspberry Pi 5 Model B Rev 1.0',
      cpu_freq_mhz: stressed ? 2400 : Math.round(pick([1500, 1800, 2400])),
      cpu_count: 4,
      cpu_times: { total_ticks: cpuTicks.total, idle_ticks: cpuTicks.idle },
      temperature_c: Number(temperatureNow().toFixed(1)),
      uptime_secs: uptime(),
      mem_total_mb: 8048,
      mem_avail_mb: Math.round(7310 + rand(-20, 20)),
      load1: Number((0.42 + cpuNow() / 60).toFixed(2)),
      ffmpeg: 'ffmpeg 7.1.1 (static, aarch64)',
      storage: { total_bytes: totalStorage, used_bytes: used, free_bytes: totalStorage - used - reserved, reserved_bytes: reserved },
      hardware: {
        undervoltage: { status: 'available', current: false, since_boot: false },
        throttling: { status: 'available', current: false, since_boot: false },
        soft_temperature_limit: { status: 'available', current: false, since_boot: false },
        thermal_throttling: { status: 'available', current: false, since_boot: false },
        fan: {
          module_status: 'available', device_status: 'available', driver_status: 'available',
          rpm_status: 'available', rpm: fanRpm(), cooling_status: 'available', cooling_state: 1, cooling_max_state: 4,
          pwm_status: 'available', pwm: 75, cooling_requested: true, stall_status: 'available', stall_warning: false,
          saved_curve: curve(S.fan), active_curve: curve(S.fan), saved_matches_active: true,
        },
      },
      audio_health: { xruns, warning_id: '0', warning_active: false },
      audio_buffer: {
        base_block_frames: 64, target_period_frames: 128, largest_period_frames: 256, largest_buffer_frames: 768,
        active_streams: 8, estimated_largest_buffer_ms: 16, level: 0, downshift_delay_ms: 60000, downshift_failures: 0,
        runtime_id: runtimeId, change_id: '0', increase_id: '0',
      },
      audio_stress_test: { available: true, mode: stressed ? stress.mode : 'idle',
        remaining_ms: stressed ? stress.until - Date.now() : 0, workers: stress.workers },
    };
  }

  // ---------------------------------------------------------------- JSON views
  function effectJson(fx) {
    const spec = specFor(fx.type);
    const meta = plugin(fx.type);
    const ccs = S.effectCc[fx.id] || {};
    const [enabledVel, bypassedVel] = S.effectLights[fx.id] || [0, 0];
    return {
      id: fx.id, name: effectLabel(fx), custom_name: fx.name || '', type: fx.type, enabled: fx.enabled,
      params: Object.fromEntries(spec.map(([name]) => [name, paramValue(fx, name)])),
      param_meta: spec.map(([name, label, min, max, def, extra]) => ({
        name, label, min, max, default: def,
        ...(extra ? { type: extra.type, unit: extra.unit, description: extra.description, step: extra.step, options: extra.options } : {}),
      })),
      ...(meta ? { text_controls: [] } : {}),
      midi: {
        toggle_note: S.effectToggle[fx.id] ?? -1,
        cc: Object.fromEntries(spec.map(([name]) => [name, ccs[name] ?? -1])),
        light: { enabled_vel: enabledVel, bypassed_vel: bypassedVel },
      },
      ...(meta ? { plugin_ui: false, plugin_status: 'ready', plugin_error: '' } : {}),
    };
  }

  function bluetoothJson() {
    const devices = S.bluetooth.map((d) => {
      const out = d.connected && d.audio_sink, input = d.connected && d.audio_source;
      return {
        address: d.address, name: d.name, paired: !!d.paired, classic: true, connecting: !!d.connecting,
        connected: !!d.connected, streaming: !!d.connected, output_connected: !!out, output_streaming: !!out,
        input_connected: !!input, input_streaming: !!input, auto_connect: !!(d.paired && d.auto_connect),
        audio_source: !!d.audio_source, audio_sink: !!d.audio_sink,
      };
    });
    const any = (key) => devices.some((d) => d[key]);
    return {
      ok: true, ready: true, scanning: !!S.btScanning, pairing: !!S.btPairing, connecting: any('connecting'),
      connected: any('connected'), streaming: any('streaming'), output_connected: any('output_connected'),
      output_streaming: any('output_streaming'), input_connected: any('input_connected'),
      input_streaming: any('input_streaming'), max_connections: 2,
      message: S.btScanning ? 'Searching for nearby audio devices...' : '', devices,
    };
  }

  function scriptJson(script) {
    return {
      channel: script.channel, note: script.note, keys: '', id: script.id, midi_enabled: script.midi_enabled,
      name: script.name, event_mask: script.event_mask, hook_mode: true,
      config_write_allowed: script.config_write_allowed, idle_velocity: script.idle_velocity,
      pressed_velocity: script.pressed_velocity, script_hex: hex(script.script), runtime_error_hex: '', ui: script.ui || [],
    };
  }

  function streamsJson() {
    const stream = (id, capture, rate, period, mode = 'MMAP', xr = 0) =>
      ({ id, capture, active: true, rate, period_frames: period, buffer_frames: period * 3, mode, xruns: xr });
    const { SCARLETT } = data.ids;
    return [
      stream(`alsa_${SCARLETT}_dev0_in`, true, 48000, 128), stream(`alsa_${SCARLETT}_dev0_out`, false, 48000, 128),
      stream('alsa_vc4hdmi0_dev0_out', false, 48000, 256),
      stream('usb_gadget_in', true, 48000, 128, 'RW'), stream('usb_gadget_out', false, 48000, 128, 'RW', 1),
    ];
  }

  // ---------------------------------------------------------------- device lifecycle
  let offlineUntil = 0;
  function reboot(seconds = 4) {
    offlineUntil = Date.now() + seconds * 1000;
    S.bootAt = offlineUntil;
    voices.length = 0;
    log('MAIN', 'info', `AudioX ${S.version} starting (board pi5, Raspberry Pi 5 Model B Rev 1.0)`);
  }

  function setRoutingText(text) {
    S.routingText = text.endsWith('\n') ? text : `${text}\n`;
  }
  function dropEdgesFor(id) {
    setRoutingText(S.routingText.split('\n').filter((line) => {
      if (!line.startsWith('edge=')) return true;
      const [src, dst] = line.slice(5).split(',');
      return src !== id && dst !== id;
    }).join('\n'));
  }

  // ---------------------------------------------------------------- router
  const routes = [];
  const on = (method, pattern, handler) => routes.push({ method, pattern, handler });

  on('GET', '/api/version', () => reply(`${S.version}\n`));
  on('GET', '/api/telemetry', () => reply({ consent: 'disabled', pending: 0, capacity: 8640, interval_ms: 10000, error: '' }));
  on('GET', '/api/telemetry/batch', () => reply({}));
  on('POST', '/api/telemetry', () => reply({ error: 'Telemetry is disabled in the demo.' }, 403));
  on('POST', '/api/telemetry/ack', () => reply({ error: 'Telemetry is disabled in the demo.' }, 403));
  on('GET', '/api/system/info', () => reply(systemInfo()));
  on('GET', '/api/system/history', ({ query }) => {
    const limit = clamp(Number(query.get('limit')) || 600, 1, 1000);
    return reply({ runtime_id: runtimeId, sample_interval_ms: 1000, capacity: 600, retention_ms: 600000,
      now_ms: monotonic(), samples: history.slice(-limit) });
  });
  on('GET', '/api/system/audio-devices', () => reply({ detected: { devices: S.alsaDetected },
    sampled_at_ms: monotonic() - 300, stale: false, streams: streamsJson() }));
  on('GET', '/api/system/diagnostics', () => reply({ format: 'audiox-diagnostics-v1', system: systemInfo(),
    privacy: 'Raw log messages, device identifiers, configuration, pairing records, scripts and audio are omitted.',
    logs: ['stdout', 'stderr'].map((source) => ({ source, available: true, line_limit: 128, byte_limit: 65536,
      lines: logRows.filter((r) => r.source === source).slice(-128).length,
      errors: logRows.filter((r) => r.source === source && r.level === 'error').length,
      warnings: logRows.filter((r) => r.source === source && r.level === 'warn').length, xrun_messages: 1 })) }));
  on('GET', '/api/system/logs', ({ query }) => {
    const order = ['debug', 'info', 'warn', 'error'];
    const source = query.get('source') || 'both';
    const level = query.get('level') || 'all';
    const text = (query.get('text') || '').toLowerCase();
    const limit = clamp(Number(query.get('limit')) || 600, 1, 5000);
    const rows = logRows.filter((r) => (source === 'both' || r.source === source)
      && (level === 'all' || order.indexOf(r.level) >= order.indexOf(level))
      && (!text || r.line.toLowerCase().includes(text)));
    return reply({ rows: rows.slice(-limit), shown: Math.min(limit, rows.length), has_more: rows.length > limit,
      scan_limited: false, missing_sources: [] });
  });
  on('GET', '/api/system/boot-failure', () => reply('no boot failure\n', 404));
  on('GET', '/api/system/fan', () => reply(S.fan));
  on('PUT', '/api/system/fan', ({ body }) => {
    const f = form(body);
    const nums = (key) => [0, 1, 2, 3].map((i) => Number(f[`${key}${i}`] ?? f[key]?.split(',')[i]));
    const temperatures = nums('temperature'), speeds = nums('speed');
    if (temperatures.every(Number.isInteger)) S.fan.temperatures = temperatures;
    if (speeds.every(Number.isInteger)) S.fan.speeds = speeds;
    if (Number.isInteger(Number(f.hysteresis))) S.fan.hysteresis = Number(f.hysteresis);
    if (f.enabled !== undefined) S.fan.enabled = f.enabled === '1' || f.enabled === 'true';
    return reply(S.fan);
  });
  on('POST', '/api/system/audio-stress', ({ body }) => {
    const f = form(body);
    if (f.mode === 'stop') {
      stress = { mode: 'idle', until: 0, workers: 0 };
      return ok({ mode: 'idle' });
    }
    stress = { mode: f.mode === 'cpu' ? 'cpu' : 'audio', until: Date.now() + clamp(Number(f.duration_secs) || 10, 1, 60) * 1000,
      workers: f.mode === 'cpu' ? 4 : 0 };
    log('SYSTEM', 'warn', `stress test (${stress.mode}) started`);
    return ok();
  });
  on('POST', '/api/system/sync', () => reply({ status: 'synced' }));
  on('POST', '/api/system/restart', () => { setTimeout(() => reboot(4), 300); return reply({ status: 'restarting' }); });
  on('POST', '/api/system/shutdown', () => {
    setTimeout(() => reboot(6), 300);
    notify('Demo device "powered off". It will boot again in a few seconds.');
    return reply({ status: 'shutting down' });
  });

  on('GET', '/api/rootfs/config.staging.txt', () => reply(S.configText));
  on('GET', '/api/rootfs/config.txt', () => reply(S.configText));
  on('PUT', '/api/rootfs/config.staging.txt', ({ body }) => { S.configText = body; return reply('ok\n'); });
  on('POST', '/api/config/reload', () => reply({ ok: true, sampleRate: 48000, playbackChannels: 2, captureChannels: 2,
    sampleSize: 2, soundboardMode: 'play', source: 'real' }));
  on('GET', '/api/config/restore/check', () => reply('ok\n'));
  on('GET', '/api/framebuffer/view', () => reply({ view: S.framebufferView }));
  on('POST', '/api/framebuffer/view', ({ body }) => {
    S.framebufferView = form(body).view === 'soundboard' ? 'soundboard' : 'meters';
    return reply({ view: S.framebufferView });
  });
  on('GET', '/api/updates/channel', () => reply({ channel: S.updateChannel }));
  on('POST', '/api/updates/channel', ({ body }) => {
    S.updateChannel = form(body).channel === 'nightly' ? 'nightly' : 'stable';
    return reply({ channel: S.updateChannel });
  });
  on('GET', '/api/updates/notifications', () => reply({ enabled: S.updateNotifications }));
  on('POST', '/api/updates/notifications', ({ body }) => {
    S.updateNotifications = form(body).enabled === '1';
    return reply({ enabled: S.updateNotifications });
  });

  // Routing
  on('GET', '/api/routing/things', () => reply({ ok: true, things: things() }));
  on('GET', '/api/rootfs/routing.txt', () => reply(S.routingText));
  on('PUT', '/api/rootfs/routing.txt', ({ body }) => { setRoutingText(body); return reply('ok\n'); });
  on('POST', '/api/routing/reload', () => {
    const { edges, skipped } = parsedEdges();
    const count = things().length;
    log('AUDIO', skipped ? 'warn' : 'info', `routing graph ready: ${count} things, ${edges.length} edges, ${skipped} skipped`);
    return reply({ ok: true, generation: 7, topologyGeneration: 5, things: count, edges: edges.length,
      skippedEdges: skipped, status: skipped ? `${skipped} edge(s) reference missing things or channels` : 'ok' });
  });
  on('POST', '/api/routing/helper/create', () => {
    if (!routingHasHelper()) setRoutingText(S.routingText.replace(/^(# audiox routing v1\n)?/, '# audiox routing v1\nnode=routing_helper\n'));
    return ok();
  });
  on('POST', '/api/routing/helper/delete', () => {
    dropEdgesFor('routing_helper');
    setRoutingText(S.routingText.replace(/^node=routing_helper\s*\n/m, ''));
    return ok();
  });

  // Effects
  const findFx = (id) => S.effects.find((fx) => fx.id === id);
  on('GET', '/api/effects', () => reply({ effects: S.effects.map(effectJson) }));
  on('POST', '/api/effect/create', ({ body }) => {
    const type = form(body).type;
    if (!typeLabels[type] && !plugin(type)) return fail('unknown effect type');
    if (S.effects.length >= 32) return fail('effect limit reached', 409);
    const fx = { id: `fx_${S.nextEffect++}`, type, name: '', enabled: true, params: {} };
    S.effects.push(fx);
    // Ids are reused after a scene recall; don't let a new effect inherit old MIDI bindings.
    delete S.effectCc[fx.id]; delete S.effectToggle[fx.id]; delete S.effectLights[fx.id];
    tails.delete(fx.id);
    return ok({ id: fx.id, type });
  });
  on('POST', '/api/effect/delete', ({ body }) => {
    const id = form(body).id;
    if (!findFx(id)) return fail('unknown effect id', 404);
    S.effects = S.effects.filter((fx) => fx.id !== id);
    delete S.effectCc[id]; delete S.effectToggle[id]; delete S.effectLights[id]; delete S.gains[id];
    dropEdgesFor(id);
    return ok({ id });
  });
  on('POST', '/api/effect/set', ({ body }) => {
    const f = form(body);
    const fx = findFx(f.id);
    if (!fx) return fail('unknown effect id', 404);
    if (f.enabled !== undefined) fx.enabled = f.enabled === '1' || f.enabled === 'true';
    if (f.type !== undefined) {
      if (!typeLabels[f.type] && !plugin(f.type)) return fail('unknown effect type');
      fx.type = f.type;
      fx.params = {};
      delete S.effectCc[fx.id];
    }
    if (f.param !== undefined) {
      const spec = specFor(fx.type).find(([name]) => name === f.param);
      const value = Number(f.value);
      if (!spec || !Number.isFinite(value)) return fail('invalid param/value');
      fx.params[f.param] = clamp(value, spec[2], spec[3]);
    }
    const json = effectJson(fx);
    return ok({ id: fx.id, type: fx.type, enabled: fx.enabled, params: json.params });
  });
  on('POST', '/api/effect/name', ({ body }) => {
    const f = form(body);
    const fx = findFx(f.id);
    if (!fx) return fail('unknown effect id', 404);
    fx.name = unhex(f.name_hex);
    return ok({ id: fx.id });
  });
  on('POST', '/api/effect/midi_cc/set', ({ body }) => {
    const f = form(body);
    if (!findFx(f.id)) return fail('unknown effect id', 404);
    for (const map of Object.values(S.effectCc)) for (const key of Object.keys(map)) if (map[key] === Number(f.cc)) delete map[key];
    (S.effectCc[f.id] ||= {})[f.param] = Number(f.cc);
    return ok({ cc: Number(f.cc), id: f.id, param: f.param });
  });
  on('POST', '/api/effect/midi_cc/delete', ({ body }) => {
    const cc = Number(form(body).cc);
    for (const map of Object.values(S.effectCc)) for (const key of Object.keys(map)) if (map[key] === cc) delete map[key];
    return ok({ cc });
  });
  on('POST', '/api/effect/midi_toggle/set', ({ body }) => {
    const f = form(body);
    S.effectToggle[f.id] = Number(f.note);
    return ok({ note: Number(f.note), id: f.id });
  });
  on('POST', '/api/effect/midi_toggle/delete', ({ body }) => {
    const note = Number(form(body).note);
    for (const [id, n] of Object.entries(S.effectToggle)) if (n === note) delete S.effectToggle[id];
    return ok({ note });
  });
  on('POST', '/api/effect/midi_light/set', ({ body }) => {
    const f = form(body);
    S.effectLights[f.id] = [Number(f.enabled_vel), Number(f.bypassed_vel)];
    return ok({ id: f.id, enabled_vel: Number(f.enabled_vel), bypassed_vel: Number(f.bypassed_vel) });
  });
  on('POST', '/api/effect/midi_light/delete', ({ body }) => { delete S.effectLights[form(body).id]; return ok({ id: form(body).id }); });

  // Plugins
  on('GET', '/api/plugins', () => reply({ plugins: S.plugins }));
  on('PUT', '/api/plugins/install', () => fail('Installing native plugins is disabled in the live demo.', 403));
  on('POST', '/api/plugins/reload', () => reply('plugin reloaded\n'));
  on('POST', '/api/plugins/remove', () => fail('Removing plugins is disabled in the live demo.', 403));

  // Volume & meters
  on('GET', '/api/audio/meters', () => reply({ meters: meters() }));
  on('GET', '/api/audio/volumes', () => reply({ volumes: things().map((t) => ({ id: t.id, name: t.name, gain: Number((S.gains[t.id] ?? 1).toFixed(4)) })) }));
  on('POST', '/api/audio/volume/set', ({ body }) => {
    const f = form(body);
    const gain = clamp(Number(f.gain), 0, 2);
    if (!things().some((t) => t.id === f.id) || !Number.isFinite(gain)) return fail('invalid id/gain');
    S.gains[f.id] = gain;
    return ok({ id: f.id, gain });
  });
  on('GET', '/api/midi/cc_volumes', () => reply({ mappings: S.ccVolumes }));
  on('POST', '/api/midi/cc_volume/set', ({ body }) => {
    const f = form(body);
    S.ccVolumes = S.ccVolumes.filter((m) => m.cc !== Number(f.cc) && m.id !== f.id).concat({ cc: Number(f.cc), id: f.id });
    return ok({ cc: Number(f.cc), id: f.id });
  });
  on('POST', '/api/midi/cc_volume/delete', ({ body }) => {
    const cc = Number(form(body).cc);
    S.ccVolumes = S.ccVolumes.filter((m) => m.cc !== cc);
    return ok({ cc });
  });

  // MIDI
  on('GET', '/api/midi/last_note', () => reply({ connected: true, device: 'Akai MPD218', last_note: midi.note,
    last_velocity: midi.velocity, last_seq: midi.noteSeq, last_channel: midi.channel }));
  on('GET', '/api/midi/last_cc', () => reply({ connected: true, last_cc: midi.cc, last_cc_value: midi.ccValue, last_seq: midi.ccSeq }));
  on('GET', '/api/midi/mappings', () => reply({ mapping_count: S.mappings.length, mappings: S.mappings }));
  on('POST', '/api/midi/mapping/set', ({ body }) => {
    const f = form(body);
    const note = Number(f.note);
    S.mappings = S.mappings.filter((m) => m.note !== note && m.sfx !== f.sfx).concat({ note, sfx: f.sfx });
    return ok({ note, sfx: f.sfx });
  });
  on('POST', '/api/midi/mapping/delete', ({ body }) => {
    const note = Number(form(body).note);
    S.mappings = S.mappings.filter((m) => m.note !== note);
    return ok({ note });
  });
  on('GET', '/api/midi/actions', () => reply({ count: S.actions.length, actions: S.actions }));
  on('POST', '/api/midi/action/set', ({ body }) => {
    const f = form(body);
    const note = Number(f.note);
    S.actions = S.actions.filter((a) => a.note !== note && a.action !== f.action).concat({ note, action: f.action });
    return ok({ note, action: f.action });
  });
  on('POST', '/api/midi/action/delete', ({ body }) => {
    const note = Number(form(body).note);
    S.actions = S.actions.filter((a) => a.note !== note);
    return ok({ note });
  });
  on('GET', '/api/midi/sampler_config', () => reply(S.sampler));
  on('POST', '/api/midi/sampler_config', ({ body }) => {
    const f = form(body);
    if (f.keyboard_enabled !== undefined) S.sampler.keyboard_enabled = f.keyboard_enabled === '1' || f.keyboard_enabled === 'true';
    if (f.keyboard_channel !== undefined) S.sampler.keyboard_channel = clamp(Number(f.keyboard_channel) || 0, 0, 15);
    if (f.root_note !== undefined) S.sampler.root_note = clamp(Number(f.root_note) || 60, 0, 127);
    return ok(S.sampler);
  });
  on('GET', '/api/midi/sampler_mode', () => reply({ active: S.samplerActive }));
  on('POST', '/api/midi/sampler_mode', () => { S.samplerActive = !S.samplerActive; return reply({ active: S.samplerActive }); });
  on('GET', '/api/midi/light_config', () => reply(S.light));
  on('POST', '/api/midi/light_config', ({ body }) => {
    const f = form(body);
    for (const key of Object.keys(S.light)) if (f[key] !== undefined) S.light[key] = clamp(Number(f[key]) || 0, 0, 127);
    return reply(S.light);
  });
  on('GET', '/api/midi/light_dim', () => ok({ dimmed: S.lightsDimmed }));
  on('POST', '/api/midi/light_dim', ({ body }) => { S.lightsDimmed = form(body).dimmed === '1'; return ok({ dimmed: S.lightsDimmed }); });
  on('POST', '/api/midi/light_refresh', () => ok());

  // Soundboard
  const soundNames = () => S.sounds.map((s) => s.name);
  on('GET', '/api/soundboard/order', () => reply(soundNames().map((n) => `${n}\n`).join('')));
  on('POST', '/api/soundboard/order', ({ body }) => {
    const names = body.split('\n').map((s) => s.trim()).filter(Boolean);
    const byName = new Map(S.sounds.map((s) => [s.name, s]));
    if (names.length !== S.sounds.length || names.some((n) => !byName.has(n))) return fail('order must list every sound once', 409);
    S.sounds = names.map((n) => byName.get(n));
    return reply(soundNames().map((n) => `${n}\n`).join(''));
  });
  on('GET', '/api/rootfs/sfx/', () => reply(`rootfs listing: /sfx\n${soundNames().join('\n')}\n`));
  on('PUT', /^\/api\/rootfs\/sfx\/(.+)$/, ({ match, rawBody }) => {
    const name = decodeURIComponent(match[1]);
    const seconds = clamp(bodySize(rawBody) / (48000 * 4), 0.5, 20);
    const existing = S.sounds.find((s) => s.name === name);
    if (existing) existing.seconds = seconds;
    else S.sounds.push({ name, seconds });
    log('SOUNDBOARD', 'info', `stored ${name} (${bodySize(rawBody)} bytes)`);
    return reply('ok\n');
  });
  on('DELETE', /^\/api\/rootfs\/sfx\/(.+)$/, ({ match }) => {
    const name = decodeURIComponent(match[1]);
    S.sounds = S.sounds.filter((s) => s.name !== name);
    S.mappings = S.mappings.filter((m) => m.sfx !== name);
    S.layout.cells = S.layout.cells.filter((c) => c.sfx !== name);
    return reply('deleted\n');
  });
  on('POST', '/api/soundboard/reload', () => reply({ ok: true, loaded: S.sounds.length, max: 64 }));
  on('POST', /^\/api\/soundboard\/(trigger|press)\/(.+)$/, ({ match }) =>
    playSound(decodeURIComponent(match[2])) ? reply('ok\n') : fail('unknown sound', 404));
  on('POST', /^\/api\/soundboard\/release\/(.+)$/, ({ match }) => {
    const name = decodeURIComponent(match[1]);
    for (let i = voices.length - 1; i >= 0; i -= 1) if (voices[i].name === name) voices.splice(i, 1);
    return reply('ok\n');
  });
  on('POST', '/api/soundboard/stop_all', () => { voices.length = 0; return reply('ok\n'); });
  on('GET', '/api/soundboard/modes', () => reply({ count: Object.keys(S.modes).length,
    modes: Object.entries(S.modes).map(([sfx, mode]) => ({ sfx, mode })) }));
  on('POST', '/api/soundboard/mode/set', ({ body }) => {
    const f = form(body);
    if (f.mode === 'hold') S.modes[f.sfx] = 'hold'; else delete S.modes[f.sfx];
    return ok({ sfx: f.sfx, mode: f.mode === 'hold' ? 'hold' : 'play' });
  });
  on('GET', '/api/soundboard/layout', () => reply(S.layout));
  on('POST', '/api/soundboard/layout', ({ body }) => {
    const f = form(body);
    const L = S.layout;
    if (f.mode !== undefined) {
      L.mode = f.mode === 'grid' ? 'grid' : 'normal';
      L.rows = clamp(Math.round(Number(f.rows)) || L.rows, 1, 8);
      L.columns = clamp(Math.round(Number(f.columns)) || L.columns, 1, 8);
      L.cells = L.cells.filter((c) => c.row < L.rows && c.column < L.columns);
    } else if (f.corner !== undefined) {
      L.corners[clamp(Number(f.corner), 0, 3)] = clamp(Number(f.note), 0, 127);
    } else {
      const sfx = f.action ? `__${f.action}__` : f.sfx;
      const previous = L.cells.find((c) => c.sfx === sfx);
      L.cells = L.cells.filter((c) => c.sfx !== sfx);
      if (f.unassign !== '1') {
        const row = Number(f.row), column = Number(f.column);
        const occupant = L.cells.find((c) => c.row === row && c.column === column);
        if (occupant) {
          L.cells = L.cells.filter((c) => c !== occupant);
          if (previous) L.cells.push({ sfx: occupant.sfx, row: previous.row, column: previous.column });
        }
        L.cells.push({ sfx, row, column });
      }
    }
    return reply(L);
  });

  // Scripts
  const findScript = (id) => S.scripts.find((s) => s.id === Number(id));
  on('GET', '/api/scripts', () => reply({ scripts: S.scripts.map(scriptJson) }));
  on('POST', '/api/scripts', ({ body }) => {
    const f = form(body);
    const source = unhex(f.script_hex);
    if (!/hook\s*\(/.test(source)) return fail('script must register at least one hook() callback');
    const fields = {
      name: unhex(f.name_hex), midi_enabled: f.midi_enabled === '1', channel: Number(f.channel) || 0,
      note: Number(f.note) || 0, idle_velocity: Number(f.idle_velocity) || 0, pressed_velocity: Number(f.pressed_velocity) || 0,
      config_write_allowed: f.config_write_ack === '1', script: source,
      event_mask: (/hook\(\s*"run"/.test(source) ? 128 : 0) | (/hook\(\s*"note_on"/.test(source) ? 1 : 0)
        | (/hook\(\s*"sound_/.test(source) ? 48 : 0) | (/hook\(\s*"(boot|ui)"/.test(source) ? 2048 : 0),
    };
    const existing = f.id ? findScript(f.id) : null;
    if (existing) Object.assign(existing, fields);
    else S.scripts.push({ id: S.nextScript++, ui: [], ...fields });
    log('SCRIPT', 'info', `saved script "${fields.name}"`);
    return reply('ok\n');
  });
  on('DELETE', '/api/scripts', ({ body }) => {
    const id = Number(form(body).id);
    S.scripts = S.scripts.filter((s) => s.id !== id);
    return reply('deleted\n');
  });
  on('POST', '/api/scripts/run', ({ body }) => {
    const script = findScript(form(body).id);
    if (!script) return fail('unknown script', 404);
    log('SCRIPT', 'info', `Script ${script.id} (${script.name}): run hook queued`);
    if (/sound\.play\("([^"]+)"/.test(script.script)) playSound(RegExp.$1);
    return reply('queued\n');
  });
  on('POST', '/api/scripts/control', ({ body }) => {
    const f = form(body);
    const script = findScript(f.id);
    const key = unhex(f.key_hex);
    const control = script?.ui.find((c) => c.key === key);
    if (!control) return fail('unknown control', 404);
    if (control.type === 'button') return reply('ui queued\n');
    const raw = unhex(f.value_hex);
    control.value = control.type === 'number' ? Number(raw) : control.type === 'boolean' ? raw === 'true' : raw;
    return reply('saved; config_change queued\n');
  });

  // Scenes
  // Scenes hold a full snapshot like src/config/scenes.cpp; recall restores it in this tab only.
  const snapshot = () => JSON.parse(JSON.stringify({ effects: S.effects, gains: S.gains, routingText: S.routingText }));
  const sceneJson = (scene) => {
    const snap = scene.snapshot;
    const edgeCount = (snap.routingText.match(/^edge=/gm) || []).length;
    const volumes = things().length - S.effects.length + snap.effects.length;
    return { id: scene.id, name: scene.name, effects: snap.effects.length, routes: edgeCount, volumes,
      channel: scene.channel, note: scene.note };
  };
  const sceneView = () => ({ scenes: S.scenes.map(sceneJson), active: S.activeScene, sequence: S.sceneSequence });
  function recallScene(scene) {
    const snap = JSON.parse(JSON.stringify(scene.snapshot));
    S.effects = snap.effects;
    S.nextEffect = Math.max(0, ...S.effects.map((fx) => effectNumber(fx.id))) + 1;
    S.gains = snap.gains;
    S.routingText = snap.routingText;
    tails.clear();
    S.activeScene = scene.id;
    S.sceneSequence += 1;
    log('SCENE', 'info', `recalled "${scene.name}" (${snap.effects.length} effects, ${parsedEdges().edges.length} routes)`);
  }
  on('GET', '/api/scenes', () => reply(sceneView()));
  on('POST', '/api/scenes/save', ({ query }) => {
    const id = query.get('id');
    const name = unhex(query.get('name_hex')).trim();
    if (!id || !name) return reply({ error: 'scene id and name are required' }, 400);
    const fields = { name, channel: Number(query.get('channel') ?? -1), note: Number(query.get('note') ?? -1),
      snapshot: snapshot() };
    const scene = S.scenes.find((s) => s.id === id);
    if (scene) Object.assign(scene, fields); else S.scenes.push({ id, ...fields });
    S.activeScene = id;
    log('SCENE', 'info', `captured "${name}" (${S.effects.length} effects, ${parsedEdges().edges.length} routes)`);
    return reply({ ok: true });
  });
  on('POST', '/api/scenes/recall', ({ query }) => {
    const scene = S.scenes.find((s) => s.id === query.get('id'));
    if (!scene) return reply({ error: 'unknown scene' }, 404);
    recallScene(scene);
    return reply({ ok: true });
  });
  on('POST', '/api/scenes/bind', ({ query }) => {
    const scene = S.scenes.find((s) => s.id === query.get('id'));
    if (!scene) return reply({ error: 'unknown scene' }, 404);
    scene.channel = Number(query.get('channel'));
    scene.note = Number(query.get('note'));
    return reply({ ok: true });
  });
  on('DELETE', '/api/scenes/delete', ({ query }) => {
    S.scenes = S.scenes.filter((s) => s.id !== query.get('id'));
    if (S.activeScene === query.get('id')) S.activeScene = '';
    return reply({ ok: true });
  });

  // Bluetooth
  on('GET', '/api/bluetooth/devices', () => reply(bluetoothJson()));
  on('POST', '/api/bluetooth/scan', () => {
    if (S.btScanning) return reply({ ok: false, error: 'bluetooth is busy' }, 409);
    S.btScanning = true;
    S.nearbyPool.forEach((device, i) => setTimeout(() => {
      if (!S.bluetooth.some((d) => d.address === device.address)) S.bluetooth.push({ ...device, paired: false, connected: false });
    }, 1200 + i * 1600));
    setTimeout(() => { S.btScanning = false; }, 10000);
    return ok();
  });
  const btDevice = (body) => S.bluetooth.find((d) => d.address === form(body).address);
  on('POST', '/api/bluetooth/pair', ({ body }) => {
    const device = btDevice(body);
    if (!device) return reply({ ok: false, error: 'invalid bluetooth address' }, 400);
    S.btPairing = true;
    device.connecting = true;
    setTimeout(() => Object.assign(device, { paired: true, connected: true, connecting: false, auto_connect: true }, (S.btPairing = false, {})), 3000);
    return ok();
  });
  on('POST', '/api/bluetooth/connect', ({ body }) => {
    const device = btDevice(body);
    if (!device?.paired) return reply({ ok: false, error: 'device is not paired or is not a classic device' }, 400);
    device.connecting = true;
    setTimeout(() => Object.assign(device, { connected: true, connecting: false }), 2200);
    return ok();
  });
  on('POST', '/api/bluetooth/disconnect', ({ body }) => {
    const address = form(body).address;
    for (const d of S.bluetooth) if (!address || d.address === address) d.connected = false;
    return ok();
  });
  on('POST', '/api/bluetooth/unpair', ({ body }) => {
    const address = form(body).address;
    S.bluetooth = S.bluetooth.filter((d) => d.address !== address);
    return ok();
  });
  on('POST', '/api/bluetooth/auto-connect', ({ body }) => {
    const device = btDevice(body);
    if (!device?.paired) return reply({ ok: false, error: 'device is not a saved Classic Bluetooth device' }, 400);
    device.auto_connect = form(body).enabled === 'true' || form(body).enabled === '1';
    return ok();
  });

  // ---------------------------------------------------------------- gateway
  async function request(input, init = {}) {
    const url = new URL(typeof input === 'string' ? input : input.url, root.location.href);
    if (!url.pathname.startsWith('/api/')) return root.fetch(input, init);
    const method = String(init.method || 'GET').toUpperCase();
    await sleep(rand(15, 70));
    if (init.signal?.aborted) throw init.signal.reason ?? new DOMException('Aborted', 'AbortError');
    if (Date.now() < offlineUntil) {
      await sleep(Math.min(800, offlineUntil - Date.now()));
      throw new TypeError('Failed to fetch');
    }
    const path = url.pathname;
    for (const route of routes) {
      if (route.method !== method && !(route.method === 'GET' && method === 'HEAD')) continue;
      const match = typeof route.pattern === 'string' ? (route.pattern === path ? [path] : null) : route.pattern.exec(path);
      if (!match) continue;
      const rawBody = init.body;
      const body = await bodyText(rawBody instanceof Blob || rawBody instanceof ArrayBuffer ? '' : rawBody);
      return route.handler({ method, path, query: url.searchParams, body, rawBody, match });
    }
    const known = routes.some((route) => typeof route.pattern === 'string' ? route.pattern === path : route.pattern.test(path));
    return known ? fail('method not allowed', 405) : fail(`not found: ${path}`, 404);
  }

  function upload(url, body, { onProgress, onSent } = {}) {
    return new Promise((resolve) => {
      const total = Math.max(1, bodySize(body));
      const steps = 24;
      let step = 0;
      const timer = setInterval(() => {
        step += 1;
        onProgress?.(Math.round(total * step / steps), total);
        if (step < steps) return;
        clearInterval(timer);
        onSent?.();
        setTimeout(() => {
          if (url === '/api/update') {
            log('UPDATE', 'info', `validated update archive (${total} bytes); rebooting into the new image`);
            setTimeout(() => reboot(5), 300);
            resolve({ status: 200, text: 'update installed; rebooting\n' });
          } else if (url === '/api/config/restore') {
            setTimeout(() => reboot(5), 300);
            resolve({ status: 202, text: 'restore staged; rebooting\n' });
          } else {
            resolve({ status: 404, text: 'not found\n' });
          }
        }, 900);
      }, 60);
    });
  }

  function notify(message) {
    if (root.AudioxUI?.notify) root.AudioxUI.notify(message, 'warn', 'demo');
    else root.alert?.(message);
  }

  function download(url) {
    notify(url === '/api/config/backup'
      ? 'Backups are disabled in the live demo; on a real device this downloads a .tar.gz of every setting and sound.'
      : 'Downloads are disabled in the live demo.');
  }

  // The update flow downloads a real release archive from the public mirror.
  // Serve a synthetic AXU of the advertised size instead so the demo never pulls firmware.
  const realFetch = root.fetch.bind(root);
  const archiveSizes = new Map();
  root.fetch = async (input, init) => {
    const url = String(typeof input === 'string' ? input : input?.url || '');
    if (/\/updates\.json(\?|$)/.test(url)) {
      const response = await realFetch(input, init);
      const clone = response.clone();
      clone.json().then((manifest) => {
        for (const entry of Object.values(manifest || {})) {
          for (const asset of Object.values(entry?.assets || {})) archiveSizes.set(new URL(asset.url, url).href, asset.size);
        }
      }).catch(() => {});
      return response;
    }
    if (/\.axu(\?|$)/.test(url) && archiveSizes.has(url)) {
      const size = archiveSizes.get(url);
      const chunk = Math.ceil(size / 40);
      let sent = 0;
      const stream = new ReadableStream({
        async pull(controller) {
          await sleep(45);
          if (sent >= size) { controller.close(); return; }
          const n = Math.min(chunk, size - sent);
          const bytes = new Uint8Array(n);
          if (sent === 0) bytes.set(new TextEncoder().encode('AXUPDATE'));
          sent += n;
          controller.enqueue(bytes);
        },
      });
      return new Response(stream, { status: 200, headers: { 'Content-Length': String(size) } });
    }
    return realFetch(input, init);
  };

  function banner() {
    if (!root.document?.body || root.document.getElementById('demo-banner')) return;
    const bar = root.document.createElement('div');
    const style = root.document.createElement('style');
    style.textContent = '#demo-banner{position:sticky;top:0;z-index:10000;display:flex;gap:12px;align-items:center;' +
      'justify-content:center;flex-wrap:wrap;padding:7px 14px;font:13px/1.4 system-ui,sans-serif;color:#1b1300;' +
      'background:linear-gradient(90deg,#f7b733,#fc4a1a);box-shadow:0 2px 8px rgba(0,0,0,.35)}' +
      '#demo-banner strong{text-transform:uppercase;letter-spacing:.08em}' +
      '#demo-banner a{color:inherit;font-weight:600;text-decoration:underline}';
    root.document.head.append(style);
    bar.id = 'demo-banner';
    bar.setAttribute('role', 'note');
    bar.innerHTML = '<strong>Live demo</strong><span>Everything here is simulated in your browser &mdash; ' +
      'no AudioX device is connected and nothing you change is saved.</span>' +
      '<a href="../">Back to AudioX</a>';
    root.document.body.prepend(bar);
  }
  if (root.document) {
    if (root.document.readyState === 'loading') root.document.addEventListener('DOMContentLoaded', banner);
    else banner();
  }

  const api = { demo: true, request, upload, download, state: S, startedAt };
  root.AudioxApi = api;
  root.apiFetch = (input, init) => root.AudioxApi.request(input, init);
})(globalThis);
