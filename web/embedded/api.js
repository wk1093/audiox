// Single gateway for every request the UI makes to the AudioX device.
// The device build ships this file; the public demo build swaps it for
// demo/mock-api.js, which implements the same interface with simulated data.
(function (root) {
  'use strict';

  function request(input, init) {
    return root.fetch(input, init);
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

  const api = { demo: false, request, upload, download };
  root.AudioxApi = api;
  root.apiFetch = (input, init) => root.AudioxApi.request(input, init);
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
