(() => {
  const MAX_PLUGIN_BYTES = 12 * 1024 * 1024;
  const BUILTIN_TYPES = new Set(['gain', 'distortion', 'pitch', 'reverb', 'gate', 'cut']);

  function normalizePluginId(value) {
    const id = String(value ?? '').trim();
    return /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/.test(id) && !BUILTIN_TYPES.has(id) ? id : '';
  }

  function finite(value, fallback) {
    const n = Number(value);
    return Number.isFinite(n) ? n : fallback;
  }

  function normalizeParams(params) {
    if (!Array.isArray(params)) return [];
    const out = [];
    for (const raw of params) {
      const name = typeof raw?.name === 'string' ? raw.name.trim() : '';
      if (!name) continue;
      out.push({
        name,
        label: typeof raw.label === 'string' && raw.label.trim() ? raw.label : name,
        min: finite(raw.min, 0),
        max: finite(raw.max, 1),
        default: finite(raw.default, finite(raw.min, 0)),
      });
    }
    return out;
  }

  function normalizeCatalog(data) {
    if (!data || !Array.isArray(data.plugins)) {
      throw new Error('Plugin catalog is not available. Refresh the page and try again.');
    }
    const plugins = [];
    for (const raw of data.plugins) {
      const id = normalizePluginId(raw?.id);
      if (!id) continue;
      plugins.push({
        id,
        name: typeof raw.name === 'string' && raw.name.trim() ? raw.name : id,
        version: typeof raw.version === 'string' ? raw.version : String(raw.version ?? ''),
        params: normalizeParams(raw.params),
        ui: raw.ui === true,
      });
    }
    return plugins.sort((a, b) => a.id.localeCompare(b.id));
  }

  function assetUrl(id) {
    const safe = normalizePluginId(id);
    return safe ? `/api/plugins/${encodeURIComponent(safe)}/assets/index.html` : '';
  }

  async function responseError(action, res) {
    const text = (await res.text().catch(() => '')).trim();
    return new Error(`${action} failed: ${res.status}${text ? ` ${text}` : ''}`);
  }

  function formatRange(param) {
    return `${param.label} (${param.min} to ${param.max}, default ${param.default})`;
  }

  class PluginManager {
    constructor({ setStatus, onCatalog, onChanged } = {}) {
      this.setStatus = typeof setStatus === 'function' ? setStatus : () => {};
      this.onCatalog = typeof onCatalog === 'function' ? onCatalog : () => {};
      this.onChanged = typeof onChanged === 'function' ? onChanged : async () => {};
      this.plugins = [];
      this.signature = '';
      this.busy = false;
      this.listEl = null;
      this.statusEl = null;
      this.fileInput = null;
      this.installButton = null;
    }

    mount({ listEl, statusEl, fileInput, installButton } = {}) {
      this.listEl = listEl || null;
      this.statusEl = statusEl || null;
      this.fileInput = fileInput || null;
      this.installButton = installButton || null;
      this.installButton?.addEventListener('click', () => this.installSelected());
      this.render();
    }

    setPanelStatus(text, kind = '') {
      if (this.statusEl) {
        this.statusEl.textContent = String(text);
        this.statusEl.className = `small ${kind}`.trim();
      }
      if (kind === 'warn') this.setStatus(text, kind);
    }

    pluginIds() {
      return this.plugins.map(plugin => plugin.id);
    }

    get(id) {
      return this.plugins.find(plugin => plugin.id === id) || null;
    }

    async load() {
      const res = await fetch('/api/plugins', { method: 'GET', cache: 'no-store' });
      if (!res.ok) throw await responseError('Plugin list load', res);
      let data;
      try {
        data = JSON.parse(await res.text());
      } catch (_) {
        throw new Error('Plugin catalog is not valid JSON. Refresh the page and try again.');
      }
      const plugins = normalizeCatalog(data);
      const signature = JSON.stringify(plugins);
      this.plugins = plugins;
      if (signature !== this.signature) {
        this.signature = signature;
        this.render();
      }
      this.onCatalog(plugins.map(plugin => ({ ...plugin, params: plugin.params.map(p => ({ ...p })) })));
      return plugins;
    }

    async mutate(busyText, action, doneText) {
      if (this.busy) throw new Error('Another plugin operation is still running.');
      this.busy = true;
      this.render();
      this.setPanelStatus(busyText);
      try {
        const result = await action();
        await this.load();
        await this.onChanged();
        this.setPanelStatus(typeof doneText === 'function' ? doneText(result) : doneText, 'ok');
        this.setStatus(typeof doneText === 'function' ? doneText(result) : doneText, 'ok');
        return true;
      } finally {
        this.busy = false;
        this.render();
      }
    }

    validateFile(file) {
      if (!file) throw new Error('Select an AudioX .axm plugin package first.');
      if (!/\.axm$/i.test(String(file.name || ''))) throw new Error('Plugin packages must be .axm files.');
      if (!(file.size > 0) || file.size > MAX_PLUGIN_BYTES) {
        throw new Error('Plugin packages must be between 1 byte and 12 MiB.');
      }
    }

    async install(file) {
      this.validateFile(file);
      if (!await window.AudioxUI.confirm(
        `Install ${file.name}? Plugins run native code inside the audio engine and can read or damage device data. ` +
        'A custom UI can also show misleading controls. Install only packages from authors you trust.', {
          title: 'Install native code plugin?', label: 'Install plugin',
        })) return false;
      return this.mutate(`Uploading ${file.name}...`, async () => {
        const res = await fetch('/api/plugins/install', {
          method: 'PUT',
          headers: { 'Content-Type': 'application/octet-stream' },
          body: file,
        });
        if (!res.ok) throw await responseError('Plugin install', res);
        if (this.fileInput) this.fileInput.value = '';
        return file.name;
      }, name => `Installed ${name}`);
    }

    async installSelected() {
      try {
        await this.install(this.fileInput?.files?.[0]);
      } catch (error) {
        this.setPanelStatus(String(error?.message || error), 'warn');
      }
    }

    async remove(id) {
      const plugin = this.get(normalizePluginId(id));
      if (!plugin) throw new Error('Unknown plugin.');
      if (!await window.AudioxUI.confirm(
        `Remove ${plugin.name} (${plugin.id})? Its native code is unloaded and effects using it become unavailable until it is installed again.`, {
          title: 'Remove plugin?', label: 'Remove plugin',
        })) return false;
      return this.mutate(`Removing ${plugin.name}...`, async () => {
        const res = await fetch('/api/plugins/remove', {
          method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: `id=${plugin.id}`,
        });
        if (!res.ok) throw await responseError('Plugin remove', res);
      }, `Removed ${plugin.name}`);
    }

    async reload(id) {
      const plugin = this.get(normalizePluginId(id));
      if (!plugin) throw new Error('Unknown plugin.');
      if (!await window.AudioxUI.confirm(
        `Reload ${plugin.name} (${plugin.id})? Its native code is restarted from the installed package; audio through its effects may drop briefly.`, {
          title: 'Reload native code plugin?', label: 'Reload plugin',
        })) return false;
      return this.mutate(`Reloading ${plugin.name}...`, async () => {
        const res = await fetch('/api/plugins/reload', {
          method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: `id=${plugin.id}`,
        });
        if (!res.ok) throw await responseError('Plugin reload', res);
      }, `Reloaded ${plugin.name}`);
    }

    render() {
      if (this.installButton) this.installButton.disabled = this.busy;
      if (!this.listEl) return;
      this.listEl.innerHTML = '';
      if (!this.plugins.length) {
        const empty = document.createElement('p');
        empty.className = 'small';
        empty.textContent = 'No plugins installed.';
        this.listEl.appendChild(empty);
        return;
      }
      for (const plugin of this.plugins) {
        const row = document.createElement('div');
        row.className = 'plugin-row';
        const meta = document.createElement('div');
        meta.className = 'plugin-meta';
        const title = document.createElement('strong');
        title.textContent = plugin.name;
        const detail = document.createElement('div');
        detail.className = 'small';
        detail.textContent = `${plugin.id}${plugin.version ? ` · version ${plugin.version}` : ''} · ` +
          (plugin.ui ? 'custom UI included' : 'no custom UI');
        const params = document.createElement('div');
        params.className = 'small';
        params.textContent = plugin.params.length
          ? `Parameters: ${plugin.params.map(formatRange).join('; ')}`
          : 'No parameters.';
        meta.append(title, detail, params);
        const reload = document.createElement('button');
        reload.textContent = 'Reload';
        reload.disabled = this.busy;
        reload.setAttribute('aria-label', `Reload plugin ${plugin.name}`);
        reload.addEventListener('click', () => this.reload(plugin.id)
          .catch(error => this.setPanelStatus(String(error?.message || error), 'warn')));
        const remove = document.createElement('button');
        remove.className = 'warn';
        remove.textContent = 'Remove';
        remove.disabled = this.busy;
        remove.setAttribute('aria-label', `Remove plugin ${plugin.name}`);
        remove.addEventListener('click', () => this.remove(plugin.id)
          .catch(error => this.setPanelStatus(String(error?.message || error), 'warn')));
        row.append(meta, reload, remove);
        this.listEl.appendChild(row);
      }
    }
  }

  window.AudioxPlugins = {
    MAX_PLUGIN_BYTES, normalizePluginId, normalizeCatalog, assetUrl,
    createManager(options) {
      return new PluginManager(options);
    },
  };
})();
