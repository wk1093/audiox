// ---- Spectrum analyzer popup ----

const spectrumView = {
  timer: null,
  frame: null,
  busy: false,
  lastFrame: 0,
  minHz: 20,
  maxHz: 20000,
  targets: [],
  bands: [],
  peaks: [],
  peakAge: [],
};
const SPECTRUM_TOP_DB = 0;
const SPECTRUM_BOTTOM_DB = -90;
const SPECTRUM_FALL_DB_PER_S = 70;
const SPECTRUM_PEAK_HOLD_S = 1;
const SPECTRUM_PEAK_FALL_DB_PER_S = 25;
const SPECTRUM_NODE_KEY = 'audiox.spectrumNode';
const spectrumDialog = document.getElementById('spectrum-dialog');
const spectrumSelect = document.getElementById('spectrum-node');
const spectrumCanvas = document.getElementById('spectrum-canvas');
const spectrumStatus = document.getElementById('spectrum-status');
const spectrumPeaks = document.getElementById('spectrum-peaks');
const spectrumReducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');

function fillSpectrumNodes() {
  const volumes = Array.isArray(state.volumes) ? state.volumes : [];
  let saved = spectrumSelect.value;
  try { saved ||= localStorage.getItem(SPECTRUM_NODE_KEY) || ''; } catch { /* storage unavailable */ }
  spectrumSelect.replaceChildren(...volumes.map((vol) => new Option(vol.name || vol.id, vol.id)));
  const fallback = volumes.find((vol) => /^(?:alsa|bt|usb)_.*_out$/.test(vol.id))
    || volumes.find((vol) => vol.id.endsWith('_out')) || volumes[0];
  spectrumSelect.value = volumes.some((vol) => vol.id === saved) ? saved : fallback?.id || '';
  spectrumSelect.disabled = !volumes.length;
}

function resetSpectrumBands() {
  spectrumView.targets = [];
  spectrumView.bands = [];
  spectrumView.peaks = [];
  spectrumView.peakAge = [];
}

async function pollSpectrum() {
  const id = spectrumSelect.value;
  if (spectrumView.busy || !id || document.hidden || !spectrumDialog.open) return;
  spectrumView.busy = true;
  try {
    const res = await apiFetch(`/api/audio/spectrum?id=${encodeURIComponent(id)}`,
      { cache: 'no-store', signal: AbortSignal.timeout(2000) });
    if (!res.ok) throw new Error(`${res.status} ${(await res.text()).trim()}`);
    const data = await res.json();
    if (id !== spectrumSelect.value || !spectrumDialog.open) return;
    spectrumView.minHz = Number(data.min_hz) || 20;
    spectrumView.maxHz = Number(data.max_hz) || 20000;
    spectrumView.targets = Array.isArray(data.bands)
      ? data.bands.map((db) => (Number.isFinite(db) ? db : SPECTRUM_BOTTOM_DB)) : [];
    spectrumStatus.textContent = data.active ? '' : 'Waiting for audio from this channel…';
  } catch (err) {
    spectrumStatus.textContent = `Spectrum unavailable: ${err.message || err}`;
  } finally {
    spectrumView.busy = false;
  }
}

function spectrumX(hz, left, width) {
  const span = Math.log(spectrumView.maxHz / spectrumView.minHz);
  return left + (Math.log(hz / spectrumView.minHz) / span) * width;
}

function spectrumY(db, top, height) {
  const clamped = Math.max(SPECTRUM_BOTTOM_DB, Math.min(SPECTRUM_TOP_DB, db));
  return top + ((SPECTRUM_TOP_DB - clamped) / (SPECTRUM_TOP_DB - SPECTRUM_BOTTOM_DB)) * height;
}

