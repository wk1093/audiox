function validFanCurve(settings) {
  if (!settings || !Array.isArray(settings.temperatures) || settings.temperatures.length !== 4
      || !Array.isArray(settings.speeds) || settings.speeds.length !== 4
      || !Number.isInteger(settings.hysteresis) || settings.hysteresis < 1 || settings.hysteresis > 15) {
    return false;
  }
  for (let i = 0; i < 4; i += 1) {
    if (!Number.isInteger(settings.temperatures[i]) || settings.temperatures[i] < 40 || settings.temperatures[i] > 80
        || !Number.isInteger(settings.speeds[i]) || settings.speeds[i] < 1 || settings.speeds[i] > 255) {
      return false;
    }
    if (i > 0 && (settings.temperatures[i] - settings.temperatures[i - 1] <= settings.hysteresis
        || settings.speeds[i] <= settings.speeds[i - 1])) return false;
  }
  return typeof settings.enabled === 'boolean';
}

function renderPi5FanCurve() {
  const { temperatures, speeds, hysteresis, enabled } = state.pi5FanEditor;
  const svg = document.getElementById('pi5-fan-graph');
  const save = document.getElementById('btn-pi5-fan-save');
  const enabledControl = document.getElementById('pi5-fan-enabled');
  const hysteresisControl = document.getElementById('pi5-fan-hysteresis');
  const summary = document.getElementById('pi5-fan-curve-summary');
  if (!svg || !save || !enabledControl || !hysteresisControl || !summary) return;

  const ready = !state.pi5FanIsPi5 || state.pi5FanSettingsLoaded;
  const left = 82, right = 724, top = 38, bottom = 334;
  const x = (temperature) => left + (temperature - 40) / 40 * (right - left);
  const y = (speed) => bottom - speed / 255 * (bottom - top);
  const points = temperatures.map((temperature, index) => ({ x: x(temperature), y: y(speeds[index]) }));
  const line = points.map((point, index) => `${index ? 'L' : 'M'} ${point.x} ${point.y}`).join(' ');
  const area = `${line} L ${points[3].x} ${bottom} L ${points[0].x} ${bottom} Z`;
  const verticalGrid = [40, 50, 60, 70, 80].map((temperature) => {
    const position = x(temperature);
    return `<line class="fan-gridline" x1="${position}" y1="${top}" x2="${position}" y2="${bottom}"/><text x="${position}" y="${bottom + 25}" text-anchor="middle">${temperature}°</text>`;
  }).join('');
  const horizontalGrid = [0, 64, 128, 192, 255].map((speed) => {
    const position = y(speed);
    return `<line class="fan-gridline" x1="${left}" y1="${position}" x2="${right}" y2="${position}"/><text x="${left - 14}" y="${position + 4}" text-anchor="end">${speed}</text>`;
  }).join('');
  const handles = points.map((point, index) => `
    <g class="fan-point${enabled ? '' : ' is-disabled'}" data-step="${index}" tabindex="${enabled && ready ? '0' : '-1'}" role="slider"
      aria-label="Fan curve step ${index + 1}; use left and right arrows to change temperature, up and down to change speed"
      aria-valuemin="40" aria-valuemax="80" aria-valuenow="${temperatures[index]}"
      aria-valuetext="${temperatures[index]} degrees Celsius, fan PWM ${speeds[index]} of 255">
      <title>Step ${index + 1}: ${temperatures[index]} °C, fan speed ${speeds[index]} / 255</title>
      <circle class="fan-point-halo" cx="${point.x}" cy="${point.y}" r="22"/>
      <circle class="fan-point-ring" cx="${point.x}" cy="${point.y}" r="11"/>
      <circle class="fan-point-core" cx="${point.x}" cy="${point.y}" r="4"/>
      <text class="fan-point-label" x="${point.x}" y="${point.y - 20}" text-anchor="middle">${temperatures[index]}° · ${speeds[index]}</text>
    </g>`).join('');
  svg.innerHTML = `
    <defs>
      <linearGradient id="fan-line-gradient" x1="0" x2="1" y1="0" y2="0">
        <stop offset="0%" stop-color="#55d6a5"/><stop offset="55%" stop-color="#e7d35d"/><stop offset="100%" stop-color="#f06b68"/>
      </linearGradient>
      <linearGradient id="fan-area-gradient" x1="0" x2="0" y1="0" y2="1">
        <stop offset="0%" stop-color="#f07167" stop-opacity=".24"/><stop offset="100%" stop-color="#53cda0" stop-opacity=".015"/>
      </linearGradient>
    </defs>
    <text x="20" y="22">FAN SPEED · PWM</text>${horizontalGrid}${verticalGrid}
    <line class="fan-axis" x1="${left}" y1="${bottom}" x2="${right}" y2="${bottom}"/>
    <line class="fan-axis" x1="${left}" y1="${top}" x2="${left}" y2="${bottom}"/>
    <path class="fan-area" d="${area}"/><path class="fan-line" d="${line}"/>
    ${handles}
    <text x="${(left + right) / 2}" y="396" text-anchor="middle">CPU TEMPERATURE · °C</text>`;
  svg.classList.toggle('is-disabled', !enabled || !ready);
  svg.setAttribute('aria-disabled', String(!enabled || !ready));
  enabledControl.checked = enabled;
  enabledControl.disabled = state.pi5FanIsPi5 && !ready;
  hysteresisControl.value = String(hysteresis);
  const maxHysteresis = Math.min(15, ...temperatures.slice(1).map((value, index) => value - temperatures[index] - 1));
  hysteresisControl.max = String(Math.max(1, maxHysteresis));
  hysteresisControl.disabled = !enabled || !ready;
  document.getElementById('pi5-fan-hysteresis-value').textContent = `${hysteresis} °C`;
  summary.replaceChildren(...temperatures.map((temperature, index) => {
    const row = document.createElement('div');
    row.className = 'fan-curve-summary-row';
    const label = document.createElement('span');
    label.textContent = `Step ${index + 1}`;
    const value = document.createElement('strong');
    value.textContent = `${temperature} °C · PWM ${speeds[index]}`;
    row.append(label, value);
    return row;
  }));
  save.disabled = !state.pi5FanIsPi5 || !ready;
}

