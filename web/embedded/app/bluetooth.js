const bluetoothHideUnnamed = document.getElementById('bluetooth-hide-unnamed');
let bluetoothDevices = [];

bluetoothHideUnnamed.addEventListener('change', () => renderBluetoothDevices(bluetoothDevices));

function isBluetoothInputOnly(device) {
  return Boolean(device?.audio_source) && !device?.audio_sink;
}

function bluetoothDeviceStatus(device) {
  if (!device.paired && !device.connected) {
    return 'Discovered; put device in pairing mode to pair';
  }
  // Older status payloads only report the combined connected/streaming flags.
  const outputUp = device.output_connected ?? (device.connected && !isBluetoothInputOnly(device));
  const inputUp = device.input_connected ?? (device.connected && isBluetoothInputOnly(device));
  const parts = [];
  if (outputUp) {
    parts.push((device.output_streaming ?? device.streaming) === false
      ? 'Connected as audio output; audio paused' : 'Connected as audio output');
  }
  if (inputUp) {
    parts.push((device.input_streaming ?? device.streaming)
      ? 'Connected as audio input; receiving audio' : 'Connected as audio input; waiting for audio');
  }
  if (parts.length) {
    return parts.join(' · ');
  }
  if (device.connected) {
    return 'Connected for audio';
  }
  if (device.connecting) {
    return 'Connecting...';
  }
  return isBluetoothInputOnly(device) ? 'Audio input, not connected' : 'Paired, not connected';
}

function bluetoothDeviceHasName(device) {
  const name = String(device.name || '').trim();
  const compact = (value) => value.replace(/[:-]/g, '').toLowerCase();
  return Boolean(name) && compact(name) !== compact(String(device.address || ''));
}

function renderBluetoothDevices(devices) {
  if (!bluetoothDevicesEl) {
    return;
  }
  const focused = bluetoothDevicesEl.contains(document.activeElement)
    ? document.activeElement.dataset.deviceAction : null;
  const supported = devices.filter((device) => device.classic);
  const nearby = supported.filter((device) => !device.paired && !device.connected);
  const hiddenCount = bluetoothHideUnnamed.checked
    ? nearby.filter((device) => !bluetoothDeviceHasName(device)).length : 0;
  bluetoothDevicesEl.replaceChildren();
  for (const [title, members, emptyText] of [
    ['My devices', supported.filter((device) => device.paired || device.connected), 'No saved devices. Choose Add device to find and pair an audio device.'],
    ['Nearby devices', nearby.filter((device) => !bluetoothHideUnnamed.checked || bluetoothDeviceHasName(device)),
      hiddenCount ? `${hiddenCount} unnamed device(s) hidden. Turn off Hide unnamed devices to show them.`
        : 'No nearby devices found. Put your device in pairing mode, then choose Add device.'],
  ]) {
    const group = document.createElement('section');
    group.className = 'bluetooth-group';
    const heading = document.createElement('h2');
    heading.textContent = title;
    const list = document.createElement('div');
    list.className = 'bluetooth-device-list';
    group.append(heading, list);
    if (title === 'Nearby devices' && hiddenCount && members.length) {
      const notice = document.createElement('p');
      notice.className = 'small';
      notice.textContent = `${hiddenCount} unnamed device(s) hidden.`;
      heading.after(notice);
    }
    bluetoothDevicesEl.append(group);
    if (!members.length) {
      const empty = document.createElement('div');
      empty.className = 'bluetooth-empty';
      empty.textContent = emptyText;
      list.append(empty);
    }
    for (const device of members.sort((a, b) => Number(b.connected) - Number(a.connected) ||
      String(a.name || a.address).localeCompare(String(b.name || b.address)))) {
      const card = document.createElement('section');
      card.className = `card bluetooth-device${device.connected ? ' connected' : ''}`;
      const info = document.createElement('div');
      info.className = 'bluetooth-device-info';
      const name = document.createElement('strong');
      name.textContent = device.name || device.address;
      const status = document.createElement('span');
      status.className = 'bluetooth-device-state';
      status.textContent = bluetoothDeviceStatus(device);
      const address = document.createElement('div');
      address.className = 'bluetooth-device-address';
      address.textContent = `${device.address} · Classic Bluetooth`;
      info.append(name, status, address);
      const actions = document.createElement('div');
      actions.className = 'bluetooth-device-actions';
      card.append(info, actions);
      const addAction = (label, action, style = 'flat') => {
        const button = document.createElement('button');
        button.textContent = label;
        button.className = style;
        button.disabled = bluetoothBusy;
        button.dataset.deviceAction = `${device.address}:${label}`;
        button.setAttribute('aria-label', `${label} ${device.name || device.address}`);
        button.addEventListener('click', action);
        actions.append(button);
      };
      if (!device.paired) {
        addAction('Pair', () => pairBluetoothDevice(device.address), 'primary');
      } else {
        const autoConnectLabel = document.createElement('label');
        autoConnectLabel.className = 'bluetooth-auto-connect';
        const autoConnect = document.createElement('input');
        autoConnect.type = 'checkbox';
        autoConnect.checked = device.auto_connect !== false;
        autoConnect.dataset.deviceAction = `${device.address}:Auto-connect`;
        autoConnect.setAttribute('role', 'switch');
        autoConnect.setAttribute('aria-label', `Auto-connect ${device.name || device.address}`);
        autoConnect.disabled = bluetoothBusy;
        autoConnect.addEventListener('change', () => setBluetoothAutoConnect(device.address, autoConnect.checked));
        const autoConnectText = document.createElement('span');
        autoConnectText.textContent = 'Auto-connect';
        autoConnectLabel.append(autoConnect, autoConnectText);
        actions.append(autoConnectLabel);
        if (device.connected) {
          addAction('Disconnect', () => disconnectBluetoothDevice(device.address));
        } else if (device.classic) {
          addAction('Connect', () => connectBluetoothDevice(device.address), 'primary');
        }
        addAction('Forget', async () => {
          if (await AudioxUI.confirm(`Forget ${device.name || device.address}? You will need to pair it again to reconnect.`, {
            title: 'Forget Bluetooth device?', label: 'Forget device',
          })) await unpairBluetoothDevice(device.address);
        });
      }
      list.append(card);
    }
  }
  if (focused) {
    Array.from(bluetoothDevicesEl.querySelectorAll('[data-device-action]'))
      .find((button) => button.dataset.deviceAction === focused)?.focus();
  }
}

