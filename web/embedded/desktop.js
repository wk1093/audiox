(function (root) {
  'use strict';

  let inventorySnapshot = { nodes: [], error: 'Desktop discovery unavailable' };
  const inventoryListeners = new Set();
  function publishInventory(nodes, error = '') {
    inventorySnapshot = { nodes, error };
    for (const listener of inventoryListeners) listener(inventorySnapshot);
  }
  function subscribeInventory(listener) {
    inventoryListeners.add(listener);
    listener(inventorySnapshot);
    return () => inventoryListeners.delete(listener);
  }

  function create({ api, tab, host, os, capabilities, media, routing, routeForm,
    stream, sink, routeSubmit, routeStatus, inventory, inventoryRefresh,
    inventoryStatus, inventoryList, autoRouting, autoForm, autoDirection, autoApp,
    autoTarget, autoPair, autoAppPattern, autoTargetPattern, autoSubmit, virtualChannels,
    autoStatus, autoRules, onDisconnect, defaultSection, defaultForm, defaultSink,
    defaultSubmit, defaultCurrent, defaultStatus }) {
    let routeAvailable = false;
    let session = 0;
    let busy = false;
    let generation = 0;
    let inventorySession = 0;
    let inventoryGeneration = 0;
    let inventoryTimer;
    let nodes = [];
    let autoSession = 0;
    let autoGeneration = 0;
    let autoTimer;
    let rules = [];
    let virtualEnabled = true;
    let autoBusy = false;
    let defaultSession = 0, defaultGeneration = 0, defaultTimer, defaultBusy = false;
    const showStatus = (text) => { if (routeStatus) routeStatus.textContent = text; };

    function setOptions(element, entries, placeholder) {
      const previous = element.value;
      const options = entries.map(({ value, label }) => {
        const option = root.document.createElement('option');
        option.value = value;
        option.textContent = label;
        return option;
      });
      if (!options.length) {
        const option = root.document.createElement('option');
        option.value = '';
        option.textContent = placeholder;
        options.push(option);
      }
      element.replaceChildren(...options);
      if (entries.some((entry) => entry.value === previous)) element.value = previous;
    }

    function refreshPairs() {
      const node = nodes.find((item) => item['node.name'] === autoTarget.value);
      let channels = node?.['audio.channels'];
      if (!Number.isInteger(channels) && typeof node?.['audio.position'] === 'string') {
        const positions = node['audio.position'].replace(/[\[\]]/g, '').split(/[,\s]+/).filter(Boolean);
        channels = positions.length;
      }
      const entries = [];
      if (Number.isInteger(channels) && channels > 0 && channels <= 32 && channels % 2 === 0)
        for (let pair = 0; pair < channels / 2; pair++)
          entries.push({ value: String(pair), label: `Channels ${pair * 2 + 1}/${pair * 2 + 2}` });
      setOptions(autoPair, entries, 'No stereo pairs available');
      autoSubmit.disabled = autoBusy || !autoApp.value || !autoTarget.value || !autoPair.value;
    }

    function refreshChoices() {
      if (defaultSink) {
        const targets = new Map();
        for (const node of nodes) {
          if ((node['media.class'] === 'Audio/Sink' || node['media.class']?.startsWith('Audio/Sink/')) &&
              node['node.name'] && !node['node.name'].startsWith('audiox_auto_'))
            targets.set(node['node.name'], { value: node['node.name'],
              label: node['node.description'] || node['node.name'] });
        }
        setOptions(defaultSink, [...targets.values()], 'No playback devices');
        if (defaultSubmit) defaultSubmit.disabled = defaultBusy || !defaultSink.value;
      }
      if (!autoForm) return;
      const capture = autoDirection.value === 'c';
      const appClass = capture ? 'Stream/Input/Audio' : 'Stream/Output/Audio';
      const deviceClass = capture ? 'Audio/Source' : 'Audio/Sink';
      const apps = new Map(), targets = new Map();
      for (const node of nodes) {
        if (node['media.class'] === appClass) {
          const name = node['application.name'] || node['node.name'];
          if (name) apps.set(name, { value: name, label: name });
        } else if ((node['media.class'] === deviceClass ||
                    node['media.class']?.startsWith(`${deviceClass}/`)) && node['node.name'] &&
                   !node['node.name'].startsWith('audiox_auto_')) {
          targets.set(node['node.name'], { value: node['node.name'],
            label: node['node.description'] || node['node.name'] });
        }
      }
      setOptions(autoApp, [...apps.values()], 'No active applications');
      setOptions(autoTarget, [...targets.values()], 'No destinations');
      refreshPairs();
    }

    function ruleText(list, enabled = virtualEnabled) {
      const hex = (text) => [...new TextEncoder().encode(text)]
        .map((byte) => byte.toString(16).padStart(2, '0')).join('');
      return '# audiox desktop routes v1\n' + (enabled ? '' : '# virtual-channels off\n') + list.map((rule) =>
        [rule.direction, rule.appRegex ? 'r' : 'e', rule.targetRegex ? 'r' : 'e',
          String(rule.pair), hex(rule.app), hex(rule.target)].join('\t') + '\n').join('');
    }

    function parseRules(text) {
      const lines = text.split('\n');
      if (lines.shift() !== '# audiox desktop routes v1' || lines.pop() !== '') throw new Error('Invalid saved routing rules.');
      virtualEnabled = lines[0] !== '# virtual-channels off';
      if (!virtualEnabled) lines.shift();
      if (virtualChannels) virtualChannels.checked = virtualEnabled;
      const decode = (hex) => {
        if (!/^(?:[0-9a-f]{2}){1,128}$/.test(hex)) throw new Error('Invalid saved routing name.');
        return new TextDecoder('utf-8', { fatal: true }).decode(Uint8Array.from(hex.match(/../g), (byte) => parseInt(byte, 16)));
      };
      return lines.map((line) => {
        const [direction, appMode, targetMode, pair, app, target, ...extra] = line.split('\t');
        if (extra.length || !['p', 'c'].includes(direction) || !['e', 'r'].includes(appMode) ||
            !['e', 'r'].includes(targetMode) || !/^(?:[0-9]|1[0-5])$/.test(pair))
          throw new Error('Invalid saved routing rule.');
        return { direction, appRegex: appMode === 'r', targetRegex: targetMode === 'r',
          pair: Number(pair), app: decode(app), target: decode(target) };
      });
    }

    function renderRules() {
      const entries = rules.map((rule, index) => {
        const item = root.document.createElement('li');
        const text = root.document.createElement('span');
        text.textContent = `${rule.direction === 'c' ? 'Capture' : 'Playback'}: ${rule.app}` +
          `${rule.appRegex ? ' (pattern)' : ''} -> ${rule.target}${rule.targetRegex ? ' (pattern)' : ''}` +
          `, channels ${rule.pair * 2 + 1}/${rule.pair * 2 + 2} `;
        const remove = root.document.createElement('button');
        remove.type = 'button';
        remove.textContent = 'Remove';
        remove.disabled = autoBusy;
        remove.addEventListener('click', () => { void saveRules(rules.filter((_, n) => n !== index)); });
        item.append(text, remove);
        return item;
      });
      autoRules.replaceChildren(...entries);
    }

    async function rulesRequest(url, options) {
      const response = await api.request(url, { ...options, cache: 'no-store', signal: AbortSignal.timeout(15000) });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || `Request failed (${response.status}).`);
      return data;
    }

    async function loadRules() {
      if (!autoSession) return;
      const current = ++autoGeneration;
      root.clearTimeout(autoTimer);
      autoBusy = true;
      if (virtualChannels) virtualChannels.disabled = true;
      refreshPairs();
      try {
        const data = await rulesRequest('/api/desktop/rules', { method: 'GET' });
        if (current !== autoGeneration) return;
        rules = parseRules(data.rules);
        renderRules();
        if (data.status === 'pending') {
          autoStatus.textContent = 'Synchronizing rules with the desktop...';
          autoTimer = root.setTimeout(loadRules, 500);
          return;
        }
        if (data.status !== 'succeeded') throw new Error('Automatic routing is unavailable.');
        autoStatus.textContent = data.routing_ok === false
          ? 'A route could not be applied or restored. Check the companion logs, selected devices, and WirePlumber policy installation.'
          : data.routing_ok === true ? 'Rules synchronized. Routes apply as matching streams appear.'
            : 'Rules synchronized; waiting for the desktop to check routes.';
        autoBusy = false;
        if (virtualChannels) virtualChannels.disabled = false;
        refreshPairs();
        renderRules();
        autoTimer = root.setTimeout(loadRules, 10000);
      } catch (error) {
        if (current === autoGeneration) {
          autoStatus.textContent = error.message || 'Could not load routing rules.';
          autoBusy = true;
          if (virtualChannels) virtualChannels.disabled = true;
          refreshPairs();
          renderRules();
          autoTimer = root.setTimeout(loadRules, 10000);
        }
      }
    }

    async function saveRules(next, enabled = virtualEnabled) {
      if (!autoSession || autoBusy) return;
      const current = ++autoGeneration;
      root.clearTimeout(autoTimer);
      const savedSession = autoSession;
      autoBusy = true;
      if (virtualChannels) virtualChannels.disabled = true;
      refreshPairs();
      renderRules();
      autoStatus.textContent = 'Saving routing rules...';
      try {
        await rulesRequest(`/api/desktop/rules?session=${savedSession}`, {
          method: 'PUT', headers: { 'Content-Type': 'text/plain' }, body: ruleText(next, enabled),
        });
        if (current === autoGeneration) void loadRules();
      } catch (error) {
        if (current === autoGeneration) {
          autoStatus.textContent = error.message || 'Could not save routing rules.';
          autoBusy = false;
          if (virtualChannels) { virtualChannels.checked = virtualEnabled; virtualChannels.disabled = false; }
          refreshPairs();
          renderRules();
        }
      }
    }

    function addRule(event) {
      event.preventDefault();
      if (!autoSession || autoBusy || !autoApp.value || !autoTarget.value || !autoPair.value) return;
      const app = autoAppPattern.value || autoApp.value;
      const target = autoTargetPattern.value || autoTarget.value;
      if (rules.length >= 16 || new TextEncoder().encode(app).length > 128 ||
          new TextEncoder().encode(target).length > 128) {
        autoStatus.textContent = 'At most 16 rules are allowed; names and patterns must be at most 128 bytes.';
        return;
      }
      void saveRules([...rules, { direction: autoDirection.value, app, target,
        appRegex: !!autoAppPattern.value, targetRegex: !!autoTargetPattern.value, pair: Number(autoPair.value) }]);
    }

    autoForm?.addEventListener('submit', addRule);
    virtualChannels?.addEventListener('change', () => {
      void saveRules(rules, virtualChannels.checked);
    });
    autoDirection?.addEventListener('change', refreshChoices);
    autoTarget?.addEventListener('change', refreshPairs);
    async function refreshInventory() {
      if (!inventorySession) return;
      const current = ++inventoryGeneration;
      const currentSession = inventorySession;
      root.clearTimeout(inventoryTimer);
      inventoryRefresh.disabled = true;
      inventoryStatus.textContent = 'Discovering PipeWire nodes...';
      try {
        async function request(url, options) {
          const response = await api.request(url, { ...options, cache: 'no-store', signal: AbortSignal.timeout(15000) });
          const data = await response.json();
          if (!response.ok) throw new Error(data.error || `Request failed (${response.status}).`);
          return data;
        }
        const accepted = await request('/api/desktop/inventory', {
          method: 'POST', body: new URLSearchParams({ session: String(currentSession) }).toString(),
        });
        if (!Number.isInteger(accepted.id) || accepted.id <= 0 || accepted.id > 4294967295)
          throw new Error('Invalid discovery response.');
        const deadline = Date.now() + 30000;
        while (current === inventoryGeneration) {
          const result = await request(`/api/desktop/inventory?id=${accepted.id}`, { method: 'GET' });
          if (current !== inventoryGeneration) return;
          if (result.id !== accepted.id) throw new Error('Invalid discovery result.');
          if (result.status === 'succeeded') {
            if (!Array.isArray(result.nodes) || !result.nodes.every((node) =>
              node && Number.isInteger(node.id) &&
              ['node.name', 'node.description', 'application.name', 'media.class'].every((key) =>
                node[key] === undefined || typeof node[key] === 'string')))
              throw new Error('Invalid discovery result.');
            nodes = result.nodes;
            publishInventory(nodes);
            const entries = nodes.map((node) => {
              const label = node['application.name'] || node['node.description'] || node['node.name'] || '(unnamed)';
              const name = node['node.name'] ? ` - ${node['node.name']}` : '';
              const kind = node['media.class'] ? ` [${node['media.class']}]` : '';
              return { value: String(node.id), label: `${label}${name}${kind}` };
            });
            setOptions(inventoryList, entries, 'No nodes found');
            refreshChoices();
            inventoryStatus.textContent = `${result.nodes.length} nodes found.`;
            return;
          }
          if (result.status !== 'pending') throw new Error(`Discovery ${result.status}.`);
          if (Date.now() >= deadline) throw new Error('Timed out waiting for discovery.');
          await new Promise((resolve) => root.setTimeout(resolve, 300));
        }
      } catch (error) {
        if (current === inventoryGeneration) {
          nodes = [];
          publishInventory([], error.message || 'Desktop discovery failed');
          setOptions(inventoryList, [], 'Discovery failed');
          refreshChoices();
          inventoryStatus.textContent = error.message || 'Discovery failed.';
        }
      } finally {
        if (current === inventoryGeneration) {
          inventoryRefresh.disabled = false;
          inventoryTimer = root.setTimeout(refreshInventory, 10000);
        }
      }
    }

    inventoryRefresh?.addEventListener('click', refreshInventory);
    async function defaultRequest(url, options) {
      const response = await api.request(url, { ...options, cache: 'no-store', signal: AbortSignal.timeout(15000) });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || `Request failed (${response.status}).`);
      return data;
    }
    async function refreshDefault() {
      if (!defaultSession) return;
      const current = defaultGeneration, savedSession = defaultSession;
      root.clearTimeout(defaultTimer);
      try {
        const result = await defaultRequest(`/api/desktop/default?session=${savedSession}`, { method: 'GET' });
        if (current !== defaultGeneration) return;
        if (typeof result.name !== 'string' || !result.name) throw new Error('Invalid default-sink response.');
        const node = nodes.find(item => item['node.name'] === result.name);
        defaultCurrent.textContent = node?.['node.description'] || result.name;
        if (!defaultBusy) defaultStatus.textContent = '';
      } catch (error) {
        if (current === defaultGeneration) {
          defaultCurrent.textContent = 'Unavailable';
          if (!defaultBusy) defaultStatus.textContent = error.message || 'Default sink unavailable.';
        }
      } finally {
        if (current === defaultGeneration) defaultTimer = root.setTimeout(refreshDefault, 2000);
      }
    }
    async function setDefault(event) {
      event.preventDefault();
      if (!defaultSession || defaultBusy || !defaultSink.value) return;
      const current = defaultGeneration, savedSession = defaultSession;
      const name = defaultSink.value;
      defaultBusy = true;
      defaultSubmit.disabled = true;
      defaultStatus.textContent = 'Setting default device...';
      try {
        const hex = [...new TextEncoder().encode(name)].map(byte => byte.toString(16).padStart(2, '0')).join('');
        const accepted = await defaultRequest('/api/desktop/default', {
          method: 'POST', body: `session=${savedSession}&name_hex=${hex}`,
        });
        if (!Number.isInteger(accepted.id) || accepted.id <= 0 || accepted.id > 4294967295)
          throw new Error('Invalid default-sink response.');
        const deadline = Date.now() + 15000;
        while (current === defaultGeneration) {
          const result = await defaultRequest(`/api/desktop/default?id=${accepted.id}`, { method: 'GET' });
          if (current !== defaultGeneration) return;
          if (result.id !== accepted.id) throw new Error('Invalid default-sink result.');
          if (result.status === 'succeeded') {
            defaultStatus.textContent = 'Default device changed.';
            return;
          }
          if (result.status !== 'pending') throw new Error(`Default device change ${result.status}.`);
          if (Date.now() >= deadline) throw new Error('Timed out waiting for the default device change.');
          await new Promise(resolve => root.setTimeout(resolve, 300));
        }
      } catch (error) {
        if (current === defaultGeneration) defaultStatus.textContent = error.message || 'Default device change failed.';
      } finally {
        if (current === defaultGeneration) {
          defaultBusy = false;
          defaultSubmit.disabled = !defaultSink.value;
        }
      }
    }
    defaultForm?.addEventListener('submit', setDefault);
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
      const canDiscover = inventory && inventoryRefresh && inventoryStatus && inventoryList &&
        connected && hello.os === 'linux' && hello.capabilities.includes('pipewire_nodes') &&
        Number.isInteger(data.session) && data.session > 0 && data.session <= 4294967295;
      const canDefault = canDiscover && defaultSection && defaultForm && defaultSink && defaultSubmit &&
        defaultCurrent && defaultStatus && hello.capabilities.includes('default_sink');
      if (defaultSection) defaultSection.hidden = !canDefault;
      if (defaultSession !== (canDefault ? data.session : 0)) {
        defaultGeneration++;
        root.clearTimeout(defaultTimer);
        defaultSession = canDefault ? data.session : 0;
        defaultBusy = false;
        if (defaultCurrent) defaultCurrent.textContent = 'Unavailable';
        if (defaultStatus) defaultStatus.textContent = '';
        if (defaultSession) void refreshDefault();
      }
      if (inventory) inventory.hidden = !canDiscover;
      if (inventorySession !== (canDiscover ? data.session : 0)) {
        inventoryGeneration++;
        root.clearTimeout(inventoryTimer);
        nodes = [];
        publishInventory([], canDiscover ? 'Discovering PipeWire nodes' : 'Desktop discovery unavailable');
        if (inventoryList && root.document) setOptions(inventoryList, [], 'No nodes found');
        if (inventoryStatus) inventoryStatus.textContent = '';
        if (inventoryRefresh) inventoryRefresh.disabled = false;
        inventorySession = canDiscover ? data.session : 0;
        if (inventorySession) void refreshInventory();
      }
      const canAuto = canDiscover && autoRouting && autoForm && autoDirection && autoApp && autoTarget &&
        autoPair && autoAppPattern && autoTargetPattern && autoSubmit && autoStatus && autoRules &&
        hello.capabilities.includes('auto_route');
      if (autoRouting) autoRouting.hidden = !canAuto;
      if (autoSession !== (canAuto ? data.session : 0)) {
        autoGeneration++;
        root.clearTimeout(autoTimer);
        autoSession = canAuto ? data.session : 0;
        rules = [];
        autoBusy = true;
        if (autoRules) autoRules.replaceChildren();
        if (autoStatus) autoStatus.textContent = '';
        if (canAuto) {
          refreshChoices();
          void loadRules();
        }
      }
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
      inventoryGeneration++;
      inventorySession = 0;
      defaultGeneration++;
      defaultSession = 0;
      root.clearTimeout(defaultTimer);
      publishInventory([], 'Desktop discovery unavailable');
      root.clearTimeout(inventoryTimer);
      autoGeneration++;
      autoSession = 0;
      root.clearTimeout(autoTimer);
      inventoryRefresh?.removeEventListener('click', refreshInventory);
      autoForm?.removeEventListener('submit', addRule);
      autoDirection?.removeEventListener('change', refreshChoices);
      autoTarget?.removeEventListener('change', refreshPairs);
      routeForm?.removeEventListener('submit', route);
      defaultForm?.removeEventListener('submit', setDefault);
      unsubscribe?.();
      unobserve?.();
    };
  }

  root.AudioxDesktop = { create, subscribeInventory };
  if (typeof module !== 'undefined' && module.exports) module.exports = root.AudioxDesktop;
})(globalThis);
