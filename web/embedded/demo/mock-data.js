// Initial state for the public Live Demo. Shapes mirror the device API
// (src/http/api.cpp); demo/mock-api.js mutates a fresh copy per page load.
(function (root) {
  'use strict';

  const SCARLETT = 'usb-1235-8210-s3f9a1c2d7e4b6a80';
  const JBL = { address: 'F8:5C:7D:12:A4:3B', name: 'JBL Flip 6' };

  const btThing = (address, dir) => `bt_${address.split(':').reverse().join('')}_${dir}`;

  // Built-in effect parameter specs copied from src/audio/effects/slot.cpp.
  const effectSpecs = {
    gain: [['level', 'Level', 0, 4, 1]],
    distortion: [['input_gain', 'Input Gain', 0, 4, 1], ['drive', 'Drive', 0, 8, 4],
      ['clip', 'Clip', 0.05, 1, 0.35], ['output_gain', 'Output Gain', 0, 4, 1]],
    pitch: [['input_gain', 'Input Gain', 0, 4, 1], ['semitones', 'Semitones', -12, 12, 0],
      ['mix', 'Mix', 0, 1, 1], ['output_gain', 'Output Gain', 0, 4, 1],
      ['noise_floor', 'Noise Floor', 0.0001, 0.02, 0.001], ['wet_lpf_hz', 'Wet LPF Hz', 1000, 18000, 12000],
      ['quality', 'Quality', 0, 2, 0]],
    reverb: [['input_gain', 'Input Gain', 0, 4, 1], ['feedback', 'Feedback', 0.05, 0.95, 0.64],
      ['damping', 'Damping', 0, 1, 0.48], ['wet_mix', 'Wet Mix', 0, 1, 0.26]],
    gate: [['threshold_db', 'Threshold dB', -80, 0, -62], ['attack_ms', 'Attack ms', 0.2, 100, 2],
      ['release_ms', 'Release ms', 5, 600, 160], ['range_db', 'Range dB', 0, 80, 30]],
    cut: [['channels', 'Channels', 1, 16, 2]],
    compressor: [['threshold_db', 'Threshold dB', -60, 0, -18], ['ratio', 'Ratio', 1, 20, 4],
      ['attack_ms', 'Attack ms', 0.1, 100, 10], ['release_ms', 'Release ms', 5, 1000, 120],
      ['knee_db', 'Knee dB', 0, 24, 6], ['makeup_db', 'Makeup dB', -12, 24, 0], ['mix', 'Mix', 0, 1, 1]],
    limiter: [['ceiling_db', 'Ceiling dB', -24, 0, -1], ['release_ms', 'Release ms', 5, 1000, 80],
      ['lookahead_ms', 'Lookahead ms', 0, 10, 3], ['input_gain', 'Input Gain', 0, 4, 1]],
    eq: [['low_hz', 'Low Frequency Hz', 40, 1000, 150], ['low_gain_db', 'Low Gain dB', -18, 18, 0],
      ['mid_hz', 'Mid Frequency Hz', 200, 8000, 1000], ['mid_gain_db', 'Mid Gain dB', -18, 18, 0],
      ['high_hz', 'High Frequency Hz', 1000, 18000, 6000], ['high_gain_db', 'High Gain dB', -18, 18, 0],
      ['q', 'Q', 0.2, 10, 1], ['output_gain', 'Output Gain', 0, 4, 1]],
    multiband: [['low_hz', 'Low Crossover Hz', 80, 1000, 200], ['high_hz', 'High Crossover Hz', 1000, 10000, 2500],
      ['low_threshold_db', 'Low Threshold dB', -60, 0, -18], ['mid_threshold_db', 'Mid Threshold dB', -60, 0, -18],
      ['high_threshold_db', 'High Threshold dB', -60, 0, -18], ['ratio', 'Ratio', 1, 20, 4],
      ['release_ms', 'Release ms', 5, 1000, 120], ['makeup_db', 'Makeup dB', -12, 24, 0]],
    noise_reduction: [['reduction_db', 'Reduction dB', 0, 30, 12], ['noise_floor_db', 'Noise Floor dB', -90, -20, -55],
      ['adaptation', 'Adaptation', 0, 1, 0.1], ['smoothing', 'Smoothing', 0, 1, 0.7], ['mix', 'Mix', 0, 1, 1],
      ['output_gain', 'Output Gain', 0, 4, 1]],
  };

  const plugins = [{
    id: 'tape_echo',
    name: 'Tape Echo',
    version: '1.3.0',
    ui: false,
    params: [
      { name: 'time_ms', label: 'Delay Time', min: 20, max: 1200, default: 340, type: 'float', unit: 'ms',
        description: 'Delay between repeats.', step: 1, options: [] },
      { name: 'feedback', label: 'Feedback', min: 0, max: 0.95, default: 0.42, type: 'float', unit: '',
        description: 'Amount of each repeat fed back into the tape loop.', step: 0.01, options: [] },
      { name: 'wow', label: 'Wow & Flutter', min: 0, max: 1, default: 0.25, type: 'float', unit: '',
        description: 'Simulated tape speed instability.', step: 0.01, options: [] },
      { name: 'mix', label: 'Mix', min: 0, max: 1, default: 0.3, type: 'float', unit: '',
        description: 'Dry/wet balance.', step: 0.01, options: [] },
      { name: 'sync', label: 'Tempo Sync', min: 0, max: 1, default: 0, type: 'boolean', unit: '',
        description: 'Lock repeats to a fixed musical division.', step: 1, options: [] },
      { name: 'head', label: 'Tape Head', min: 0, max: 2, default: 1, type: 'enum', unit: '',
        description: 'Playback head spacing.', step: 1, options: ['Short', 'Medium', 'Long'] },
    ],
    text_controls: [],
  }];

  // A small home streaming rig: Scarlett 2i2 (mic on ch1, guitar on ch2), the Pi's USB
  // link to the streaming PC, the soundboard and a Bluetooth speaker.
  const effectList = [
    ['fx_1', 'gate', 'Mic Gate', true, { threshold_db: -48, release_ms: 180 }],
    ['fx_2', 'compressor', 'Vocal Comp', true, { threshold_db: -22, ratio: 3.5, attack_ms: 6, makeup_db: 4 }],
    ['fx_3', 'eq', 'Vocal EQ', true, { low_hz: 110, low_gain_db: -4, mid_hz: 2800, mid_gain_db: 2.5,
      high_hz: 9000, high_gain_db: 3, q: 0.9 }],
    ['fx_4', 'pitch', 'Robot Voice', false, { semitones: -5, mix: 0.85, quality: 1 }],
    ['fx_5', 'distortion', 'Guitar Drive', true, { drive: 5.6, clip: 0.42, output_gain: 0.8 }],
    ['fx_6', 'tape_echo', 'Tape Echo', true, { time_ms: 375, feedback: 0.38, mix: 0.28, head: 1 }],
    ['fx_7', 'limiter', 'Stream Limiter', true, { ceiling_db: -1, lookahead_ms: 4 }],
    // Enabled cut = muted; bypass it to hear the processed mic in the headphones.
    ['fx_8', 'cut', 'Mic Monitor Mute', true, { channels: 2 }],
  ].map(([id, type, name, enabled, params]) => ({ id, type, name, enabled, params }));

  const S = `alsa_${SCARLETT}_dev0`;
  const JBL_OUT = btThing(JBL.address, 'out');
  const stereo = (src, dst) => [[src, dst, 0, 0], [src, dst, 1, 1]];
  const mono = (src, ch, dst) => [[src, dst, ch, 0], [src, dst, ch, 1]];
  const HEADPHONES = `${S}_out`;
  // Two independent lines. Stream send: mic, guitar and soundboard fan into the limiter, then the
  // computer. Monitor bus: the Audio Mixer combines computer playback, soundboard, guitar and a
  // mutable mic monitor, then feeds every listening device (Scarlett headphones and the JBL), so
  // another output only needs one more stereo wire. Computer playback never reaches the computer.
  const micChain = [...mono(`${S}_in`, 0, 'fx_1'), ...stereo('fx_1', 'fx_2'), ...stereo('fx_2', 'fx_3')];
  const guitarChain = [...mono(`${S}_in`, 1, 'fx_5'), ...stereo('fx_5', 'fx_6')];
  const streamSend = (...sources) => [...sources.flatMap((src) => stereo(src, 'fx_7')),
    ...stereo('fx_7', 'usb_gadget_out')];
  const micMonitor = (from) => [...stereo(from, 'fx_8'), ...stereo('fx_8', 'routing_helper')];
  const monitorBus = (...sources) => [...sources.flatMap((src) => stereo(src, 'routing_helper')),
    ...stereo('routing_helper', HEADPHONES), ...stereo('routing_helper', JBL_OUT)];
  const edges = [
    ...micChain, ...stereo('fx_3', 'fx_4'), ...guitarChain,
    ...streamSend('fx_4', 'fx_6', 'soundboard_out'),
    ...micMonitor('fx_4'), ...monitorBus('usb_gadget_in', 'soundboard_out', 'fx_6'),
  ];

  // Devices that exist independently of routing; effects and Bluetooth are added from state.
  const devices = [
    { id: 'soundboard_out', name: 'Soundboard', inputs: 0, outputs: 2 },
    { id: 'usb_gadget_out', name: 'To computer (USB)', inputs: 2, outputs: 0 },
    { id: 'usb_gadget_in', name: 'From computer (USB)', inputs: 0, outputs: 2 },
    { id: `${S}_in`, name: 'From Scarlett 2i2 USB', inputs: 0, outputs: 2 },
    { id: `${S}_out`, name: 'To Scarlett 2i2 USB', inputs: 2, outputs: 0 },
    { id: 'alsa_vc4hdmi0_dev0_out', name: 'To vc4-hdmi-0', inputs: 2, outputs: 0 },
  ];

  // Simulated signal character per source output channel (see mock-api.js).
  const signals = {
    [`${S}_in`]: ['voice', 'guitar'],
    usb_gadget_in: ['music', 'music'],
  };

  const alsaDetected = [
    { card: 0, device: 0, playback: 1, capture: 0, playbackChannels: 2, captureChannels: 0, usb: 0, gadget: 0,
      node: 'hw:0,0', name: 'vc4-hdmi-0', stableCardId: 'vc4hdmi0' },
    { card: 1, device: 0, playback: 1, capture: 1, playbackChannels: 2, captureChannels: 2, usb: 0, gadget: 1,
      node: 'hw:1,0', name: 'UAC2Gadget', stableCardId: 'UAC2Gadget' },
    { card: 2, device: 0, playback: 1, capture: 1, playbackChannels: 2, captureChannels: 2, usb: 1, gadget: 0,
      node: 'hw:2,0', name: 'Scarlett 2i2 USB', stableCardId: SCARLETT },
  ].map((device, i) => ({ handle: i + 1, generation: 1, path: `/dev/snd/pcmC${device.card}D0`, ...device }));

  const sounds = [
    ['intro_jingle.wav', 6.5], ['applause.wav', 5], ['airhorn.wav', 2.2], ['rimshot.wav', 1.4],
    ['drumroll.wav', 3.8], ['sad_trombone.wav', 3.2], ['crickets.wav', 8], ['outro_jingle.wav', 9],
  ];

  // Bottom two rows of an MPD218 (notes 36-43); the upper pads drive scripts and scenes.
  const corners = [40, 43, 36, 39];
  const noteFor = (row, column) => 40 - row * 4 + column;
  const gridOrder = ['intro_jingle.wav', 'applause.wav', 'airhorn.wav', 'rimshot.wav',
    'drumroll.wav', 'sad_trombone.wav', '__sampler_toggle__', '__stop_all__'];
  const cells = gridOrder.map((sfx, i) => ({ sfx, row: Math.floor(i / 4), column: i % 4 }));

  const scripts = [
    {
      id: 1, name: 'Go Live', midi_enabled: true, channel: 9, note: 44,
      event_mask: 129, config_write_allowed: false, idle_velocity: 1, pressed_velocity: 5,
      script: `-- Start-of-stream sequence: jingle, fade in the computer audio, hotkey OBS.
local function go_live()
  sound.play("intro_jingle.wav")
  effects.enable("fx_4", false)
  for level = 20, 70, 10 do
    volume.set("usb_gadget_in", level)
    wait(250)
  end
  tap("CTRL+SHIFT+F9")  -- OBS: start streaming
  light(127)
  state.lives = (state.lives or 0) + 1
  log("Live! (#" .. state.lives .. ")")
end

hook("run", go_live)
hook("note_on", go_live)
`,
    },
    {
      id: 2, name: 'Robot Toggle', midi_enabled: true, channel: 9, note: 45,
      event_mask: 129, config_write_allowed: false, idle_velocity: 3, pressed_velocity: 9,
      script: `-- Flip the robot voice and show its state on the pad LED.
local function toggle()
  state.robot = not state.robot
  effects.enable("fx_4", state.robot)
  light(state.robot and 9 or 3)
  log("Robot voice " .. (state.robot and "on" or "off"))
end

hook("run", toggle)
hook("note_on", toggle)
`,
    },
    {
      id: 3, name: 'Startup Volume', midi_enabled: false, channel: 0, note: 60,
      event_mask: 2176, config_write_allowed: false, idle_velocity: 0, pressed_velocity: 0,
      ui: [
        { key: 'enabled', label: 'Apply startup volume', type: 'boolean', value: true },
        { key: 'level', label: 'Startup level (%)', type: 'number', value: 60, min: 0, max: 100 },
        { key: 'apply', label: 'Apply volume now', type: 'button' },
      ],
      script: `ui.option("enabled", {
    label = "Apply startup volume", type = "boolean", default = true
})
ui.option("level", {
    label = "Startup level (%)", type = "number", default = 50,
    min = 0, max = 100
})
ui.button("apply", "Apply volume now")

local function apply()
    if script_config.get("enabled", true) then
        volume.set("usb_gadget_in", script_config.get("level", 50))
    end
end

hook("boot", apply)
hook("ui", function(event)
    if event.key == "apply" then apply() end
end)
`,
    },
  ];

  const routingText = (list) => ['# audiox routing v1', 'node=routing_helper',
    ...list.map((e) => `edge=${e.join(',')}`), ''].join('\n');
  const copyEffects = (list) => list.map((fx) => ({ ...fx, params: { ...fx.params } }));
  const pickEffects = (ids, tweaks = {}) => copyEffects(effectList.filter((fx) => ids.includes(fx.id)))
    .map((fx) => ({ ...fx, ...(tweaks[fx.id] || {}), params: { ...fx.params, ...(tweaks[fx.id]?.params || {}) } }));
  const baseGains = { [`${S}_in`]: 1, usb_gadget_in: 0.6, soundboard_out: 0.8, fx_6: 0.9 };

  // Scene snapshots use the same shape as the live state so recall is a plain restore.
  const scenes = [
    { id: 'scene-live', name: 'Live Stream', channel: 9, note: 48,
      snapshot: { effects: copyEffects(effectList), gains: { ...baseGains }, routingText: routingText(edges) } },
    { id: 'scene-podcast', name: 'Podcast', channel: 9, note: 49,
      snapshot: {
        effects: pickEffects(['fx_1', 'fx_2', 'fx_3', 'fx_7', 'fx_8'], {
          fx_2: { params: { threshold_db: -26, ratio: 4.5, makeup_db: 6 } },
          fx_8: { enabled: false },
        }),
        gains: { [`${S}_in`]: 1.15, usb_gadget_in: 0.8, soundboard_out: 0.7 },
        routingText: routingText([...micChain, ...streamSend('fx_3', 'soundboard_out'),
          ...micMonitor('fx_3'), ...monitorBus('usb_gadget_in', 'soundboard_out')]),
      } },
    // Recording guitar to the computer while playing along to a backing track.
    { id: 'scene-guitar', name: 'Guitar Practice', channel: 9, note: 50,
      snapshot: {
        effects: pickEffects(['fx_5', 'fx_6', 'fx_7'], { fx_6: { params: { mix: 0.42, feedback: 0.5 } } }),
        gains: { [`${S}_in`]: 1, usb_gadget_in: 0.75, fx_6: 1 },
        routingText: routingText([...guitarChain, ...streamSend('fx_6'),
          ...monitorBus('fx_6', 'usb_gadget_in')]),
      } },
    { id: 'scene-clean', name: 'Clean Bypass', channel: -1, note: -1,
      snapshot: {
        effects: [],
        gains: { usb_gadget_in: 0.6, soundboard_out: 0.8 },
        routingText: routingText([...mono(`${S}_in`, 0, 'usb_gadget_out'), ...stereo('soundboard_out', 'usb_gadget_out'),
          ...monitorBus('usb_gadget_in', 'soundboard_out')]),
      } },
  ];

  function create() {
    return {
      version: '1.14.1',
      bootAt: Date.now() - (3 * 86400 + 7 * 3600 + 42 * 60) * 1000,
      devices: devices.map((d) => ({ ...d })),
      effects: copyEffects(effectList),
      nextEffect: effectList.length + 1,
      routingText: routingText(edges),
      gains: { ...baseGains },
      ccVolumes: [{ cc: 3, id: 'usb_gadget_in' }, { cc: 4, id: 'soundboard_out' }, { cc: 5, id: `${S}_in` }],
      effectCc: { fx_5: { drive: 8 }, fx_6: { feedback: 6 } },
      effectToggle: { fx_4: 46, fx_8: 47 },
      effectLights: { fx_4: [9, 3], fx_8: [1, 9] },
      sounds: sounds.map(([name, seconds]) => ({ name, seconds })),
      layout: { mode: 'grid', rows: 2, columns: 4, corners: [...corners], cells: cells.map((c) => ({ ...c })) },
      mappings: cells.filter((c) => !c.sfx.startsWith('__')).map((c) => ({ note: noteFor(c.row, c.column), sfx: c.sfx })),
      actions: [{ note: noteFor(1, 3), action: 'stop_all' }, { note: noteFor(1, 2), action: 'sampler_toggle' }],
      modes: { 'drumroll.wav': 'hold' },
      configText: [
        'usb_playback_channels=2', 'usb_capture_channels=2', 'usb_sample_rate=48000', 'usb_sample_size=2',
        'soundboard_mode=play', 'framebuffer_view=meters', 'midi_passthrough=unmapped', 'keyboard_macros=1', '',
      ].join('\n'),
      framebufferView: 'meters',
      samplerActive: false,
      sampler: { keyboard_enabled: true, keyboard_channel: 0, root_note: 60 },
      light: { channel: 9, mapped_vel: 3, playing_vel: 5, stop_all_vel: 1 },
      lightsDimmed: false,
      scripts: scripts.map((s) => ({ ...s, ui: (s.ui || []).map((c) => ({ ...c })) })),
      nextScript: scripts.length + 1,
      scenes: JSON.parse(JSON.stringify(scenes)),
      activeScene: 'scene-live',
      sceneSequence: 1,
      plugins: JSON.parse(JSON.stringify(plugins)),
      bluetooth: [
        { ...JBL, paired: true, connected: true, auto_connect: true, audio_sink: true, audio_source: false },
        { address: '38:18:4C:B2:90:5E', name: 'WH-1000XM4', paired: true, connected: false, auto_connect: false,
          audio_sink: true, audio_source: false },
      ],
      nearbyPool: [
        { address: '00:1A:7D:DA:71:13', name: 'Bose SoundLink Mini', audio_sink: true, audio_source: false },
        { address: '4C:87:5D:0B:3F:A2', name: 'Living Room TV', audio_sink: true, audio_source: false },
        { address: 'E4:58:B8:6A:0C:91', name: 'Galaxy Buds2 Pro', audio_sink: true, audio_source: false },
      ],
      fan: { enabled: true, temperatures: [50, 60, 67, 75], speeds: [75, 125, 175, 250], hysteresis: 5 },
      updateChannel: 'stable',
      updateNotifications: true,
      alsaDetected: alsaDetected.map((d) => ({ ...d })),
      signals,
    };
  }

  const api = { create, effectSpecs, btThing, noteFor, ids: { SCARLETT, JBL } };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.AudioxDemoData = api;
})(globalThis);
