/* global document, fetch, setInterval */
'use strict';

const effects = ['Gain', 'Distortion', 'Pitch', 'Reverb', 'Gate', 'Cut', 'Compressor',
  'Limiter', 'EQ', 'Multiband', 'Noise reduction', 'Third-party plugins'];
const number = (value) => value === null ? 'unavailable' : new Intl.NumberFormat(undefined,
  { maximumFractionDigits: 2 }).format(value);
const element = (name, text) => {
  const result = document.createElement(name);
  if (text !== undefined) result.textContent = text;
  return result;
};
const sum = (days, key) => days.reduce((total, day) => total + day[key], 0);
const mean = (days, key) => {
  const available = days.filter((day) => day[key] !== null && day.points);
  const count = sum(available, 'points');
  return count ? available.reduce((total, day) => total + day[key] * day.points, 0) / count : 0;
};

function bars(id, values) {
  const target = document.getElementById(id);
  target.replaceChildren();
  target.className = 'bars';
  const max = Math.max(1, ...values.map((entry) => entry[1]));
  for (const [label, value, suffix = ''] of values) {
    const row = element('div');
    row.className = 'bar-row';
    const bar = element('progress');
    bar.max = max; bar.value = value;
    bar.setAttribute('aria-label', label);
    const text = element('span', `${number(value)}${suffix}`);
    text.className = 'bar-value';
    row.append(element('span', label), bar, text);
    target.append(row);
  }
}

function chart(id, days, series) {
  const target = document.getElementById(id);
  target.replaceChildren();
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 500 210');
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', series.map((line) => line.label).join(', ') + ' by receipt date; daily values in the table below');
  const max = Math.max(1, ...series.flatMap((line) => days.map(line.value).filter((value) => value !== null)));
  const add = (name, attributes, text) => {
    const node = document.createElementNS('http://www.w3.org/2000/svg', name);
    for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
    if (text !== undefined) node.textContent = text;
    svg.append(node);
  };
  for (let i = 0; i < 4; i++) {
    const y = 20 + i * 50;
    add('line', { x1: 45, y1: y, x2: 485, y2: y, stroke: '#28536b' });
    add('text', { x: 0, y: y + 4, class: 'chart-label' }, number(max * (3 - i) / 3));
  }
  for (const line of series) {
    let path = '', connected = false;
    days.forEach((day, i) => {
      const value = line.value(day);
      if (value === null) { connected = false; return; }
      const x = 45 + i * 440 / Math.max(1, days.length - 1), y = 170 - value / max * 150;
      path += `${connected ? 'L' : 'M'}${x},${y} `;
      connected = true;
      add('circle', { cx: x, cy: y, r: 2, fill: line.color });
    });
    add('path', { d: path, fill: 'none', stroke: line.color, 'stroke-width': 2 });
  }
  add('text', { x: 45, y: 200, class: 'chart-label' }, days[0].day);
  add('text', { x: 415, y: 200, class: 'chart-label' }, days.at(-1).day);
  target.append(svg);
  if (series.length > 1) target.append(element('p', series.map((line) => line.label).join(' / ')));
}

function render(snapshot) {
  if (snapshot.format !== 'audiox-public-telemetry-v1' || !Array.isArray(snapshot.days) || snapshot.days.length !== 30)
    throw new Error('Unexpected dashboard response.');
  const days = snapshot.days;
  const count = sum(days, 'points');
  document.getElementById('empty').hidden = count !== 0;
  const totals = document.getElementById('totals');
  totals.replaceChildren();
  for (const [value, label] of [[count, 'Observations'], [count / 6, 'Observed minutes'],
    [sum(days, 'xruns'), 'X-run events'], [sum(days, 'soundboard_triggers'), 'Soundboard triggers']]) {
    const card = element('div'); card.className = 'stat';
    card.append(element('strong', number(value)), element('span', label)); totals.append(card);
  }
  chart('observations', days, [{ label: 'Observed minutes', color: '#7fd1ff', value: (day) => day.points / 6 }]);
  chart('xruns', days, [{ label: 'X-runs', color: '#ff9d82', value: (day) => day.xruns }]);
  chart('health', days, [
    { label: 'CPU (%) - blue', color: '#7fd1ff', value: (day) => day.cpu_percent },
    { label: 'Temperature (C) - green', color: '#b9ff96', value: (day) => day.temperature_c },
  ]);
  bars('effects', effects.flatMap((label, i) => [
    [`${label}: configured`, mean(days, `effect_${i}_0`)],
    [`${label}: enabled`, mean(days, `effect_${i}_1`)],
  ]));
  bars('devices', [['USB audio', 'usb_audio'], ['Gadget', 'gadget_audio'], ['Other ALSA', 'other_audio'],
    ['Bluetooth audio', 'bluetooth_audio'], ['MIDI present', 'midi_connected'],
    ['Input-capable', 'input_devices'], ['Output-capable', 'output_devices']]
    .map(([label, key]) => [label, mean(days, key)]));
  bars('boards', [['Pi 4', 'pi4'], ['Pi 5', 'pi5'], ['Unknown', 'unknown_board']]
    .map(([label, key]) => [label, sum(days, key)]));
  bars('firmware', [['Release-style', 'release_style'], ['Prerelease-style', 'prerelease_style'],
    ['Development/custom', 'custom_style']].map(([label, key]) => [label, sum(days, key)]));
  bars('features', [['Soundboard', 'soundboard_triggers'], ['MIDI note events', 'midi_notes'],
    ['MIDI CC events', 'midi_cc'], ['Scene recalls', 'scene_recalls'], ['Audio-active intervals', 'audio_active']]
    .map(([label, key]) => [label, sum(days, key)]));
  const table = element('table'), head = element('thead'), heading = element('tr'), body = element('tbody');
  for (const label of ['Received date', 'Observations', 'Observed minutes', 'X-runs', 'CPU %', 'Temperature C', 'Soundboard', 'MIDI notes', 'MIDI CC', 'Scenes']) {
    const th = element('th', label); th.scope = 'col'; heading.append(th);
  }
  head.append(heading);
  for (const day of days) {
    const row = element('tr');
    const date = element('th', day.day); date.scope = 'row'; row.append(date);
    for (const value of [day.points, day.points / 6, day.xruns, day.cpu_percent, day.temperature_c,
      day.soundboard_triggers, day.midi_notes, day.midi_cc, day.scene_recalls]) row.append(element('td', number(value)));
    body.append(row);
  }
  table.append(head, body); document.getElementById('daily-table').replaceChildren(table);
}

let loading = false;
async function load() {
  if (loading) return;
  loading = true;
  const button = document.getElementById('refresh'), status = document.getElementById('status');
  button.disabled = true;
  try {
    const response = await fetch('/api/public/summary', {
      credentials: 'omit', cache: 'no-cache', signal: AbortSignal.timeout(15000),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `Dashboard request failed (${response.status}).`);
    render(data);
    status.textContent = `Snapshot updated ${new Date(data.updated_at).toLocaleString()}. Refreshes hourly.`;
    status.removeAttribute('data-error');
  } catch (error) {
    status.textContent = `${error.message} Any charts still visible are from the previous snapshot.`;
    status.setAttribute('data-error', '');
  } finally { loading = false; button.disabled = false; }
}
document.getElementById('refresh').addEventListener('click', load);
load();
setInterval(load, 300000);