function editPi5FanPoint(index, temperatureDelta, speedDelta, keepFocus = false) {
  const curve = state.pi5FanEditor;
  if (!curve.enabled || (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded)) return;
  const minTemperature = index === 0 ? 40 : curve.temperatures[index - 1] + curve.hysteresis + 1;
  const maxTemperature = index === 3 ? 80 : curve.temperatures[index + 1] - curve.hysteresis - 1;
  const minSpeed = index === 0 ? 1 : curve.speeds[index - 1] + 1;
  const maxSpeed = index === 3 ? 255 : curve.speeds[index + 1] - 1;
  curve.temperatures[index] = Math.max(minTemperature,
    Math.min(maxTemperature, curve.temperatures[index] + temperatureDelta));
  curve.speeds[index] = Math.max(minSpeed, Math.min(maxSpeed, curve.speeds[index] + speedDelta));
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
  if (keepFocus) document.querySelector(`#pi5-fan-graph [data-step="${index}"]`)?.focus();
}

function updatePi5FanPointFromPointer(event, index) {
  if (!state.pi5FanEditor.enabled || (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded)) return;
  const svg = document.getElementById('pi5-fan-graph');
  const matrix = svg.getScreenCTM();
  if (!matrix) return;
  const pointer = svg.createSVGPoint();
  pointer.x = event.clientX;
  pointer.y = event.clientY;
  const position = pointer.matrixTransform(matrix.inverse());
  const left = 82, right = 724, top = 38, bottom = 334;
  const temperature = Math.round(40 + (position.x - left) / (right - left) * 40);
  const speed = Math.round((bottom - position.y) / (bottom - top) * 255);
  const curve = state.pi5FanEditor;
  const minTemperature = index === 0 ? 40 : curve.temperatures[index - 1] + curve.hysteresis + 1;
  const maxTemperature = index === 3 ? 80 : curve.temperatures[index + 1] - curve.hysteresis - 1;
  const minSpeed = index === 0 ? 1 : curve.speeds[index - 1] + 1;
  const maxSpeed = index === 3 ? 255 : curve.speeds[index + 1] - 1;
  const nextTemperature = Math.max(minTemperature, Math.min(maxTemperature, temperature));
  const nextSpeed = Math.max(minSpeed, Math.min(maxSpeed, speed));
  if (nextTemperature === curve.temperatures[index] && nextSpeed === curve.speeds[index]) return;
  curve.temperatures[index] = nextTemperature;
  curve.speeds[index] = nextSpeed;
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
}

async function loadPi5FanSettings() {
  if (!state.pi5FanIsPi5 || state.pi5FanSettingsLoading) return;
  state.pi5FanSettingsLoading = true;
  const status = document.getElementById('pi5-fan-status');
  status.textContent = 'Loading saved fan curve…';
  renderPi5FanCurve();
  try {
    const response = await apiFetch('/api/system/fan', { cache: 'no-store' });
    const text = (await response.text()).trim();
    if (!response.ok) throw new Error(`${response.status} ${text}`);
    const settings = JSON.parse(text);
    if (!validFanCurve(settings)) throw new Error('device returned an invalid fan curve');
    state.pi5FanEditor = {
      enabled: settings.enabled,
      temperatures: settings.temperatures,
      speeds: settings.speeds,
      hysteresis: settings.hysteresis,
    };
    state.pi5FanSettingsLoaded = true;
    AudioxUI.setUnsaved('pi5-fan', false);
    status.textContent = settings.enabled
      ? 'Saved fan curve loaded. Save changes, then restart to apply.'
      : 'No AudioX-managed curve is set. Firmware defaults or other boot-config fan settings remain active.';
    renderPi5FanCurve();
  } catch (error) {
    status.textContent = `Could not load Pi 5 fan settings: ${error.message}. Close and reopen to retry.`;
  } finally {
    state.pi5FanSettingsLoading = false;
    renderPi5FanCurve();
  }
}

