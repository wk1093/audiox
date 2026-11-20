// Single gateway for every request the UI makes to the AudioX device.
// The device bundle includes this gateway; the public demo build swaps it for
// demo/mock-api.js, which implements the same interface with simulated data.
(function (root) {
  'use strict';

  function request(input, init) {
    return root.fetch(input, init);
  }

  const socketListeners = new Map();
  let socket = null;
  let reconnectTimer = null;
  let reconnectDelay = 500;

  function activeTopics() {
    return [...socketListeners].filter(([, listeners]) => listeners.size).map(([topic]) => topic);
  }

  function isConnected() {
    return socket?.readyState === root.WebSocket?.OPEN;
  }

  function sendSubscription(type, topics) {
    if (isConnected() && topics.length) {
      socket.send(JSON.stringify({ type, topics }));
    }
  }

  function connectSocket() {
    if (socket || !activeTopics().length || typeof root.WebSocket !== 'function') return;
    const scheme = root.location.protocol === 'https:' ? 'wss:' : 'ws:';
    let current;
    try {
      current = new root.WebSocket(`${scheme}//${root.location.host}/ws`, 'audiox.v1');
    } catch (_) {
      return;
    }
    socket = current;
    current.addEventListener('open', () => {
      if (socket !== current) return;
      reconnectDelay = 500;
      sendSubscription('subscribe', activeTopics());
    });
    current.addEventListener('message', (event) => {
      if (socket !== current || typeof event.data !== 'string') return;
      let message;
      try { message = JSON.parse(event.data); } catch (_) { return; }
      if (message?.type !== 'event' || typeof message.topic !== 'string') return;
      const listeners = socketListeners.get(message.topic);
      if (!listeners) return;
      for (const listener of [...listeners]) {
        try { listener(message.data); } catch (error) { console.error('AudioX event listener failed', error); }
      }
    });
    current.addEventListener('close', () => {
      if (socket !== current) return;
      socket = null;
      if (!activeTopics().length) return;
      reconnectTimer = root.setTimeout(() => {
        reconnectTimer = null;
        connectSocket();
      }, reconnectDelay);
      reconnectDelay = Math.min(reconnectDelay * 2, 10000);
    });
    current.addEventListener('error', () => current.close());
  }

  function subscribe(topics, listener) {
    if (typeof root.WebSocket !== 'function' || typeof listener !== 'function') return null;
    const requested = [...new Set(Array.isArray(topics) ? topics : [topics])];
    if (!requested.length || requested.some((topic) => typeof topic !== 'string' || !topic)) return null;
    const added = [];
    for (const topic of requested) {
      let listeners = socketListeners.get(topic);
      if (!listeners) {
        listeners = new Set();
        socketListeners.set(topic, listeners);
        added.push(topic);
      }
      listeners.add(listener);
    }
    if (socket?.readyState === root.WebSocket.OPEN) sendSubscription('subscribe', added);
    connectSocket();

    let active = true;
    return () => {
      if (!active) return;
      active = false;
      const removed = [];
      for (const topic of requested) {
        const listeners = socketListeners.get(topic);
        if (!listeners) continue;
        listeners.delete(listener);
        if (!listeners.size) {
          socketListeners.delete(topic);
          removed.push(topic);
        }
      }
      sendSubscription('unsubscribe', removed);
      if (!activeTopics().length) {
        if (reconnectTimer !== null) root.clearTimeout(reconnectTimer);
        reconnectTimer = null;
        const previous = socket;
        socket = null;
        previous?.close();
      }
    };
  }

  // Streams a large body with upload progress. Resolves with the final HTTP
  // status and response text; rejects with error.reason 'error' or 'abort'.
  function upload(url, body, { method = 'PUT', contentType = 'application/octet-stream', onProgress, onSent } = {}) {
    return new Promise((resolve, reject) => {
      const xhr = new root.XMLHttpRequest();
      xhr.open(method, url);
      xhr.setRequestHeader('Content-Type', contentType);
      xhr.upload.addEventListener('progress', (event) => {
        if (event.lengthComputable && onProgress) onProgress(event.loaded, event.total);
      });
      xhr.upload.addEventListener('load', () => onSent?.());
      xhr.addEventListener('load', () => resolve({ status: xhr.status, text: xhr.responseText || '' }));
      xhr.addEventListener('error', () => reject(Object.assign(new Error('Upload failed'), { reason: 'error' })));
      xhr.addEventListener('abort', () => reject(Object.assign(new Error('Upload aborted'), { reason: 'abort' })));
      xhr.send(body);
    });
  }

  // Native downloads stream to disk instead of buffering in JavaScript.
  function download(url) {
    root.open(url, '_blank', 'noopener,noreferrer');
  }

  const api = { demo: false, request, upload, download, subscribe, isConnected };
  root.AudioxApi = api;
  root.apiFetch = (input, init) => root.AudioxApi.request(input, init);
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
