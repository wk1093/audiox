(function (root) {
  'use strict';

  const historyRetentionMs = 600000;
  const historyCapacity = 600;

  function createHistory(limit = historyCapacity) {
    let samples = [], runtime = null;
    return {
      merge(data) {
        if (typeof data?.runtime_id !== 'string' || !Array.isArray(data.samples)
            || data.samples.length > 1000 || data.samples.some((sample, i) =>
              !Number.isSafeInteger(sample.id) || sample.id < 1 || !Number.isFinite(sample.now)
              || sample.now < 0 || (i && (sample.id <= data.samples[i - 1].id || sample.now <= data.samples[i - 1].now))
              || ['temperature', 'cpu', 'xruns'].some((key) => sample[key] !== null && !Number.isFinite(sample[key]))))
          throw new Error('Invalid device history response.');
        if (runtime !== data.runtime_id) samples = [];
        runtime = data.runtime_id;
        const merged = new Map(samples.map((sample) => [sample.id, sample]));
        for (const sample of data.samples) merged.set(sample.id, { ...sample });
        const now = Number.isFinite(data.now_ms) ? data.now_ms
          : Math.max(0, ...[...merged.values()].map((sample) => sample.now));
        samples = [...merged.values()].filter((sample) =>
          sample.now <= now && now - sample.now < historyRetentionMs)
          .sort((a, b) => a.id - b.id).slice(-Math.min(limit, historyCapacity));
        return samples;
      },
      lastId() { return samples.at(-1)?.id || 0; },
      runtime() { return runtime; },
    };
  }

  function historySvg(samples, now = samples.at(-1)?.now) {
    samples = samples.filter((sample) => sample.now <= now && now - sample.now < historyRetentionMs);
    if (!samples.length) return '';
    const end = now;
    const duration = historyRetentionMs;
    const start = end - duration;
    const x = (now) => 45 + (now - start) / duration * 710;
    const maxXruns = Math.max(1, ...samples.map((sample) => sample.xruns || 0));
    const lines = [['temperature', '#ff9d82', 100], ['cpu', '#7fd1ff', 100], ['xruns', '#b9ff96', maxXruns]];
    let svg = '<title>Device temperature, CPU utilization, and X-runs</title>';
    for (const tick of [0, 25, 50, 75, 100]) {
      const y = 185 - tick / 100 * 160;
      svg += `<path d="M45 ${y} H755" stroke="currentColor" opacity=".15"/><text x="38" y="${y + 4}" text-anchor="end">${tick}</text>`;
    }
    for (const [key, color, max] of lines) {
      let path = '', previous = null;
      for (const sample of samples) {
        const value = sample[key];
        if (value == null || !Number.isFinite(value)) { previous = null; continue; }
        const move = !previous || sample.now - previous.now > 5000;
        path += `${move ? 'M' : 'L'}${x(sample.now).toFixed(1)} ${(185 - Math.max(0, Math.min(max, value)) / max * 160).toFixed(1)} `;
        previous = sample;
      }
      svg += `<path d="${path}" stroke="${color}" stroke-width="2" fill="none"/>`;
    }
    svg += `<text x="45" y="210">${Math.round(duration / 60000)} minutes ago</text><text x="755" y="210" text-anchor="end">Now</text>`;
    svg += `<text x="762" y="29">${maxXruns} X-runs</text><text x="762" y="189">0</text>`;
    return svg;
  }

  function deviceRows(data, remembered = new Map()) {
    if (!Array.isArray(data?.detected?.devices) || !Array.isArray(data?.streams))
      throw new Error('Invalid audio device health response.');
    for (const row of remembered.values()) row.connected = false;
    for (const device of data.detected.devices) {
      if (device.gadget) continue;
      const key = `${device.stableCardId || `card${device.card}`}_dev${device.device}`;
      remembered.set(key, { key, name: String(device.name || device.node || key), connected: true,
        streams: data.streams.filter((stream) => stream.id === `alsa_${key}_in`
          || stream.id === `alsa_${key}_out` || stream.id === `alsa_card${device.card}_dev${device.device}_in`
          || stream.id === `alsa_card${device.card}_dev${device.device}_out`) });
    }
    for (const stream of data.streams) {
      if (!stream.id.startsWith('usb_gadget')) continue;
      remembered.set(stream.id, { key: stream.id, name: 'USB audio gadget', connected: true, streams: [stream] });
    }
    while (remembered.size > 64) {
      const offline = [...remembered].find(([, row]) => !row.connected);
      if (!offline) break;
      remembered.delete(offline[0]);
    }
    return remembered;
  }

  function flagSummary(flag) {
    if (flag?.status !== 'available') return `Status ${flag?.status || 'unavailable'}`;
    if (flag.current === true) return 'Active now';
    return flag.since_boot === true ? 'Since boot (inactive)' : 'OK';
  }

  function fanSummary(fan) {
    if (fan?.module_status === 'available' && fan?.device_status
        && fan.device_status !== 'available')
      return `Fan module loaded; fan device ${fan.device_status}`;
    if (!fan || fan.driver_status !== 'available')
      return `Fan driver ${fan?.driver_status || 'unavailable'}`;
    const rpm = fan.rpm_status === 'available' ? `${fan.rpm} RPM` : `RPM ${fan.rpm_status}`;
    const cooling = fan.cooling_status === 'available'
      ? `Cooling step ${fan.cooling_state}/${fan.cooling_max_state}` : `Cooling state ${fan.cooling_status}`;
    const curve = fan.saved_matches_active === true ? 'Saved curve is active'
      : fan.saved_matches_active === false ? 'Saved curve differs from active; restart to apply'
        : fan.saved_curve?.status === 'missing' ? 'No custom curve'
          : 'Curve status unavailable';
    const stalled = fan.stall_warning === true ? 'WARNING: cooling requested but fan remains stopped'
      : fan.cooling_requested === false && fan.rpm === 0 ? 'Fan intentionally stopped'
        : fan.stall_status !== 'available' ? 'Stall detection unavailable' : '';
    return [rpm, cooling, curve, stalled].filter(Boolean).join(' · ');
  }

  function curveSummary(curve) {
    if (curve?.status !== 'available' || !Array.isArray(curve.temperatures_c)
        || !Array.isArray(curve.speeds_pwm)) return `Curve ${curve?.status || 'unavailable'}`;
    return curve.temperatures_c.map((temperature, i) =>
      `${temperature} °C → PWM ${curve.speeds_pwm[i]}`).join(' · ') + `; hysteresis ${curve.hysteresis_c} °C`;
  }

  function attach() {
    const history = createHistory();
    const remembered = new Map();
    const advanced = document.getElementById('system-advanced');
    let loading = false;
    let historyLoading = false, historyNeedsFull = true;
    let lastHardwareWarning = '';
    const text = (id, value) => { document.getElementById(id).textContent = value; };
    async function loadHistory() {
      if (historyLoading || !advanced.open) return;
      historyLoading = true;
      const status = document.getElementById('system-history-status');
      const request = async (limit) => {
        const response = await fetch(`/api/system/history?limit=${limit}`, { cache: 'no-store', signal: AbortSignal.timeout(10000) });
        if (!response.ok) throw new Error(`History request failed (${response.status}).`);
        return response.json();
      };
      try {
        let data = await request(historyNeedsFull ? historyCapacity : 50);
        if (!historyNeedsFull && history.runtime() === data.runtime_id && history.lastId()
            && data.samples?.[0]?.id > history.lastId() + 1) data = await request(historyCapacity);
        const samples = history.merge(data);
        historyNeedsFull = false;
        document.getElementById('system-history').innerHTML = historySvg(samples, data.now_ms);
        const stale = samples.length && data.now_ms - samples.at(-1).now > 5000;
        status.textContent = !samples.length ? 'No samples yet.' : stale ? 'History is stale.' : '';
        status.hidden = samples.length > 0 && !stale;
        root.AudioxUI.resolve('system-history');
      } catch (error) {
        status.hidden = false;
        status.textContent = 'History unavailable.';
        root.AudioxUI.notify(error, 'warn', 'system-history');
      } finally { historyLoading = false; }
    }
    async function devices() {
      if (loading) return;
      loading = true;
      try {
        const response = await fetch('/api/system/audio-devices', { cache: 'no-store', signal: AbortSignal.timeout(10000) });
        if (!response.ok) throw new Error(`Audio health request failed (${response.status}).`);
        const data = await response.json();
        const rows = deviceRows(data, remembered);
        const host = document.getElementById('system-device-health');
        host.replaceChildren();
        for (const row of rows.values()) {
          const item = document.createElement('div');
          item.className = 'metric';
          const title = document.createElement('strong');
          title.textContent = row.name;
          const status = document.createElement('p');
          status.className = 'small';
          status.textContent = !row.connected ? 'Disconnected'
            : data.stale ? 'Connected · readings stale'
            : !row.streams.length ? 'Connected · idle'
              : row.streams.map((stream) => `${stream.capture ? 'Capture' : 'Playback'}: ${stream.active
                ? `${stream.rate} Hz, ${stream.mode}, period ${stream.period_frames}, buffer ${stream.buffer_frames} frames`
                : 'offline / retrying'}; ${stream.xruns} X-runs`).join(' · ');
          item.append(title, status);
          host.append(item);
        }
        text('system-device-health-status', data.stale ? 'Stream readings stale.'
          : rows.size ? '' : 'No audio devices detected.');
        document.getElementById('system-device-health-status').hidden = !data.stale && rows.size > 0;
        root.AudioxUI.resolve('device-health');
      } catch (error) {
        document.getElementById('system-device-health-status').hidden = false;
        text('system-device-health-status', 'Audio device health unavailable.');
        root.AudioxUI.notify(error, 'warn', 'device-health');
      } finally { loading = false; }
    }
    advanced.addEventListener('toggle', () => {
      if (advanced.open) {
        historyNeedsFull = true;
        loadHistory();
        devices();
      }
    });
    document.getElementById('btn-system-diagnostics').addEventListener('click', async () => {
      try {
        const response = await fetch('/api/system/diagnostics', { cache: 'no-store', signal: AbortSignal.timeout(10000) });
        if (!response.ok) throw new Error(`Diagnostics request failed (${response.status}).`);
        const report = await response.json();
        if (report.format !== 'audiox-diagnostics-v1') throw new Error('Invalid diagnostic report.');
        const url = URL.createObjectURL(new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' }));
        const link = document.createElement('a');
        link.href = url;
        link.download = 'audiox-diagnostics.json';
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
        root.AudioxUI.notify('Diagnostic summary downloaded.', 'ok');
      } catch (error) { root.AudioxUI.notify(error, 'warn', 'diagnostics'); }
    });
    return {
      update(data) {
        const hardware = data.hardware;
        text('sysinfo-undervoltage', flagSummary(hardware?.undervoltage));
        text('sysinfo-throttling', flagSummary(hardware?.throttling));
        text('sysinfo-soft-temperature', flagSummary(hardware?.soft_temperature_limit));
        text('sysinfo-fan', fanSummary(hardware?.fan));
        text('pi5-fan-live', fanSummary(hardware?.fan));
        text('pi5-fan-active-curve', curveSummary(hardware?.fan?.active_curve));
        text('pi5-fan-saved-curve', hardware?.fan?.saved_curve?.status === 'missing'
          ? 'No AudioX-managed curve saved' : curveSummary(hardware?.fan?.saved_curve));
        document.getElementById('system-hardware-health').dataset.warning = String(
          hardware?.undervoltage?.current === true || hardware?.thermal_throttling?.current === true
          || hardware?.fan?.stall_warning === true);
        const warnings = [];
        if (hardware?.undervoltage?.current === true) warnings.push('Undervoltage is active; check the power supply and cable.');
        if (hardware?.thermal_throttling?.current === true) warnings.push('Throttling or a thermal limit is active; check power and temperature.');
        if (hardware?.fan?.stall_warning === true) warnings.push('Fan cooling is requested but RPM remains zero; check the fan.');
        const warning = warnings.join(' ');
        if (warning && warning !== lastHardwareWarning) root.AudioxUI.notify(warning, 'warn', 'hardware-health');
        if (!warning) root.AudioxUI.resolve('hardware-health');
        lastHardwareWarning = warning;
        if (advanced.open) { devices(); loadHistory(); }
      },
      unavailable() {
        document.getElementById('system-history-status').hidden = false;
        text('system-history-status', 'Device unreachable.');
        text('sysinfo-undervoltage', 'Unavailable; device unreachable');
        text('sysinfo-throttling', 'Unavailable; device unreachable');
        text('sysinfo-soft-temperature', 'Unavailable; device unreachable');
        text('sysinfo-fan', 'Unavailable; device unreachable');
        text('pi5-fan-live', 'Unavailable; device unreachable');
        text('pi5-fan-active-curve', 'Active curve unavailable; device unreachable');
        text('pi5-fan-saved-curve', 'Saved curve unavailable; device unreachable');
        document.getElementById('system-hardware-health').dataset.warning = 'false';
      },
    };
  }

  const api = { createHistory, historySvg, deviceRows, flagSummary, fanSummary, curveSummary, attach };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.AudioxSystemMonitor = attach();
})(typeof window !== 'undefined' ? window : globalThis);
