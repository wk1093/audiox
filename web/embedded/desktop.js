(function (root) {
  'use strict';

  function create({ api, tab, host, os, capabilities, media, routing, routeForm,
    stream, sink, routeSubmit, routeStatus, onDisconnect }) {
    let routeAvailable = false;
    let session = 0;
    let busy = false;
    let generation = 0;
    const showStatus = (text) => { if (routeStatus) routeStatus.textContent = text; };

    async function route(event) {
      event.preventDefault();
      if (!routeAvailable || busy) return;
      const validId = (value) => /^(0|[1-9][0-9]*)$/.test(value) && Number(value) <= 4294967295;
      if (!validId(stream.value) || !validId(sink.value)) {
        showStatus('Enter valid stream and sink IDs (0 to 4294967295).');
        return;
      }
      const current = ++generation;
      busy = true;
      routeSubmit.disabled = true;
      showStatus('Requesting route...');
      try {
        async function request(url, options) {
          const response = await api.request(url, { ...options, cache: 'no-store', signal: AbortSignal.timeout(15000) });
          const data = await response.json();
          if (!response.ok) throw new Error(data.error || `Request failed (${response.status}).`);
          return data;
        }
        const body = new URLSearchParams({ stream: stream.value, sink: sink.value, session: String(session) }).toString();
        const accepted = await request('/api/desktop/route', { method: 'POST', body });
        if (!Number.isInteger(accepted.id) || accepted.id <= 0 || accepted.id > 4294967295)
          throw new Error('Invalid route response.');
        const deadline = Date.now() + 15000;
        while (generation === current) {
          const result = await request(`/api/desktop/route?id=${accepted.id}`, { method: 'GET' });
          if (generation !== current) return;
          if (result.id !== accepted.id) throw new Error('Invalid route result.');
          if (result.status === 'succeeded') {
            showStatus('Stream moved successfully.');
            return;
          }
          if (result.status !== 'pending') {
            const errors = {
              failed: 'The desktop could not move the stream. Check the IDs and companion logs.',
              disconnected: 'The desktop disconnected before confirming the route.',
              timed_out: 'The desktop did not confirm the route in time.',
            };
            throw new Error(errors[result.status] || 'Invalid route result.');
          }
          if (Date.now() >= deadline) throw new Error('Timed out waiting for the route result.');
          await new Promise((resolve) => root.setTimeout(resolve, 300));
        }
      } catch (error) {
        if (generation === current) showStatus(error.message || 'Route request failed.');
      } finally {
        if (generation === current) {
          busy = false;
          routeSubmit.disabled = false;
        }
      }
    }

    routeForm?.addEventListener('submit', route);
    function update(data) {
      const hello = data?.hello;
      const connected = data?.connected === true && hello &&
        typeof hello.hostname === 'string' && typeof hello.os === 'string' &&
        Array.isArray(hello.capabilities) && hello.capabilities.every((value) => typeof value === 'string');
      tab.hidden = !connected;
      const canRoute = connected && hello.os === 'linux' && hello.capabilities.includes('route_stream') &&
        Number.isInteger(data.session) && data.session > 0 && data.session <= 4294967295;
      if (routing) routing.hidden = !canRoute;
      if (routeAvailable && (!canRoute || session !== data.session)) {
        generation++;
        busy = false;
        if (routeSubmit) routeSubmit.disabled = false;
        if (stream) stream.value = '';
        if (sink) sink.value = '';
        showStatus('');
      }
      routeAvailable = canRoute;
      session = canRoute ? data.session : 0;
      if (!connected) {
        host.textContent = '';
        os.textContent = '';
        capabilities.textContent = '';
        media.textContent = '';
        onDisconnect();
        return;
      }
      host.textContent = hello.hostname;
      os.textContent = hello.os;
      capabilities.textContent = hello.capabilities.length ? hello.capabilities.join(', ') : 'None detected';
      const statuses = { playing: 'Playing', paused: 'Paused', stopped: 'Stopped',
        none: 'No players', error: 'Unable to query playback status' };
      media.textContent = Object.prototype.hasOwnProperty.call(statuses, hello.media) ? statuses[hello.media] : 'Unavailable';
    }
    const unsubscribe = api?.subscribe?.('desktop.status', update);
    const unobserve = api?.onConnectionChange?.((connected) => { if (!connected) update(null); });
    return () => {
      generation++;
      routeForm?.removeEventListener('submit', route);
      unsubscribe?.();
      unobserve?.();
    };
  }

  root.AudioxDesktop = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = root.AudioxDesktop;
})(globalThis);