let bluetoothBusy = false;
let bluetoothPollTimer = null;
let bluetoothLoadInFlight = false;
let bluetoothRequestInFlight = false;

async function loadBluetoothDevices() {
  if (!bluetoothStatusEl || bluetoothLoadInFlight || AudioxUI.isBusy()) {
    return;
  }
  if (bluetoothPollTimer) {
    clearTimeout(bluetoothPollTimer);
    bluetoothPollTimer = null;
  }
  bluetoothLoadInFlight = true;
  try {
    const res = await apiFetch('/api/bluetooth/devices', { method: 'GET', signal: AbortSignal.timeout(10000) });
    const data = await res.json();
    if (!res.ok || !data.ok) {
      throw new Error(data.error || `request failed: ${res.status}`);
    }
    bluetoothBusy = bluetoothRequestInFlight || Boolean(data.scanning || data.pairing || data.connecting);
    bluetoothDevices = Array.isArray(data.devices) ? data.devices : [];
    renderBluetoothDevices(bluetoothDevices);
    bluetoothStatusEl.textContent = data.message || (data.ready ? 'Ready' : 'Adapter not ready');
    AudioxUI.resolve('bluetooth-load');
    const scanButton = document.getElementById('btn-bluetooth-scan');
    if (scanButton) {
      scanButton.disabled = bluetoothBusy || Boolean(data.connected) || !data.ready;
      scanButton.textContent = data.scanning ? 'Scanning...' : 'Add device';
    }
    const hint = document.getElementById('bluetooth-scan-hint');
    hint.hidden = !data.connected;
    hint.textContent = 'Disconnect the current audio device before adding another device.';
  } catch (err) {
    bluetoothStatusEl.textContent = AudioxUI.formatMessage(err);
    setStatus(err, 'warn', 'bluetooth-load');
  } finally {
    bluetoothLoadInFlight = false;
    if (state.activeTab === 'bluetooth') {
      bluetoothPollTimer = setTimeout(loadBluetoothDevices, bluetoothBusy ? 1000 : 3000);
    }
  }
}

async function bluetoothRequest(path, body) {
  if (bluetoothBusy) return;
  bluetoothRequestInFlight = true;
  bluetoothBusy = true;
  document.getElementById('btn-bluetooth-scan').disabled = true;
  bluetoothDevicesEl.querySelectorAll('button').forEach((button) => { button.disabled = true; });
  bluetoothStatusEl.textContent = 'Applying Bluetooth request...';
  try {
    const res = await apiFetch(path, { method: 'POST', body });
    const data = await res.json();
    if (!res.ok || !data.ok) {
      throw new Error(data.error || `request failed: ${res.status}`);
    }
    AudioxUI.resolve('bluetooth-action');
  } catch (err) {
    bluetoothStatusEl.textContent = AudioxUI.formatMessage(err);
    setStatus(err, 'warn', 'bluetooth-action');
  } finally {
    bluetoothRequestInFlight = false;
    bluetoothBusy = false;
    await loadBluetoothDevices();
  }
}

function scanBluetoothDevices() {
  return bluetoothRequest('/api/bluetooth/scan', '');
}

function pairBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/pair', `address=${address}`);
}

function connectBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/connect', `address=${address}`);
}

function disconnectBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/disconnect', address ? `address=${address}` : '');
}

function unpairBluetoothDevice(address) {
  return bluetoothRequest('/api/bluetooth/unpair', `address=${address}`);
}

function setBluetoothAutoConnect(address, enabled) {
  return bluetoothRequest('/api/bluetooth/auto-connect', `address=${address}&enabled=${enabled}`);
}

