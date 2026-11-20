(function (root) {
  'use strict';

  function create({ api, tab, host, os, capabilities, media, onDisconnect }) {
    function update(data) {
      const hello = data?.hello;
      const connected = data?.connected === true && hello &&
        typeof hello.hostname === 'string' && typeof hello.os === 'string' &&
        Array.isArray(hello.capabilities) && hello.capabilities.every((value) => typeof value === 'string');
      tab.hidden = !connected;
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
    return () => { unsubscribe?.(); unobserve?.(); };
  }

  root.AudioxDesktop = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = root.AudioxDesktop;
})(globalThis);