function drawSpectrum() {
  const ratio = window.devicePixelRatio || 1;
  const cssWidth = spectrumCanvas.clientWidth;
  const cssHeight = spectrumCanvas.clientHeight;
  if (!cssWidth || !cssHeight) return;
  if (spectrumCanvas.width !== Math.round(cssWidth * ratio) || spectrumCanvas.height !== Math.round(cssHeight * ratio)) {
    spectrumCanvas.width = Math.round(cssWidth * ratio);
    spectrumCanvas.height = Math.round(cssHeight * ratio);
  }
  const ctx = spectrumCanvas.getContext('2d');
  if (!ctx) return;
  const styles = getComputedStyle(spectrumDialog);
  const accent = styles.getPropertyValue('--accent').trim() || '#4fd1c5';
  const muted = styles.getPropertyValue('--muted').trim() || '#8aa0b0';
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, cssWidth, cssHeight);
  const left = 40, right = 10, top = 10, bottom = 24;
  const width = cssWidth - left - right;
  const height = cssHeight - top - bottom;
  if (width <= 0 || height <= 0) return;

  ctx.font = '11px system-ui, sans-serif';
  ctx.lineWidth = 1;
  ctx.strokeStyle = 'rgba(143, 177, 200, 0.14)';
  ctx.fillStyle = muted;
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let db = SPECTRUM_TOP_DB; db >= SPECTRUM_BOTTOM_DB; db -= 15) {
    const y = Math.round(spectrumY(db, top, height)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(left + width, y);
    ctx.stroke();
    ctx.fillText(`${db}`, left - 6, y);
  }
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (const hz of [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
    if (hz < spectrumView.minHz || hz > spectrumView.maxHz) continue;
    const x = Math.round(spectrumX(hz, left, width)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(x, top);
    ctx.lineTo(x, top + height);
    ctx.stroke();
    ctx.fillText(hz >= 1000 ? `${hz / 1000}k` : `${hz}`, Math.min(x, left + width - 10), top + height + 6);
  }

  const bands = spectrumView.bands;
  if (!bands.length) return;
  const slot = width / bands.length;
  const gap = slot > 4 ? 1 : 0;
  const gradient = ctx.createLinearGradient(0, top, 0, top + height);
  gradient.addColorStop(0, '#ff6b6b');
  gradient.addColorStop(0.18, '#ffd166');
  gradient.addColorStop(0.45, accent);
  gradient.addColorStop(1, 'rgba(35, 91, 112, 0.6)');
  ctx.fillStyle = gradient;
  bands.forEach((db, band) => {
    const y = spectrumY(db, top, height);
    ctx.fillRect(left + band * slot + gap / 2, y, Math.max(1, slot - gap), top + height - y);
  });
  if (spectrumPeaks.checked) {
    ctx.fillStyle = 'rgba(255, 255, 255, 0.85)';
    spectrumView.peaks.forEach((db, band) => {
      if (db <= SPECTRUM_BOTTOM_DB) return;
      ctx.fillRect(left + band * slot + gap / 2, spectrumY(db, top, height) - 1, Math.max(1, slot - gap), 2);
    });
  }
}

function animateSpectrum(now) {
  spectrumView.frame = null;
  if (!spectrumDialog.open) return;
  const dt = Math.min(0.25, Math.max(0, (now - (spectrumView.lastFrame || now)) / 1000));
  spectrumView.lastFrame = now;
  const instant = spectrumReducedMotion.matches;
  spectrumView.targets.forEach((target, band) => {
    const current = spectrumView.bands[band] ?? SPECTRUM_BOTTOM_DB;
    const next = instant || target >= current ? target : Math.max(target, current - SPECTRUM_FALL_DB_PER_S * dt);
    spectrumView.bands[band] = next;
    const peak = spectrumView.peaks[band] ?? SPECTRUM_BOTTOM_DB;
    if (next >= peak) {
      spectrumView.peaks[band] = next;
      spectrumView.peakAge[band] = 0;
    } else {
      spectrumView.peakAge[band] = (spectrumView.peakAge[band] || 0) + dt;
      if (spectrumView.peakAge[band] > SPECTRUM_PEAK_HOLD_S)
        spectrumView.peaks[band] = Math.max(next, peak - SPECTRUM_PEAK_FALL_DB_PER_S * dt);
    }
  });
  spectrumView.bands.length = spectrumView.targets.length;
  drawSpectrum();
  spectrumView.frame = requestAnimationFrame(animateSpectrum);
}

function stopSpectrum() {
  if (spectrumView.timer !== null) clearInterval(spectrumView.timer);
  if (spectrumView.frame !== null) cancelAnimationFrame(spectrumView.frame);
  spectrumView.timer = null;
  spectrumView.frame = null;
  spectrumView.lastFrame = 0;
}

async function openSpectrumDialog() {
  if (!Array.isArray(state.volumes) || !state.volumes.length) await loadVolumes();
  fillSpectrumNodes();
  resetSpectrumBands();
  spectrumStatus.textContent = spectrumSelect.value ? 'Connecting…' : 'No mixer channels are available.';
  spectrumDialog.showModal();
  stopSpectrum();
  pollSpectrum();
  spectrumView.timer = setInterval(pollSpectrum, 50);
  spectrumView.frame = requestAnimationFrame(animateSpectrum);
}

document.getElementById('btn-vol-spectrum').addEventListener('click', openSpectrumDialog);
document.getElementById('btn-spectrum-close').addEventListener('click', () => spectrumDialog.close());
spectrumDialog.addEventListener('close', stopSpectrum);
spectrumSelect.addEventListener('change', () => {
  try { localStorage.setItem(SPECTRUM_NODE_KEY, spectrumSelect.value); } catch { /* storage unavailable */ }
  resetSpectrumBands();
  spectrumStatus.textContent = 'Connecting…';
  pollSpectrum();
});
spectrumPeaks.addEventListener('change', drawSpectrum);
