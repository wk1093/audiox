function parseConfigText(text) {
  const cfg = {
    usb_playback_channels: 2,
    usb_capture_channels: 2,
    usb_sample_rate: 48000,
    usb_sample_size: 2,
    soundboard_mode: 'play',
    framebuffer_view: 'meters',
    midi_passthrough: 'off',
    keyboard_macros: 0,
  };

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    if (key === 'midi_passthrough') {
      const mode = line.slice(eq + 1).trim();
      cfg.midi_passthrough = ['off', 'unmapped', 'all'].includes(mode) ? mode : 'off';
      continue;
    }
    if (key === 'soundboard_mode') {
      const mode = line.slice(eq + 1).trim().toLowerCase();
      cfg.soundboard_mode = (mode === 'hold') ? 'hold' : 'play';
      continue;
    }
    if (key === 'framebuffer_view') {
      cfg.framebuffer_view = line.slice(eq + 1).trim() === 'soundboard' ? 'soundboard' : 'meters';
      continue;
    }
    const value = Number(line.slice(eq + 1).trim());
    if (Number.isNaN(value)) continue;
    if (key in cfg) cfg[key] = value;
  }
  return cfg;
}

let configuredUsbSampleRate = 48000;

function formatConfigText(cfg) {
  return [
    `usb_playback_channels=${cfg.usb_playback_channels}`,
    `usb_capture_channels=${cfg.usb_capture_channels}`,
    `usb_sample_rate=${cfg.usb_sample_rate}`,
    `usb_sample_size=${cfg.usb_sample_size}`,
    `soundboard_mode=${cfg.soundboard_mode || 'play'}`,
    `framebuffer_view=${cfg.framebuffer_view || 'meters'}`,
    `midi_passthrough=${cfg.midi_passthrough || 'off'}`,
    `keyboard_macros=${cfg.keyboard_macros ? 1 : 0}`,
    ''
  ].join('\n');
}

function setFormFromConfig(cfg) {
  if (!cfg) return;
  document.getElementById('usb_playback_channels').value = String(cfg.usb_playback_channels ?? 2);
  document.getElementById('usb_capture_channels').value = String(cfg.usb_capture_channels ?? 2);
  configuredUsbSampleRate = Number(cfg.usb_sample_rate) || 48000;
  document.getElementById('usb_sample_size').value = String(cfg.usb_sample_size ?? 2);
  document.getElementById('midi_passthrough').value = cfg.midi_passthrough || 'off';
  document.getElementById('keyboard_macros').value = String(cfg.keyboard_macros ? 1 : 0);
}

function getConfigFromForm() {
  return {
    usb_playback_channels: Number(document.getElementById('usb_playback_channels').value) || 2,
    usb_capture_channels: Number(document.getElementById('usb_capture_channels').value) || 2,
    usb_sample_rate: configuredUsbSampleRate,
    usb_sample_size: Number(document.getElementById('usb_sample_size').value) || 2,
    soundboard_mode: 'play',
    framebuffer_view: state.framebufferView,
    midi_passthrough: document.getElementById('midi_passthrough').value,
    keyboard_macros: Number(document.getElementById('keyboard_macros').value),
  };
}

async function loadLegacyConfigStatus() {
  const notice = document.getElementById('legacy-config-notice');
  if (!notice) return;
  try {
    const response = await apiFetch('/api/config/legacy');
    if (!response.ok) throw new Error(`${response.status} ${(await response.text()).trim()}`);
    const data = await response.json();
    notice.hidden = data.legacy_entries === 0;
  } catch (error) {
    setStatus(`legacy configuration check failed: ${error}`, 'warn');
  }
}

async function deleteLegacyConfig() {
  const accepted = await AudioxUI.confirm(
    'Delete the retained legacy configuration and old script data? The active configuration is unchanged. Back up first. Older firmware will no longer have its original settings, and rollback may require a compatible backup.', {
      label: 'Delete legacy data', danger: true,
    });
  if (!accepted) return;
  const button = document.getElementById('btn-delete-legacy-config');
  button.disabled = true;
  try {
    const response = await apiFetch('/api/config/legacy', { method: 'DELETE' });
    if (!response.ok) throw new Error(`${response.status} ${(await response.text()).trim()}`);
    await loadLegacyConfigStatus();
    setStatus('legacy configuration deleted; active settings preserved', 'ok');
  } catch (error) {
    setStatus(`legacy configuration cleanup failed: ${error}`, 'warn');
  } finally {
    button.disabled = false;
  }
}

async function loadConfig() {
  setStatus('loading config...');
  try {
    let res = await apiFetch(configPath, { method: 'GET' });
    let txt = await res.text();
    if (!res.ok) {
      res = await apiFetch(realConfigPath, { method: 'GET' });
      txt = await res.text();
      if (!res.ok) {
        setStatus(`config load failed: ${res.status} ${txt.trim()}`, 'warn');
        return null;
      }
    }

    const cfg = parseConfigText(txt);
    setFormFromConfig(cfg);
    AudioxUI.setUnsaved('usb-config', false);
    setStatus('config loaded', 'ok');
    return cfg;
  } catch (err) {
    setStatus(`config load error: ${err}`, 'warn');
    return null;
  }
}

function renderFramebufferView() {
  const button = document.getElementById('btn-fb-view');
  const active = state.framebufferView === 'soundboard';
  button.textContent = active ? 'Show Meters on Display' : 'Show Soundboard on Display';
  button.setAttribute('aria-pressed', String(active));
}

async function loadFramebufferView() {
  try {
    const res = await apiFetch('/api/framebuffer/view');
    if (!res.ok) throw new Error(`${res.status}`);
    const data = await res.json();
    state.framebufferView = data.view === 'soundboard' ? 'soundboard' : 'meters';
    renderFramebufferView();
  } catch (err) {
    setStatus(`display mode load failed: ${err}`, 'warn');
  }
}

async function toggleFramebufferView() {
  const view = state.framebufferView === 'soundboard' ? 'meters' : 'soundboard';
  const button = document.getElementById('btn-fb-view');
  button.disabled = true;
  try {
    const res = await apiFetch('/api/framebuffer/view', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `view=${view}`,
    });
    if (!res.ok) throw new Error(`${res.status} ${(await res.text()).trim()}`);
    state.framebufferView = view;
    renderFramebufferView();
    setStatus(`display: ${view}`, 'ok');
  } catch (err) {
    setStatus(`display mode save failed: ${err}`, 'warn');
  } finally {
    button.disabled = false;
  }
}

async function saveConfig() {
  const cfg = getConfigFromForm();
  const payload = formatConfigText(cfg);
  setStatus('saving config...');

  try {
    const res = await apiFetch(configPath, {
      method: 'PUT',
      headers: { 'Content-Type': 'text/plain' },
      body: payload,
    });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`config save failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    setStatus('config saved', 'ok');
    AudioxUI.setUnsaved('usb-config', false);
    return true;
  } catch (err) {
    setStatus(`config save error: ${err}`, 'warn');
    return false;
  }
}

async function reloadConfig() {
  setStatus('reloading config...');
  try {
    const res = await apiFetch('/api/config/reload', { method: 'POST' });
    const txt = await res.text();
    if (!res.ok) {
      setStatus(`reload failed: ${res.status} ${txt.trim()}`, 'warn');
      return false;
    }

    setStatus('config reloaded', 'ok');
    return true;
  } catch (err) {
    setStatus(`reload error: ${err}`, 'warn');
    return false;
  }
}

