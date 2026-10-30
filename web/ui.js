(function (root) {
  'use strict';

  function formatMessage(value) {
    let text = String(value instanceof Error ? value.message : value || '').trim()
      .replace(/^(?:(?:Error|TypeError|SyntaxError):\s*)+/i, '');
    const context = text.match(/^([^:<>{}\n]*(?:failed|error|rejected))\s*:/i)?.[1];
    const fallback = context ? `${context[0].toUpperCase()}${context.slice(1)}.` : 'The request could not be completed.';
    if (/failed to fetch|fetch failed|networkerror|network request failed|^load failed$/i.test(text)) {
      return 'Cannot reach AudioX. Check your connection to the device, then try again.';
    }
    if (/unexpected token|unexpected end of json|is not valid json|json\.parse/i.test(text)) {
      return `${fallback} The device returned an unexpected response. Reload the page and try again.`;
    }
    // Error strings often have an operation and HTTP code before the response body.
    const jsonBody = text.match(/(?:^|:\s*|\b[45]\d\d\s+)([{[])/);
    const jsonStart = jsonBody ? jsonBody.index + jsonBody[0].length - 1 : -1;
    if (jsonStart >= 0) {
      try {
        const data = JSON.parse(text.slice(jsonStart));
        const detail = data.message || (typeof data.error === 'string' ? data.error : data.error?.message);
        text = typeof detail === 'string' ? `${context ? `${context}: ` : ''}${detail}` : fallback;
      } catch {
        if (/^[{[]/.test(text.slice(jsonStart))) text = fallback;
      }
    }
    if (/<[a-z!/][^>]*>/i.test(text)) {
      const code = String(value).match(/\b(4\d\d|5\d\d)\b/)?.[1];
      const hint = code === '404' ? 'This feature may require newer device firmware.'
        : code === '413' ? 'The selected file is too large.'
          : code === '503' ? 'The device is busy or restarting. Try again shortly.'
            : 'The device returned an unexpected response. Try again, or check System logs.';
      return `${fallback} ${hint}`;
    }
    text = text.split(/\n\s*(?:at\s|Traceback\b|File ")/)[0].replace(/\s+/g, ' ').trim();
    if (!text) return fallback;
    text = text[0].toUpperCase() + text.slice(1);
    return text.length > 300 ? `${text.slice(0, 297)}...` : text;
  }

  function isConnectionError(value) {
    return /failed to fetch|fetch failed|networkerror|network request failed|cannot reach audiox|^load failed$/i.test(String(value));
  }

  function resolveNotice(key) {
    for (const notice of Array.from(document.getElementById('notifications').children)) {
      if (notice.dataset.key === key) notice.remove();
    }
  }

  function notify(value, kind = '', key = '') {
    const host = document.getElementById('notifications');
    const text = formatMessage(value);
    const tone = kind === 'warn' ? 'warn' : kind === 'ok' ? 'ok' : 'info';
    if (!key && busy) key = taskKey;
    if (tone === 'warn' && isConnectionError(value)) key = 'connection';
    const existing = Array.from(host.children).find((item) => item.dataset.message === text && item.dataset.key === key);
    if (existing) return;
    if (key && tone !== 'info') resolveNotice(key);
    // Progress is a single changing notice, rather than a new popup per upload tick.
    host.querySelector('[data-tone="info"]')?.remove();
    if (tone === 'ok') host.querySelector('[data-tone="ok"]')?.remove();
    if (host.children.length >= 4) {
      (Array.from(host.children).find((item) => item.dataset.tone !== 'warn') || host.firstElementChild).remove();
    }
    const notice = document.createElement('section');
    notice.className = `notification ${tone}`;
    notice.dataset.message = text;
    notice.dataset.tone = tone;
    notice.dataset.key = key;
    notice.setAttribute('role', tone === 'warn' ? 'alert' : 'status');
    const content = document.createElement('div');
    const title = document.createElement('strong');
    title.textContent = tone === 'warn' ? 'Needs attention' : tone === 'ok' ? 'Done' : 'Working';
    const message = document.createElement('p');
    message.textContent = text;
    content.append(title, message);
    const dismiss = document.createElement('button');
    dismiss.className = 'notification-dismiss flat';
    dismiss.textContent = 'Close';
    dismiss.setAttribute('aria-label', 'Dismiss notification');
    dismiss.addEventListener('click', () => notice.remove());
    notice.append(content, dismiss);
    host.append(notice);
    if (tone !== 'warn') {
      let timer;
      const schedule = () => { timer = setTimeout(() => notice.remove(), tone === 'ok' ? 5000 : 8000); };
      notice.addEventListener('mouseenter', () => clearTimeout(timer));
      notice.addEventListener('mouseleave', schedule);
      notice.addEventListener('focusin', () => clearTimeout(timer));
      notice.addEventListener('focusout', schedule);
      schedule();
    }
  }

  let confirming = false;
  let busy = false;
  let taskKey = '';
  const unsavedChanges = new Set();
  function setUnsaved(key, dirty = true) {
    if (dirty) unsavedChanges.add(key);
    else unsavedChanges.delete(key);
  }
  function clearUnsaved() {
    unsavedChanges.clear();
  }
  function hasUnsavedChanges(key) {
    return key ? unsavedChanges.has(key) : unsavedChanges.size > 0;
  }
  function handleBeforeUnload(event) {
    if (!hasUnsavedChanges()) return;
    event.preventDefault();
    event.returnValue = '';
  }
  if (typeof root.addEventListener === 'function') root.addEventListener('beforeunload', handleBeforeUnload);
  if (typeof document !== 'undefined' && document.addEventListener) {
    const markEditorDirty = (event) => {
      const key = event.target?.dataset?.unsavedKey;
      if (key) setUnsaved(key);
    };
    document.addEventListener('input', markEditorDirty);
    document.addEventListener('change', markEditorDirty);
  }

  async function confirmAction(message, { title = 'Confirm action', label = 'Continue' } = {}) {
    if (confirming || busy) return false;
    confirming = true;
    const dialog = document.getElementById('confirm-dialog');
    const accept = document.getElementById('confirm-accept');
    const cancel = document.getElementById('confirm-cancel');
    const previousFocus = document.activeElement;
    document.getElementById('confirm-title').textContent = title;
    document.getElementById('confirm-message').textContent = message;
    accept.textContent = label;
    try {
      return await new Promise((resolve) => {
        const finish = (accepted) => {
          accept.removeEventListener('click', onAccept);
          cancel.removeEventListener('click', onCancel);
          dialog.removeEventListener('cancel', onCancel);
          dialog.removeEventListener('keydown', onKeyDown);
          dialog.close();
          resolve(accepted);
        };
        const onAccept = () => finish(true);
        const onCancel = (event) => {
          event.preventDefault();
          finish(false);
        };
        const onKeyDown = (event) => {
          // Keep graph keyboard shortcuts from handling keys inside the modal.
          event.stopPropagation();
          if (event.key === 'Escape') onCancel(event);
        };
        accept.addEventListener('click', onAccept);
        cancel.addEventListener('click', onCancel);
        dialog.addEventListener('cancel', onCancel);
        dialog.addEventListener('keydown', onKeyDown);
        dialog.showModal();
        cancel.focus();
      });
    } finally {
      confirming = false;
      previousFocus?.focus();
    }
  }

  async function probeDevice(fetcher = fetch, signal) {
    const timeout = AbortSignal.timeout(10000);
    let response;
    try {
      response = await fetcher('/api/system/info', {
        cache: 'no-store',
        signal: signal ? AbortSignal.any([signal, timeout]) : timeout,
      });
    } catch (error) {
      if (error.name === 'TimeoutError') {
        throw new Error('Cannot reach AudioX. The device is taking too long to respond.');
      }
      throw error;
    }
    if (!response.ok) throw new Error(`Cannot reach AudioX (${response.status}).`);
    const info = await response.json();
    if (!Number.isFinite(info.uptime_secs) || info.uptime_secs < 0 || typeof info.version !== 'string') {
      throw new Error('AudioX returned incomplete device information.');
    }
    return info;
  }

  async function waitForRestart(before, {
    signal, startedAt = performance.now(), fetcher = fetch,
    now = () => performance.now(), pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
    onProgress = () => {},
  } = {}) {
    while (!signal?.aborted) {
      const tick = now();
      try {
        const info = await probeDevice(fetcher, signal);
        // A successful HTTP response alone may still be from the old boot.
        const expectedUptime = before.uptime_secs + (now() - startedAt) / 1000;
        if (info.uptime_secs + 2 < expectedUptime) return info;
        onProgress('Waiting for AudioX to restart. The previous session is still online...');
      } catch (error) {
        if (signal?.aborted) break;
        console.debug('AudioX restart check:', error.message);
        onProgress('AudioX is offline or starting up. Reconnecting automatically...');
      }
      await pause(Math.max(0, 1000 - (now() - tick)));
    }
    throw new Error('Stopped waiting for AudioX. The device action may still be running. Reconnect or reload this page to check before trying again.');
  }

  function taskProgress(text) {
    if (!busy) return;
    const message = document.getElementById('loading-message');
    const next = formatMessage(text);
    if (message.textContent !== next) message.textContent = next;
  }

  async function runTask(title, action, { restart = false } = {}) {
    if (busy || confirming) throw new Error('Another device action is already running.');
    const dialog = document.getElementById('loading-dialog');
    const stop = document.getElementById('loading-stop');
    const previousFocus = document.activeElement;
    const controller = new AbortController();
    const stopWaiting = () => controller.abort();
    const preventCancel = (event) => event.preventDefault();
    const isolateKeys = (event) => {
      event.stopPropagation();
      if (event.key === 'Escape') event.preventDefault();
    };
    busy = true;
    taskKey = `task:${title}`;
    document.getElementById('loading-title').textContent = title;
    taskProgress('Preparing device action...');
    stop.hidden = true;
    dialog.addEventListener('cancel', preventCancel);
    dialog.addEventListener('keydown', isolateKeys);
    stop.addEventListener('click', stopWaiting);
    try {
      dialog.showModal();
      const before = restart ? await probeDevice() : null;
      const startedAt = performance.now();
      const result = await action();
      if (restart) {
        taskProgress('Waiting for AudioX to restart and reconnect...');
        stop.hidden = false;
        stop.focus();
        await waitForRestart(before, { signal: controller.signal, startedAt, onProgress: taskProgress });
        resolveNotice('connection');
        root.location.reload();
      }
      return result;
    } finally {
      busy = false;
      taskKey = '';
      dialog.close();
      dialog.removeEventListener('cancel', preventCancel);
      dialog.removeEventListener('keydown', isolateKeys);
      stop.removeEventListener('click', stopWaiting);
      previousFocus?.focus();
    }
  }

  function startConnectionMonitor(onReconnect = () => {}, onInfo = () => {}) {
    let stopped = false;
    let disconnected = false;
    let failures = 0;
    let timer;
    const check = async () => {
      let reconnected = false;
      if (!busy && document.visibilityState === 'visible') {
        try {
          const info = await probeDevice();
          if (!stopped) {
            const hadNotice = Array.from(document.getElementById('notifications').children)
              .some((notice) => notice.dataset.key === 'connection');
            resolveNotice('connection');
            reconnected = disconnected || hadNotice;
            disconnected = false;
            failures = 0;
            onInfo(info);
          }
        } catch (error) {
          disconnected = true;
          failures++;
          console.debug('AudioX connection check:', error.message);
          if (!stopped && !busy && failures >= 2) {
            notify('Cannot reach AudioX. Reconnecting automatically; check the device connection.', 'warn', 'connection');
          }
        }
      }
      if (reconnected) onReconnect();
      if (!stopped) timer = setTimeout(check, 3000);
    };
    timer = setTimeout(check, 3000);
    return () => { stopped = true; clearTimeout(timer); };
  }

  const exported = {
    formatMessage, notify, resolve: resolveNotice, confirm: confirmAction,
    setUnsaved, clearUnsaved, hasUnsavedChanges, handleBeforeUnload,
    probeDevice, waitForRestart, runTask, taskProgress, startConnectionMonitor,
    isBusy: () => busy,
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxUI = exported;
})(globalThis);
