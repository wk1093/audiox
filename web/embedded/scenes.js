(() => {
  'use strict';
  const hex = (text) => Array.from(new TextEncoder().encode(text), (byte) => byte.toString(16).padStart(2, '0')).join('');
  async function request(path, values = null, method = 'POST') {
    const query = values ? `?${new URLSearchParams(values)}` : '';
    const send = async () => {
      const response = await apiFetch(`/api/scenes${path}${query}`, { method: values ? method : 'GET', cache: 'no-store' });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || `Scene request failed (${response.status})`);
      return data;
    };
    return values ? AudioxUI.runTask('Updating setup scenes', send) : send();
  }
  function mount() {
    const host = document.getElementById('scenes-panel');
    if (!host) return;
    host.innerHTML = `<div class="scene-controls">
        <label>Scene name <input id="scene-name" type="text" maxlength="63" placeholder="Clean / Live / Studio"></label>
        <button id="scene-capture" type="button">Capture new scene</button>
        <button id="scene-refresh" type="button">Refresh</button>
      </div><div id="scene-status" class="small" role="status" aria-live="polite"></div>
      <div id="scene-list" class="scene-list"></div>`;
    const status = host.querySelector('#scene-status');
    const list = host.querySelector('#scene-list');
    const captureButton = host.querySelector('#scene-capture');
    let busy = false;
    let sequence = null;
    let signature = '';
    const statusText = (text) => { status.textContent = text; };
    const requireSavedRouting = () => {
      if (AudioxUI.hasUnsavedChanges('routing'))
        throw new Error('Save or undo pending routing edits before capturing, updating or recalling a scene.');
    };
    const changed = () => {
      if (AudioxUI.hasUnsavedChanges('routing')) {
        const message = 'Scene recalled on device. Your unsaved routing draft was preserved. Download it before loading active routing.';
        statusText(message);
        AudioxUI.notify(message, 'warn', 'scenes');
        window.dispatchEvent(new CustomEvent('audiox-scene-conflict'));
        return;
      }
      window.dispatchEvent(new CustomEvent('audiox-scene-recalled'));
    };
    async function run(action) {
      if (busy) return;
      busy = true;
      host.querySelectorAll('button').forEach((button) => { button.disabled = true; });
      try {
        if (AudioxUI.isBusy()) throw new Error('Another device action is already running.');
        await action();
      } catch (error) {
        statusText(error.message);
        AudioxUI.notify(error.message, 'warn', 'scenes');
      }
      finally {
        busy = false;
        host.querySelectorAll('button').forEach((button) => { button.disabled = false; });
      }
    }
    async function refresh() {
      const data = await request('');
      if (sequence !== null && data.sequence !== sequence) changed();
      sequence = data.sequence;
      const nextSignature = JSON.stringify(data);
      if (nextSignature === signature) return;
      signature = nextSignature;
      list.replaceChildren();
      for (const scene of data.scenes) {
        const row = document.createElement('div');
        row.className = 'scene-row';
        row.dataset.active = String(scene.id === data.active);
        const description = document.createElement('div');
        description.className = 'scene-description';
        const name = document.createElement('strong');
        name.textContent = scene.name;
        const details = document.createElement('small');
        details.textContent = `${scene.effects} effects · ${scene.routes} routes · ${scene.volumes} gains`;
        description.append(name, details);
        row.append(description);
        const button = (label, action) => {
          const control = document.createElement('button');
          control.type = 'button';
          control.textContent = label;
          control.addEventListener('click', () => run(action));
          row.append(control);
        };
        button('Recall', async () => {
          requireSavedRouting();
          if (!await AudioxUI.confirm(`Recall "${scene.name}"? This replaces routing, gains and all effects.`, {
            title: 'Recall setup scene', label: 'Recall scene',
          })) return;
          requireSavedRouting();
          await request('/recall', { id: scene.id });
          statusText(`Recalled ${scene.name}`);
          await refresh();
        });
        button('Update capture', async () => {
          requireSavedRouting();
          if (!await AudioxUI.confirm(`Replace "${scene.name}" with the current live setup?`, {
            title: 'Update setup scene', label: 'Replace capture',
          })) return;
          requireSavedRouting();
          await request('/save', { id: scene.id, name_hex: hex(scene.name), channel: scene.channel, note: scene.note });
          statusText(`Updated ${scene.name}`);
          await refresh();
        });
        const channelLabel = document.createElement('label');
        channelLabel.textContent = 'MIDI channel ';
        const channel = document.createElement('input');
        channel.type = 'number'; channel.min = '1'; channel.max = '16';
        channel.value = scene.channel >= 0 ? scene.channel + 1 : 1;
        channel.setAttribute('aria-label', `${scene.name} MIDI channel`);
        channelLabel.append(channel);
        const noteLabel = document.createElement('label');
        noteLabel.textContent = 'Note ';
        const note = document.createElement('input');
        note.type = 'number'; note.min = '0'; note.max = '127';
        note.value = scene.note >= 0 ? scene.note : '';
        note.placeholder = 'None';
        note.setAttribute('aria-label', `${scene.name} MIDI note (blank disables)`);
        noteLabel.append(note);
        row.append(channelLabel, noteLabel);
        button('Bind MIDI', async () => {
          const enabled = note.value !== '';
          if (enabled && (!channel.checkValidity() || !note.checkValidity() || !Number.isInteger(Number(note.value)) ||
              !Number.isInteger(Number(channel.value)) || channel.value === '')) throw new Error('Use channel 1–16 and note 0–127.');
          await request('/bind', { id: scene.id, channel: enabled ? Number(channel.value) - 1 : -1, note: enabled ? Number(note.value) : -1 });
          statusText(enabled ? `MIDI binding saved for ${scene.name}` : `MIDI binding removed for ${scene.name}`);
          await refresh();
        });
        button('Delete', async () => {
          if (!await AudioxUI.confirm(`Delete scene "${scene.name}"?`, {
            title: 'Delete setup scene', label: 'Delete scene',
          })) return;
          await request('/delete', { id: scene.id }, 'DELETE');
          statusText(`Deleted ${scene.name}`);
          await refresh();
        });
        list.append(row);
      }
      if (!data.scenes.length) list.textContent = 'No saved scenes yet.';
    }
    captureButton.addEventListener('click', () => run(async () => {
      requireSavedRouting();
      const name = host.querySelector('#scene-name').value.trim();
      if (!name || new TextEncoder().encode(name).length > 63) throw new Error('Enter a scene name (up to 63 UTF-8 bytes).');
      const random = new Uint32Array(1);
      window.crypto.getRandomValues(random);
      await request('/save', { id: `scene-${Date.now().toString(36)}-${random[0].toString(36)}`, name_hex: hex(name), channel: -1, note: -1 });
      host.querySelector('#scene-name').value = '';
      statusText(`Captured ${name}`);
      await refresh();
    }));
    host.querySelector('#scene-refresh').addEventListener('click', () => run(refresh));
    run(refresh);
    // Background polls must not go through run(): toggling every button's disabled state makes them flicker.
    let polling = false;
    window.setInterval(async () => {
      if (polling || busy || AudioxUI.isBusy() || document.hidden) return;
      polling = true;
      try { await refresh(); } catch (error) { statusText(error.message); } finally { polling = false; }
    }, 2500);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', mount);
  else mount();
})();