function openPi5FanDialog() {
  const dialog = document.getElementById('pi5-fan-dialog');
  const warning = document.getElementById('pi5-fan-warning');
  const status = document.getElementById('pi5-fan-status');
  warning.hidden = state.pi5FanIsPi5;
  if (state.pi5FanIsPi5) {
    status.textContent = state.pi5FanSettingsLoaded
      ? status.textContent : 'Loading saved fan curve…';
  } else {
    status.textContent = 'Preview mode · saving disabled';
  }
  renderPi5FanCurve();
  dialog.showModal();
  if (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded) loadPi5FanSettings();
}

const pi5FanEnabled = document.getElementById('pi5-fan-enabled');
const pi5FanStatus = document.getElementById('pi5-fan-status');
const pi5FanGraph = document.getElementById('pi5-fan-graph');
document.getElementById('btn-pi5-fan-open').addEventListener('click', openPi5FanDialog);
document.getElementById('btn-pi5-fan-close').addEventListener('click', () => {
  document.getElementById('pi5-fan-dialog').close();
});
pi5FanGraph.addEventListener('pointerdown', (event) => {
  const point = event.target.closest?.('[data-step]');
  if (!point || !state.pi5FanEditor.enabled || (state.pi5FanIsPi5 && !state.pi5FanSettingsLoaded)) return;
  state.pi5FanDragging = Number(point.getAttribute('data-step'));
  pi5FanGraph.setPointerCapture(event.pointerId);
  event.preventDefault();
});
pi5FanGraph.addEventListener('pointermove', (event) => {
  if (state.pi5FanDragging !== undefined) updatePi5FanPointFromPointer(event, state.pi5FanDragging);
});
const finishFanDrag = (event) => {
  if (state.pi5FanDragging === undefined) return;
  state.pi5FanDragging = undefined;
  if (pi5FanGraph.hasPointerCapture(event.pointerId)) pi5FanGraph.releasePointerCapture(event.pointerId);
};
pi5FanGraph.addEventListener('pointerup', finishFanDrag);
pi5FanGraph.addEventListener('pointercancel', finishFanDrag);
pi5FanGraph.addEventListener('keydown', (event) => {
  const point = event.target.closest?.('[data-step]');
  if (!point) return;
  const step = Number(point.getAttribute('data-step'));
  const precision = event.shiftKey ? 1 : 5;
  if (event.key === 'ArrowLeft') editPi5FanPoint(step, -1, 0, true);
  else if (event.key === 'ArrowRight') editPi5FanPoint(step, 1, 0, true);
  else if (event.key === 'ArrowUp') editPi5FanPoint(step, 0, precision, true);
  else if (event.key === 'ArrowDown') editPi5FanPoint(step, 0, -precision, true);
  else return;
  event.preventDefault();
});
pi5FanEnabled.addEventListener('change', () => {
  state.pi5FanEditor.enabled = pi5FanEnabled.checked;
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
});
document.getElementById('pi5-fan-hysteresis').addEventListener('input', (event) => {
  state.pi5FanEditor.hysteresis = Number(event.currentTarget.value);
  renderPi5FanCurve();
  AudioxUI.setUnsaved('pi5-fan', state.pi5FanIsPi5 && state.pi5FanSettingsLoaded);
});
document.getElementById('btn-pi5-fan-save').addEventListener('click', async () => {
  if (!state.pi5FanIsPi5 || !state.pi5FanSettingsLoaded) {
    pi5FanStatus.textContent = 'Fan curves can only be saved from a Raspberry Pi 5.';
    return;
  }
  const curve = state.pi5FanEditor;
  if (!validFanCurve(curve)) {
    pi5FanStatus.textContent = 'Invalid curve. Temperatures and speeds must increase at every step.';
    return;
  }
  const settings = { enabled: curve.enabled ? 1 : 0 };
  for (let i = 0; i < 4; i += 1) {
    settings[`temperature${i}`] = curve.temperatures[i];
    settings[`speed${i}`] = curve.speeds[i];
  }
  settings.hysteresis = curve.hysteresis;
  try {
    const response = await apiFetch('/api/system/fan', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams(settings).toString(),
    });
    const text = (await response.text()).trim();
    if (!response.ok) throw new Error(`${response.status} ${text}`);
    const saved = JSON.parse(text);
    if (!validFanCurve(saved)) throw new Error('device returned an invalid fan curve');
    state.pi5FanEditor = {
      enabled: saved.enabled,
      temperatures: saved.temperatures,
      speeds: saved.speeds,
      hysteresis: saved.hysteresis,
    };
    state.pi5FanSettingsLoaded = true;
    AudioxUI.setUnsaved('pi5-fan', false);
    renderPi5FanCurve();
    pi5FanStatus.textContent = saved.enabled
      ? 'Fan curve saved to boot config. Restart the device to apply it.'
      : 'AudioX curve removed. Firmware defaults or other boot-config fan settings will apply after restart.';
  } catch (error) {
    pi5FanStatus.textContent = `Fan curve save failed: ${error.message}`;
  }
});

