async function pollSystemIfNeeded() {
  if (AudioxUI.isBusy()) return;
  if (state.activeTab !== 'system') {
    return;
  }
  if (document.visibilityState && document.visibilityState !== 'visible') {
    return;
  }
  if (state.systemPollInFlight) {
    return;
  }

  state.systemPollInFlight = true;
  try {
    await Promise.all([
      loadSystemInfo(),
      loadLogs(),
    ]);
  } finally {
    state.systemPollInFlight = false;
  }
}

function startSystemPolling() {
  if (state.systemPollTimer) {
    clearInterval(state.systemPollTimer);
  }
  state.systemPollTimer = setInterval(() => {
    pollSystemIfNeeded();
  }, 1000);
}

function stopSystemPolling() {
  if (!state.systemPollTimer) {
    return;
  }
  clearInterval(state.systemPollTimer);
  state.systemPollTimer = null;
}
async function loadVersion() {
  try {
    const res = await apiFetch('/api/version', { method: 'GET' });
    const txt = (await res.text()).trim();
    if (!res.ok || !txt) {
      return;
    }
    appVersionEl.textContent = `version ${txt}`;
  } catch (err) {
  }
}

function renderResourceMeter(id, value, max, warningAt, criticalAt, valueText) {
  const meter = document.getElementById(id);
  if (!meter) return;
  if (!Number.isFinite(value) || !Number.isFinite(max) || max <= 0) {
    meter.hidden = true;
    return;
  }
  const bounded = Math.max(0, Math.min(max, value));
  const percent = bounded / max * 100;
  const palettes = {
    'sysinfo-temperature-meter': [[0, 53, 190, 151], [66.7, 53, 190, 151], [83.3, 241, 195, 72], [100, 239, 79, 91]],
    'sysinfo-memory-meter': [[0, 53, 190, 151], [70, 53, 190, 151], [85, 241, 195, 72], [100, 239, 79, 91]],
    'sysinfo-storage-meter': [[0, 53, 190, 151], [65, 53, 190, 151], [75, 241, 195, 72], [90, 239, 79, 91], [100, 239, 79, 91]],
    'sysinfo-load-meter': [[0, 53, 190, 151], [60, 53, 190, 151], [80, 241, 195, 72], [100, 239, 79, 91]],
    'sysinfo-cpu-utilization-meter': [[0, 53, 190, 151], [65, 53, 190, 151], [80, 241, 195, 72], [100, 239, 79, 91]],
  };
  const stops = palettes[id];
  if (stops) {
    const position = percent;
    let upper = stops.findIndex(([stop]) => stop >= position);
    if (upper < 0) upper = stops.length - 1;
    const lower = Math.max(0, upper - 1);
    const [lowerAt, ...lowerRgb] = stops[lower];
    const [upperAt, ...upperRgb] = stops[upper];
    const amount = upperAt === lowerAt ? 0 : (position - lowerAt) / (upperAt - lowerAt);
    const rgb = lowerRgb.map((channel, index) => Math.round(channel + (upperRgb[index] - channel) * amount));
    const rgbCss = `rgb(${rgb.join(', ')})`;
    meter.style.setProperty('--meter-color', rgbCss);
    meter.style.setProperty('--meter-color-soft',
      `rgb(${rgb.map((channel) => Math.round(channel + (255 - channel) * 0.28)).join(', ')})`);
    meter.style.setProperty('--meter-glow', `rgba(${rgb.join(', ')}, 0.28)`);
  }
  meter.style.setProperty('--meter-level', `${percent}%`);
  meter.dataset.state = bounded >= criticalAt ? 'critical' : bounded >= warningAt ? 'warning' : 'normal';
  meter.setAttribute('aria-valuenow', String(Math.round(bounded * 10) / 10));
  meter.setAttribute('aria-valuetext', valueText || `${Math.round(percent)}%`);
  meter.hidden = false;
}

