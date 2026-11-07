(function (root) {
  'use strict';

  const endpoint = 'https://telemetry.audiox.wklab.net/v1/batches';
  const description = 'Help improve AudioX by sharing one summary every 10 seconds: firmware and Pi model, ' +
    'device counts by category, configured/enabled effect counts by type, soundboard/MIDI/scene usage counts, ' +
    'sample rate, block size, X-runs, observed audio activity, CPU usage and temperature. ' +
    'Idle intervals with no usage or configuration changes are skipped. No audio, names, hardware identifiers, ' +
    'scripts or logs are sent. A persistent random installation ID lets reports from this installation ' +
    'be grouped across restarts. It is not derived from hardware or network identifiers; reports are ' +
    'pseudonymous, not anonymous. Disabling telemetry deletes this ID; enabling it again creates a new one. ' +
    'Daily aggregate statistics will be PUBLIC at https://telemetry.audiox.wklab.net/. ' +
    'The public dashboard excludes installation IDs and raw reports, but with few participants even daily ' +
    'totals can reveal individual activity. There is no minimum participant count. ' +
    'Opting out cannot recall uploaded reports or copies of published statistics. ' +
    'Cloudflare sees your connection IP. Up to 8,640 points are stored on this device. ' +
    'When this WebUI is open and at least 100 points are pending, all queued points are sent to ' +
    'telemetry.audiox.wklab.net. Raw reports expire after 30 days. You can disable this and clear pending ' +
    'data on the System page. Telemetry is optional and off until you enable it.';

  function createRelay({ deviceRequest, request, onStatus = () => {}, onUnknown = () => {}, now = Date.now }) {
    let running = false, controller = null, retryAt = 0, failures = 0, cancelled = false, draining = false;
    async function device(path, init = {}) {
      const response = await deviceRequest(`/api/telemetry${path}`, {
        cache: 'no-store', ...init, signal: controller.signal,
      });
      if (!response.ok) throw new Error(`Telemetry device request failed (${response.status}).`);
      const result = await response.json();
      if (result.error) {
        onStatus(result);
        throw new Error(result.error);
      }
      return result;
    }
    return {
      cancel() { cancelled = true; controller?.abort(); },
      async tick() {
        if (running) return;
        running = true;
        cancelled = false;
        controller = new AbortController();
        let timeout = setTimeout(() => controller.abort(), 30000);
        try {
          const status = await device('');
          onStatus(status);
          if (status.consent === 'unknown') { onUnknown(); return; }
          if (status.consent !== 'enabled') { draining = false; return; }
          if (!status.pending) draining = false;
          if ((!draining && status.pending < 100) || now() < retryAt) return;
          draining = true;
          let remaining = status.pending;
          while (remaining > 0) {
            clearTimeout(timeout);
            timeout = setTimeout(() => controller.abort(), 30000);
            const batch = await device('/batch');
            if (!batch.batch || !Array.isArray(batch.points) || batch.points.length < 1 || batch.points.length > 1000)
              throw new Error('Invalid telemetry batch; queued data retained.');
            // Another tab may have disabled telemetry while this batch was being read.
            const current = await device('');
            if (current.consent !== 'enabled') { draining = false; onStatus(current); return; }
            const response = await request(endpoint, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(batch),
              credentials: 'omit',
              referrerPolicy: 'no-referrer',
              cache: 'no-store',
              signal: controller.signal,
            });
            if (!response.ok) throw new Error(`Telemetry upload failed (${response.status}); queued data retained.`);
            const receipt = await response.json();
            if (receipt.batch !== batch.batch) throw new Error('Invalid telemetry receipt; queued data retained.');
            controller.signal.throwIfAborted();
            const acknowledged = await device('/ack', {
              method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: batch.batch,
            });
            onStatus(acknowledged);
            if (!Number.isSafeInteger(acknowledged.pending) || acknowledged.pending < 0)
              throw new Error('Invalid telemetry queue status.');
            failures = 0;
            retryAt = 0;
            remaining = Math.min(remaining - batch.points.length, acknowledged.pending);
          }
          draining = false;
        } catch (error) {
          if (cancelled) return;
          failures = Math.min(failures + 1, 7);
          retryAt = now() + Math.min(3600000, 60000 * (2 ** (failures - 1)));
          onStatus({ error: `${controller.signal.aborted ? 'Telemetry request timed out; queued data retained.' : error.message} Retrying later.` });
        } finally {
          clearTimeout(timeout);
          running = false;
        }
      },
    };
  }

  function init() {
    const statusEl = document.getElementById('telemetry-status');
    const toggle = document.getElementById('telemetry-enabled');
    const preview = document.getElementById('telemetry-preview');
    const dialog = document.getElementById('telemetry-dialog');
    document.getElementById('telemetry-description').textContent = description;
    document.getElementById('telemetry-dialog-description').textContent = description;
    if (root.AudioxApi.demo) {
      toggle.disabled = true;
      document.getElementById('btn-telemetry-preview').disabled = true;
      statusEl.textContent = 'Telemetry is disabled in the demo; no collection or uploads.';
      return;
    }
    let prompted = false, saving = false;
    const relay = createRelay({
      deviceRequest: root.apiFetch,
      request: (...args) => root.fetch(...args),
      onStatus(status) {
        if (status.consent) toggle.checked = status.consent === 'enabled';
        if (status.error) { statusEl.textContent = status.error; return; }
        statusEl.textContent = `${status.consent}; ${status.pending}/${status.capacity} points pending. ` +
          'Uploads begin at 100 pending points while this WebUI is open.';
      },
      onUnknown() {
        if (prompted) return;
        // Avoid competing with maintenance or another modal on page startup.
        if (root.AudioxUI.isBusy() || document.querySelector('dialog[open]')) return;
        prompted = true;
        dialog.showModal();
        document.getElementById('btn-telemetry-decline').focus();
      },
    });
    async function save(enabled) {
      if (saving) return;
      saving = true;
      relay.cancel();
      toggle.disabled = true;
      const buttons = ['btn-telemetry-accept', 'btn-telemetry-decline'];
      for (const id of buttons) document.getElementById(id).disabled = true;
      let actualConsent;
      try {
        const response = await root.apiFetch('/api/telemetry', {
          method: 'POST', headers: { 'Content-Type': 'text/plain' },
          body: enabled ? 'enabled=1&consent_version=3' : 'enabled=0', signal: AbortSignal.timeout(10000),
        });
        const result = await response.json();
        actualConsent = result.consent;
        if (!response.ok || result.error) throw new Error(result.error || `Telemetry setting failed (${response.status}).`);
        if (dialog.open) dialog.close();
        preview.textContent = '';
        statusEl.textContent = enabled ? 'Telemetry enabled. Collection starts now.' : 'Telemetry disabled; pending data cleared.';
        toggle.checked = enabled;
      } catch (error) {
        statusEl.textContent = error.message;
        root.AudioxUI.notify(error, 'warn', 'telemetry');
        toggle.checked = actualConsent ? actualConsent === 'enabled' : !enabled;
      } finally {
        saving = false;
        toggle.disabled = false;
        for (const id of buttons) document.getElementById(id).disabled = false;
      }
    }
    toggle.addEventListener('change', () => save(toggle.checked));
    document.getElementById('btn-telemetry-accept').addEventListener('click', () => save(true));
    document.getElementById('btn-telemetry-decline').addEventListener('click', () => save(false));
    dialog.addEventListener('keydown', (event) => event.stopPropagation());
    document.getElementById('btn-telemetry-preview').addEventListener('click', async () => {
      try {
        const response = await root.apiFetch('/api/telemetry/batch', {
          cache: 'no-store', signal: AbortSignal.timeout(10000),
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `Preview failed (${response.status}).`);
        preview.textContent = result.batch ? JSON.stringify(result, null, 2) : 'No pending telemetry points.';
      } catch (error) { statusEl.textContent = error.message; }
    });
    const tick = () => { if (!saving) relay.tick(); };
    tick();
    setInterval(tick, 60000);
    root.addEventListener('pagehide', () => relay.cancel());
    root.addEventListener('pageshow', tick);
  }

  const api = { createRelay, init, description, endpoint };
  root.AudioxTelemetry = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
