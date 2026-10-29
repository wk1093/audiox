(function (root) {
  'use strict';

  function create({ view, summary, source, level, text, limit, follow, refresh, clear,
    notify, resolve, fetcher = (...args) => fetch(...args) }) {
    let following = true;
    let selecting = false;
    let pending = null;
    let inFlight = null;
    let generation = 0;
    let debounce;
    let lastSummary = 'No logs loaded yet.';

    const hasSelection = () => {
      const selection = root.getSelection();
      return selection && !selection.isCollapsed &&
        (view.contains(selection.anchorNode) || view.contains(selection.focusNode) ||
          selection.containsNode(view, true));
    };
    const updateSummary = () => {
      summary.textContent = `${lastSummary}${selecting || hasSelection()
        ? ' Updates paused while selecting text.' : !following ? ' Paused; resume to follow new logs.' : ''}`;
    };
    const setFollowing = (value) => {
      following = value;
      follow.textContent = value ? 'Pause live logs' : 'Resume live logs';
      follow.setAttribute('aria-pressed', String(value));
      updateSummary();
    };
    const apply = () => {
      if (!pending || selecting || hasSelection() || !following) { updateSummary(); return; }
      const { data } = pending;
      pending = null;
      const existing = new Map(Array.from(view.children, (node) => [node.dataset.logId, node]));
      let cursor = view.firstChild;
      for (const row of data.rows) {
        let node = existing.get(row.id);
        if (!node) {
          node = document.createElement('div');
          node.dataset.logId = row.id;
          node.className = `log-line log-level-${row.level}`;
          const label = document.createElement('span');
          label.className = 'log-src';
          label.textContent = `[${row.source}] `;
          node.append(label, document.createTextNode(row.line + (row.truncated ? ' [long line clipped]' : '')));
        }
        existing.delete(row.id);
        if (node !== cursor) view.insertBefore(node, cursor);
        cursor = node.nextSibling;
      }
      for (const node of existing.values()) node.remove();
      lastSummary = `Showing ${data.rows.length} recent matching lines${data.has_more ? ' (older matches omitted)' : ''}; refreshed ${new Date().toLocaleTimeString()}.`;
      if (data.scan_limited) lastSummary += ' Search reached the bounded tail limit.';
      if (data.missing_sources.length) lastSummary += ` No ${data.missing_sources.join('/')} log file yet.`;
      updateSummary();
      view.scrollTop = view.scrollHeight;
    };
    const query = () => {
      const value = Number(limit.value);
      if (!Number.isInteger(value) || value < 1 || value > 5000) throw new Error('Choose a log limit between 1 and 5000.');
      if (new TextEncoder().encode(text.value).length > 160) throw new Error('Keep the log text filter within 160 bytes.');
      return new URLSearchParams({ source: source.value, level: level.value, text: text.value, limit: String(value) }).toString();
    };
    const load = async ({ manual = false } = {}) => {
      if (!manual && (!following || selecting || hasSelection() || inFlight)) { updateSummary(); return; }
      let params;
      try { params = query(); }
      catch (error) { notify(error, 'warn', 'logs'); return; }
      const current = ++generation;
      inFlight?.abort();
      const controller = new AbortController();
      inFlight = controller;
      try {
        const response = await fetcher(`/api/system/logs?${params}`, {
          cache: 'no-store', signal: AbortSignal.any([controller.signal, AbortSignal.timeout(10000)]),
        });
        if (!response.ok) {
          if (response.status === 404) throw new Error('Update the device firmware to use bounded log queries. Full log files will not be downloaded automatically.');
          throw new Error(`Could not load logs: ${response.status} ${await response.text()}`);
        }
        const data = await response.json();
        if (!data || !Array.isArray(data.rows) || data.rows.length > 5000 ||
            !Array.isArray(data.missing_sources) || data.rows.some((row) =>
              typeof row.id !== 'string' || typeof row.line !== 'string' ||
              !['stdout', 'stderr'].includes(row.source) || !['debug', 'info', 'warn', 'error'].includes(row.level)) ||
            new Set(data.rows.map((row) => row.id)).size !== data.rows.length) {
          throw new Error('The device returned invalid log data. Refresh or update its firmware.');
        }
        if (current !== generation) return;
        pending = { data };
        apply();
        resolve('logs');
      } catch (error) {
        if (!controller.signal.aborted && current === generation) notify(error, 'warn', 'logs');
      } finally {
        if (current === generation) inFlight = null;
      }
    };
    const changeFilters = () => {
      ++generation;
      inFlight?.abort();
      inFlight = null;
      pending = null;
      clearTimeout(debounce);
      setFollowing(true);
      debounce = setTimeout(() => load({ manual: true }), 250);
    };
    for (const input of [source, level, limit]) input.addEventListener('change', changeFilters);
    text.addEventListener('input', changeFilters);
    clear.addEventListener('click', () => {
      source.value = 'both'; level.value = 'all'; text.value = ''; limit.value = '600';
      changeFilters();
    });
    refresh.addEventListener('click', () => {
      setFollowing(true);
      load({ manual: true });
    });
    follow.addEventListener('click', () => {
      setFollowing(!following);
      if (following) { apply(); load({ manual: true }); }
    });
    view.addEventListener('scroll', () => {
      if (view.scrollHeight - view.clientHeight - view.scrollTop > 8) setFollowing(false);
    }, { passive: true });
    view.addEventListener('wheel', () => setFollowing(false), { passive: true });
    view.addEventListener('keydown', (event) => {
      if (['ArrowUp', 'PageUp', 'Home'].includes(event.key)) setFollowing(false);
    });
    view.addEventListener('pointerdown', () => { selecting = true; });
    root.addEventListener('pointerup', () => { selecting = false; apply(); });
    root.addEventListener('pointercancel', () => { selecting = false; apply(); });
    root.addEventListener('blur', () => { selecting = false; updateSummary(); });
    document.addEventListener('selectionchange', () => { apply(); updateSummary(); });
    setFollowing(true);
    return { load };
  }

  function createHealthNotifier(notify) {
    let lastWarning = '';
    return (info) => {
      const health = info?.audio_health;
      if (health?.warning_active !== true || typeof health.warning_id !== 'string' ||
          health.warning_id === '0' || health.warning_id === lastWarning) return;
      lastWarning = health.warning_id;
      try {
        if (root.sessionStorage?.getItem('audiox-xrun-warning') === lastWarning) return;
        root.sessionStorage?.setItem('audiox-xrun-warning', lastWarning);
      } catch (error) {
        console.warn('AudioX could not remember the X-run warning across page reloads:', error.message);
      }
      notify('Repeated audio dropouts (X-runs) were detected. The system may be overloaded. Try reducing effects or active routes and check your audio devices.', 'warn', 'audio-xruns');
    };
  }

  const exported = { create, createHealthNotifier };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxLogs = exported;
})(globalThis);