function calculateCpuUtilization(previous, current) {
  if (!previous || !current
      || !Number.isSafeInteger(previous.total_ticks) || !Number.isSafeInteger(previous.idle_ticks)
      || !Number.isSafeInteger(current.total_ticks) || !Number.isSafeInteger(current.idle_ticks)) return null;
  if (current.total_ticks < previous.total_ticks) return null;
  const hasCapacity = previous.capacity_ticks !== undefined || current.capacity_ticks !== undefined;
  if (hasCapacity && (!Number.isSafeInteger(previous.capacity_ticks)
      || !Number.isSafeInteger(current.capacity_ticks)
      || !Number.isSafeInteger(current.cpu_count) || current.cpu_count < 1
      || current.cpu_count !== previous.cpu_count)) return null;
  // Older firmware only supplies raw tick counters.
  const totalDelta = hasCapacity ? current.capacity_ticks - previous.capacity_ticks
    : current.total_ticks - previous.total_ticks;
  const idleDelta = current.idle_ticks - previous.idle_ticks;
  if (totalDelta <= 0 || idleDelta < 0 || idleDelta > totalDelta) return null;
  return Math.max(0, Math.min(100, (totalDelta - idleDelta) / totalDelta * 100));
}

function renderStorageUsage(storage) {
  const value = document.getElementById('sysinfo-storage');
  const free = document.getElementById('sysinfo-storage-free');
  const fields = ['total_bytes', 'used_bytes', 'free_bytes', 'reserved_bytes'];
  if (!storage || !fields.every((key) => Number.isSafeInteger(storage[key]) && storage[key] >= 0)
      || storage.total_bytes === 0
      || storage.used_bytes + storage.free_bytes + storage.reserved_bytes !== storage.total_bytes) {
    value.textContent = 'Unavailable';
    renderResourceMeter('sysinfo-storage-meter', NaN, 100, 70, 90);
    free.textContent = 'Storage usage could not be read.';
    return;
  }
  const format = (bytes) => bytes >= 1024 ** 3
    ? `${(bytes / 1024 ** 3).toFixed(1)} GiB`
    : `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  value.textContent = `${format(storage.used_bytes)} / ${format(storage.total_bytes)} used`;
  const occupiedPercent = (storage.total_bytes - storage.free_bytes) / storage.total_bytes * 100;
  renderResourceMeter('sysinfo-storage-meter', occupiedPercent, 100, 70, 90,
    `${occupiedPercent.toFixed(1)}% used or reserved`);
  free.textContent = `${format(storage.free_bytes)} free`
    + (storage.reserved_bytes > 0 ? ` (${format(storage.reserved_bytes)} reserved)` : '');
}

async function loadSystemInfo() {
  try {
    const data = await AudioxUI.probeDevice();

    const vEl = document.getElementById('sysinfo-version');
    const kEl = document.getElementById('sysinfo-kernel');
    const modelEl = document.getElementById('sysinfo-model');
    const cpuClockEl = document.getElementById('sysinfo-cpu-clock');
    const temperatureEl = document.getElementById('sysinfo-temperature');
    const temperatureMeter = document.getElementById('sysinfo-temperature-meter');
    state.pi5FanIsPi5 = data.board === 'pi5';
    const uEl = document.getElementById('sysinfo-uptime');
    const mEl = document.getElementById('sysinfo-memory');
    const lEl = document.getElementById('sysinfo-load');
    const fEl = document.getElementById('sysinfo-ffmpeg');
    const audioBufferEl = document.getElementById('sysinfo-audio-buffer');
    const audioBufferDetailEl = document.getElementById('sysinfo-audio-buffer-detail');
    const stressPanel = document.getElementById('system-audio-stress');
    const stressStatus = document.getElementById('audio-stress-status');
    const stressAudioButton = document.getElementById('btn-audio-stress-thread');
    const stressCpuButton = document.getElementById('btn-audio-stress-cpu');
    const stressStopButton = document.getElementById('btn-audio-stress-stop');
    if (!vEl) return;

    vEl.textContent = data.version ? `v${data.version}` : '\u2014';
    kEl.textContent = data.kernel || '\u2014';
    if (modelEl) {
      modelEl.textContent = data.model || '\u2014';
    }
    if (cpuClockEl) {
      const cpuFreq = Number(data.cpu_freq_mhz || 0);
      cpuClockEl.textContent = cpuFreq > 0 ? `${cpuFreq} MHz` : '\u2014';
    }
    if (temperatureEl && temperatureMeter) {
      const temperature = Number(data.temperature_c);
      if (data.temperature_c !== null && data.temperature_c !== undefined
          && Number.isFinite(temperature) && temperature >= 0 && temperature <= 150) {
        temperatureEl.textContent = `${temperature.toFixed(1)} °C`;
        renderResourceMeter('sysinfo-temperature-meter', temperature, 90, 60, 75,
          `${temperature.toFixed(1)} degrees Celsius`);
      } else {
        temperatureEl.textContent = 'Unavailable';
        renderResourceMeter('sysinfo-temperature-meter', NaN, 90, 60, 75);
      }
    }
    const secs = Number(data.uptime_secs || 0);
    const d = Math.floor(secs / 86400);
    const h = Math.floor((secs % 86400) / 3600);
    const m = Math.floor((secs % 3600) / 60);
    uEl.textContent = d > 0 ? `${d}d ${h}h ${m}m` : `${h}h ${m}m`;

    const totalMb = Number(data.mem_total_mb || 0);
    const availMbRaw = Number(data.mem_avail_mb || 0);
    const availMb = Math.max(0, Math.min(totalMb, availMbRaw));
    const usedMb = Math.max(0, totalMb - availMb);
    if (Number.isFinite(totalMb) && totalMb > 0 && Number.isFinite(availMbRaw)) {
      mEl.textContent = `${usedMb} / ${totalMb} MB`;
      const memoryPercent = usedMb / totalMb * 100;
      renderResourceMeter('sysinfo-memory-meter', memoryPercent, 100, 75, 90,
        `${memoryPercent.toFixed(1)}% of memory used`);
    } else {
      mEl.textContent = 'Unavailable';
      renderResourceMeter('sysinfo-memory-meter', NaN, 100, 75, 90);
    }
    renderStorageUsage(data.storage);

    const cpuSample = data.cpu_times;
    const utilization = calculateCpuUtilization(state.cpuTimesSample, cpuSample);
    state.cpuTimesSample = cpuSample && Number.isSafeInteger(cpuSample.total_ticks)
      && Number.isSafeInteger(cpuSample.idle_ticks) ? cpuSample : null;
    const cpuUtilization = document.getElementById('sysinfo-cpu-utilization');
    if (Number.isFinite(utilization)) {
      cpuUtilization.textContent = `${utilization.toFixed(1)}%`;
      renderResourceMeter('sysinfo-cpu-utilization-meter', utilization, 100, 70, 90,
        `${utilization.toFixed(1)}% CPU utilization`);
    } else {
      cpuUtilization.textContent = state.cpuTimesSample ? 'Sampling…' : 'Unavailable';
      renderResourceMeter('sysinfo-cpu-utilization-meter', NaN, 100, 70, 90);
    }

    const load = Number(data.load1);
    const onlineCpus = Number(data.cpu_count);
    if (Number.isFinite(load) && load >= 0) {
      lEl.textContent = load.toFixed(2);
      if (Number.isFinite(onlineCpus) && onlineCpus > 0) {
        const loadPercent = load / onlineCpus * 100;
        renderResourceMeter('sysinfo-load-meter', loadPercent, 100, 70, 100,
          `${load.toFixed(2)} load average relative to ${onlineCpus} online CPUs`);
      } else {
        renderResourceMeter('sysinfo-load-meter', NaN, 100, 70, 100);
      }
    } else {
      lEl.textContent = 'Unavailable';
      renderResourceMeter('sysinfo-load-meter', NaN, 100, 70, 100);
    }

    if (fEl) {
      fEl.textContent = data.ffmpeg || '\u2014';
    }
    if (audioBufferEl && audioBufferDetailEl) {
      const buffer = data.audio_buffer;
      const base = Number(buffer?.base_block_frames);
      const target = Number(buffer?.target_period_frames);
      const period = Number(buffer?.largest_period_frames);
      const largest = Number(buffer?.largest_buffer_frames);
      const streams = Number(buffer?.active_streams);
      const latency = Number(buffer?.estimated_largest_buffer_ms);
      const xruns = Number(data.audio_health?.xruns);
      const downshiftDelay = Number(buffer?.downshift_delay_ms);
      const downshiftFailures = Number(buffer?.downshift_failures);
      const downshiftDelaySeconds = Number.isFinite(downshiftDelay)
        ? Math.round(downshiftDelay / 1000) : 0;
      const downshiftFailureCount = Number.isFinite(downshiftFailures) ? downshiftFailures : 0;
      if (Number.isFinite(base) && base > 0 && Number.isFinite(target) && target > 0) {
        audioBufferEl.textContent = target > base
          ? `${target} frames (adaptive)`
          : `${target} frames (base)`;
        audioBufferDetailEl.textContent = streams > 0 && Number.isFinite(largest) && largest > 0
          ? `Period ${Number.isFinite(period) && period > 0 ? period : '—'} · buffer ${largest} frames (~${latency.toFixed(1)} ms capacity) · ${streams} streams · ${Number.isFinite(xruns) ? xruns : '—'} X-runs${target > base ? ` · downshift after ${downshiftDelaySeconds}s quiet · ${downshiftFailureCount} failed downshifts` : ''}`
          : `No active streams · ${Number.isFinite(xruns) ? xruns : '—'} X-runs`;
      } else {
        audioBufferEl.textContent = 'Unavailable';
        audioBufferDetailEl.textContent = 'Audio buffer status could not be read.';
      }
    }
    showAudioHealth(data);
    window.AudioxSystemMonitor?.update(data, utilization);
    if (stressPanel) {
      const stress = data.audio_stress_test;
      stressPanel.hidden = stress?.available !== true;
      if (stressStatus && stress) {
        stressStatus.textContent = stress.mode === 'idle'
          ? 'No stress test running.'
          : `${stress.mode === 'audio' ? 'Audio-thread overload' : 'System CPU contention'} active; ${Number(stress.remaining_ms || 0)} ms remain${stress.mode === 'cpu' ? ` (${Number(stress.workers || 0)} workers)` : ''}.`;
      }
      const running = stress?.mode !== 'idle';
      if (stressAudioButton) stressAudioButton.disabled = running;
      if (stressCpuButton) stressCpuButton.disabled = running;
      if (stressStopButton) stressStopButton.disabled = !running;
    }
    AudioxUI.resolve('system-info');
  } catch (err) {
    renderStorageUsage(null);
    window.AudioxSystemMonitor?.unavailable();
    setStatus(`device info load error: ${err}`, 'warn', 'system-info');
  }
}

async function runSystemAction(path, label) {
  try {
    await AudioxUI.runTask({
      sync: 'Syncing filesystem', restart: 'Restarting AudioX', shutdown: 'Shutting down AudioX',
    }[label], async () => {
      AudioxUI.taskProgress(`${label} in progress...`);
      let res;
      try {
        res = await apiFetch(path, { method: 'POST', signal: AbortSignal.timeout(15000) });
      } catch (error) {
        if (label !== 'restart' || !(error instanceof TypeError || error.name === 'TimeoutError')) throw error;
        AudioxUI.taskProgress('Connection interrupted after the restart request. Checking whether AudioX restarts...');
        return;
      }
      const txt = (await res.text()).trim();
      if (!res.ok) throw new Error(`${label} failed: ${res.status} ${txt}`);
    }, { restart: label === 'restart' });
    if (label === 'shutdown') {
      stopConnectionMonitor();
      stopSystemPolling();
    }
    const messages = {
      sync: 'Filesystem synced. Saved files have been flushed to storage.',
      restart: 'AudioX restarted and reconnected.',
      shutdown: 'Shutdown requested. Wait for the device to power off before disconnecting it.',
    };
    setStatus(messages[label] || `${label} completed`, 'ok', `system-${label}`);
    return true;
  } catch (err) {
    setStatus(`${label} error: ${err}`, 'warn', `system-${label}`);
    return false;
  }
}

async function requestAudioStress(mode) {
  const duration = Number(document.getElementById('audio-stress-duration').value);
  const workMs = Number(document.getElementById('audio-stress-work').value);
  if (mode !== 'stop' && !await AudioxUI.confirm(
    mode === 'audio'
      ? 'This intentionally overloads the audio thread and can cause audible dropouts. Start the bounded test?'
      : 'This runs CPU workers on all available cores (up to 8) and can cause system-wide audio dropouts. Start the bounded test?',
    { title: 'Run audio stress test?', label: 'Start stress test' })) return;

  const fields = { mode };
  if (mode !== 'stop') fields.duration_secs = String(duration);
  if (mode === 'audio') fields.work_ms = String(workMs);
  try {
    const response = await apiFetch('/api/system/audio-stress', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: new URLSearchParams(fields).toString(),
      signal: AbortSignal.timeout(10000),
    });
    if (!response.ok) throw new Error((await response.text()).trim() || `HTTP ${response.status}`);
    setStatus(mode === 'stop' ? 'Audio stress test stopped.' : 'Audio stress test started.', 'warn', 'audio-stress');
    await loadSystemInfo();
  } catch (error) {
    setStatus(`audio stress test failed: ${error.message}`, 'warn', 'audio-stress');
  }
}

document.getElementById('btn-audio-stress-thread').addEventListener('click', () => requestAudioStress('audio'));
document.getElementById('btn-audio-stress-cpu').addEventListener('click', () => requestAudioStress('cpu'));
document.getElementById('btn-audio-stress-stop').addEventListener('click', () => requestAudioStress('stop'));
async function loadBootFailure() {
  const bannerEl = document.getElementById('boot-failure-banner');
  try {
    const res = await apiFetch('/api/system/boot-failure', { cache: 'no-store' });
    if (res.status !== 200) {
      bannerEl.hidden = true;
      return;
    }
    const report = await res.text();
    const field = (name) => {
      const match = report.match(new RegExp(`^${name}=(.*)$`, 'm'));
      return match ? match[1] : 'unknown';
    };
    const reverted = field('reverted') === 'yes';
    const summary = reverted
      ? `${field('failed_version')} failed ${field('attempts')} boot attempt(s); restored ${field('restored_version')}.`
      : `${field('failed_version')} failed ${field('attempts')} boot attempt(s); no backup was available.`;
    document.getElementById('boot-failure-title').textContent = reverted
      ? 'Last update failed to boot and was reverted.'
      : 'Last update failed to boot.';
    document.getElementById('boot-failure-summary').textContent = summary;
    document.getElementById('boot-failure-report').textContent = report;
    bannerEl.hidden = false;
  } catch (err) {
    bannerEl.hidden = true;
  }
}

document.getElementById('btn-boot-failure-view').addEventListener('click', () => {
  const reportEl = document.getElementById('boot-failure-report');
  reportEl.hidden = !reportEl.hidden;
  document.getElementById('btn-boot-failure-view').textContent = reportEl.hidden ? 'View report' : 'Hide report';
});

document.getElementById('btn-boot-failure-dismiss').addEventListener('click', async () => {
  try {
    const res = await apiFetch('/api/system/boot-failure', { method: 'DELETE' });
    const txt = (await res.text()).trim();
    if (!res.ok) {
      setStatus(`dismiss failed: ${res.status} ${txt}`, 'warn');
      return;
    }
    document.getElementById('boot-failure-banner').hidden = true;
    setStatus('boot failure report dismissed', 'ok');
  } catch (err) {
    setStatus(`dismiss error: ${err}`, 'warn');
  }
});
