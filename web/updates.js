(function (root) {
  'use strict';

  const repository = 'wk1093/audiox';
  const api = `https://api.github.com/repos/${repository}`;
  const mirror = 'https://wklab.net/audiox/';
  const maxArchiveSize = 256 * 1024 * 1024;
  const noticeKey = 'audiox.updateNotice';
  const dismissedKey = 'audiox.updateDismissed';
  const channelKey = 'audiox.updateChannel';
  const noticeInterval = 6 * 60 * 60 * 1000;
  const noticeRetryInterval = 60 * 60 * 1000;
  const releases = typeof module !== 'undefined' && module.exports
    ? require('./releases.js') : root.AudioxReleases;

  function deviceBoard(info) {
    if (info.board === 'pi4' || info.board === 'pi5') return info.board;
    // Older runtimes expose model but not the machine-readable board field.
    const match = /^Raspberry Pi ([45]) Model B(?:\s|$)/.exec(info.model || '');
    if (match) return `pi${match[1]}`;
    throw new Error('Unsupported or unknown Pi hardware; use a board-matched manual update.');
  }

  function selectAsset(release, board) {
    if (!release || release.draft || !Array.isArray(release.assets)) {
      throw new Error('Release metadata is invalid or unpublished.');
    }
    const name = `audiox-update-${board}.axu`;
    const asset = release.assets.find((entry) => entry.name === name && entry.state === 'uploaded');
    if (!asset) throw new Error(`${release.tag_name}: no ${name} archive. Older releases may not include AXU files.`);
    if (!Number.isSafeInteger(asset.id) || !Number.isSafeInteger(asset.size) ||
        asset.size < 16 || asset.size > maxArchiveSize) {
      throw new Error('Release archive has an invalid size or asset ID.');
    }
    return asset;
  }

  async function githubJson(path, request = fetch, allowMissing = false) {
    const response = await request(`${api}${path}`, {
      headers: { Accept: 'application/vnd.github+json' },
      credentials: 'omit',
      cache: 'no-store',
    });
    if (response.status === 404 && allowMissing) return null;
    if (!response.ok) {
      throw new Error(`GitHub ${response.status}: ${response.status === 403 || response.status === 429
        ? 'API rate limit or access restriction; retry later'
        : response.status === 404 ? 'no published release found' : 'release lookup failed'}.`);
    }
    return response.json();
  }

  async function latestRelease(channel, request = fetch) {
    function validateStable(release) {
      if (release && (release.draft || release.prerelease || !/^v\d+\.\d+\.\d+$/.test(release.tag_name))) {
        throw new Error('The latest GitHub release is not an AudioX stable version.');
      }
      return release;
    }
    if (channel === 'stable') {
      return validateStable(await githubJson('/releases/latest', request));
    }
    if (channel !== 'nightly') throw new Error('Unknown update channel.');
    // Use the rolling release only as an index, then resolve immutable assets.
    const [stable, rolling] = await Promise.all([
      githubJson('/releases/latest', request, true).then(validateStable),
      githubJson('/releases/tags/nightly', request, true),
    ]);
    if (!rolling) {
      if (stable) return stable;
      throw new Error('No published stable or nightly release found.');
    }
    const body = rolling.body || '';
    const tagMatch = /audiox-release-tag:\s*([^\s]+)/.exec(body);
    const runMatch = /audiox-run-number:\s*(\d+)/.exec(body);
    const tag = tagMatch ? tagMatch[1] : runMatch ? `nightly-${runMatch[1]}` : '';
    if (!tag) throw new Error('No versioned nightly is published yet.');
    const release = await githubJson(`/releases/tags/${encodeURIComponent(tag)}`, request);
    if (release.draft || !release.prerelease) throw new Error('The selected nightly is not a published prerelease.');
    return releases.newestRelease(stable, release);
  }

  function releaseVersion(release) {
    const match = /Version:\s*`([^`]+)`/.exec(release?.body || '');
    return match ? match[1] : '';
  }

  async function checkForUpdate(channel, installedVersion, request = fetch) {
    const [latest, history] = await Promise.all([
      latestRelease(channel, request),
      githubJson('/releases?per_page=100', request),
    ]);
    if (!Array.isArray(history)) throw new Error('GitHub returned an invalid release list.');
    return {
      available: releases.updateAvailable(installedVersion, latest, history),
      tag: latest.tag_name,
      version: releaseVersion(latest) || String(latest.tag_name || '').replace(/^v/, ''),
      prerelease: Boolean(latest.prerelease),
    };
  }

  function readStored(storage, key) {
    try {
      return storage ? storage.getItem(key) : null;
    } catch (_) {
      return null;
    }
  }

  function writeStored(storage, key, value) {
    try {
      if (storage) storage.setItem(key, value);
    } catch (_) {
      // Private browsing or full storage only disables caching.
    }
  }

  // Limits GitHub API use to one check per browser every 6 hours (1 hour after a failure).
  async function cachedUpdateCheck({ channel, version, request = fetch, storage, now = Date.now() }) {
    let cached = null;
    try {
      cached = JSON.parse(readStored(storage, noticeKey));
    } catch (_) {
      cached = null;
    }
    if (cached && cached.channel === channel && cached.deviceVersion === version &&
        Number.isFinite(cached.checkedAt) && cached.checkedAt <= now &&
        now - cached.checkedAt < (cached.error ? noticeRetryInterval : noticeInterval)) {
      if (cached.error) throw new Error(cached.error);
      return cached.result;
    }
    const entry = { checkedAt: now, channel, deviceVersion: version };
    try {
      entry.result = await checkForUpdate(channel, version, request);
    } catch (error) {
      entry.error = error.message || String(error);
      writeStored(storage, noticeKey, JSON.stringify(entry));
      throw error;
    }
    writeStored(storage, noticeKey, JSON.stringify(entry));
    return entry.result;
  }

  function noticeVisible(result, storage) {
    return Boolean(result && result.available && result.tag && readStored(storage, dismissedKey) !== result.tag);
  }

  function dismissNotice(result, storage) {
    if (result && result.tag) writeStored(storage, dismissedKey, result.tag);
  }

  async function mirrorArchive(channel, release, board, asset, request = fetch) {
    const response = await request(`${mirror}updates.json`, {
      credentials: 'omit',
      cache: 'no-store',
    });
    if (!response.ok) throw new Error(`Update mirror unavailable (${response.status}); enable GitHub Pages and wait for deployment.`);
    const entry = (await response.json())[release.prerelease ? channel : 'stable'];
    const archive = entry?.assets?.[board];
    if (entry?.tag !== release.tag_name || !archive || archive.size !== asset.size ||
        archive.url !== `updates/${release.tag_name}/${asset.name}`) {
      throw new Error('The update mirror has not deployed this release yet. Retry after the Pages deployment finishes.');
    }
    return new URL(archive.url, mirror).href;
  }

  async function downloadArchive(asset, url, report, request = fetch) {
    const response = await request(url, {
      credentials: 'omit',
      cache: 'no-store',
    });
    if (!response.ok) throw new Error(`GitHub archive download failed (${response.status}).`);
    const chunks = [];
    let received = 0;
    if (response.body) {
      const reader = response.body.getReader();
      try {
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          received += value.byteLength;
          if (received > asset.size || received > maxArchiveSize) {
            throw new Error('Downloaded archive exceeds the expected size.');
          }
          chunks.push(value);
          report(Math.round(received / asset.size * 100));
        }
      } catch (error) {
        await reader.cancel();
        throw error;
      } finally {
        reader.releaseLock();
      }
    } else {
      chunks.push(await response.blob());
    }
    const blob = new Blob(chunks, { type: 'application/octet-stream' });
    if (blob.size !== asset.size) throw new Error('Downloaded archive size does not match GitHub metadata.');
    const magic = await blob.slice(0, 8).text();
    if (magic !== 'AXUPDATE') throw new Error('GitHub returned an invalid AXU archive (possibly a JSON response).');
    return blob;
  }

  function init({ setStatus, ui, showUpdates, storage }) {
    const element = (id) => document.getElementById(id);
    if (storage === undefined) {
      try {
        storage = root.localStorage;
      } catch (_) {
        storage = null;
      }
    }
    const channel = element('update-channel');
    const check = element('btn-update-check');
    const online = element('btn-update-online');
    const manual = element('btn-update-upload');
    const file = element('update_file');
    const progress = element('update-progress');
    const status = element('update-status');
    const summary = element('update-release');
    const link = element('update-download');
    let selected = null;
    let busy = false;

    function message(text, kind = '') {
      status.textContent = ui.formatMessage(text);
      setStatus(text, kind, 'update');
    }
    function controls(locked) {
      busy = locked;
      for (const control of [channel, check, manual, file]) control.disabled = locked;
      for (const id of ['btn-config-backup', 'btn-config-restore', 'backup_file']) {
        const control = element(id);
        if (control) control.disabled = locked;
      }
      online.disabled = locked || !selected || selected.installed || !selected.mirrorUrl;
    }
    function invalidate() {
      selected = null;
      online.disabled = true;
      link.hidden = true;
      link.removeAttribute('href');
      summary.textContent = 'No release checked yet.';
    }
    async function systemInfo() {
      const response = await fetch('/api/system/info', { cache: 'no-store' });
      if (!response.ok) throw new Error(`Cannot identify device (${response.status}).`);
      const info = await response.json();
      if (typeof info.version !== 'string' || !info.version) {
        throw new Error('Device did not report its firmware version.');
      }
      return info;
    }
    async function deviceInfo() {
      const info = await systemInfo();
      return { ...info, board: deviceBoard(info) };
    }

    const banner = element('update-banner');
    const bannerText = element('update-banner-text');
    const bannerView = element('btn-update-banner-view');
    const bannerDismiss = element('btn-update-banner-dismiss');
    const notifications = element('update-notifications');
    const noticeEnabled = Boolean(banner && bannerText && bannerView && bannerDismiss && notifications);
    let notice = null;
    let noticeRun = 0;

    function hideNotice() {
      if (banner) banner.hidden = true;
    }
    function renderNotice() {
      if (!noticeEnabled) return;
      if (!notifications.checked || !noticeVisible(notice, storage)) return hideNotice();
      bannerText.textContent = `AudioX ${notice.version} is available${notice.prerelease ? ' (nightly)' : ''}.`;
      banner.hidden = false;
    }
    async function refreshNotice() {
      if (!noticeEnabled || !notifications.checked) return hideNotice();
      const run = ++noticeRun;
      try {
        const { version } = await systemInfo();
        const result = await cachedUpdateCheck({ channel: channel.value, version, storage });
        if (run !== noticeRun) return;
        notice = result;
        renderNotice();
      } catch (error) {
        if (run === noticeRun) hideNotice();
        if (root.console) root.console.debug(`Update notification check skipped: ${error.message}`);
      }
    }
    async function loadNoticePreference() {
      try {
        const response = await fetch('/api/updates/notifications', { cache: 'no-store' });
        if (response.ok) notifications.checked = (await response.json()).enabled !== false;
      } catch (_) {
        // Older firmware lacks the endpoint; notifications stay enabled.
      }
      await refreshNotice();
    }

    async function upload(blob, name) {
      progress.hidden = false;
      progress.value = 0;
      message(`Uploading ${name} to the Pi...`);
      await new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('PUT', '/api/update');
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        xhr.upload.addEventListener('progress', (event) => {
          if (event.lengthComputable) {
            progress.value = Math.round(event.loaded / event.total * 100);
            message(`Uploading ${name} to the Pi: ${progress.value}%`);
          }
        });
        xhr.upload.addEventListener('load', () => {
          progress.value = 100;
          message('Upload complete; validating and installing...');
        });
        xhr.addEventListener('load', () => {
          const text = (xhr.responseText || '').trim();
          if (xhr.status >= 200 && xhr.status < 300) {
            message('Update installed. Waiting for the device to restart...');
            resolve();
          } else {
            reject(new Error(`Update failed: ${xhr.status} ${text}`));
          }
        });
        xhr.addEventListener('error', () => reject(new Error('Update request failed; the device may be rebooting or unreachable. Check its version before retrying.')));
        xhr.addEventListener('abort', () => reject(new Error('Update upload was aborted.')));
        xhr.send(blob);
      });
    }

    if (readStored(storage, channelKey) === 'nightly' || readStored(storage, channelKey) === 'stable') {
      channel.value = readStored(storage, channelKey);
    }
    channel.addEventListener('change', () => {
      writeStored(storage, channelKey, channel.value);
      invalidate();
      refreshNotice();
    });
    if (noticeEnabled) {
      notifications.checked = true;
      notifications.addEventListener('change', async () => {
        const enabled = notifications.checked;
        notifications.disabled = true;
        try {
          const response = await fetch('/api/updates/notifications', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `enabled=${enabled ? 1 : 0}`,
          });
          if (!response.ok) throw new Error(`${response.status} ${(await response.text()).trim()}`);
          message(enabled ? 'Update notifications enabled.' : 'Update notifications disabled.', 'ok');
          await refreshNotice();
        } catch (error) {
          notifications.checked = !enabled;
          message(`Could not save update notification setting: ${error.message}`, 'warn');
        } finally {
          notifications.disabled = false;
        }
      });
      bannerDismiss.addEventListener('click', () => {
        dismissNotice(notice, storage);
        hideNotice();
      });
      bannerView.addEventListener('click', () => {
        if (showUpdates) showUpdates();
        if (!busy) check.click();
      });
      loadNoticePreference();
    }
    check.addEventListener('click', async () => {
      if (busy) return;
      invalidate();
      controls(true);
      message('Checking device and GitHub release...');
      try {
        await ui.runTask('Checking firmware releases', async () => {
          const info = await deviceInfo();
          const release = await latestRelease(channel.value);
          const asset = selectAsset(release, info.board);
          const installed = !release.prerelease
            ? info.version === release.tag_name.slice(1)
            : /^[a-f0-9]{40}$/i.test(release.target_commitish || '') &&
              info.version.endsWith(`+${release.target_commitish.slice(0, 12)}`);
          selected = { release, asset, board: info.board, installed };
          const fallback = channel.value === 'nightly' && !release.prerelease
            ? ' Using stable; no newer nightly is available.' : '';
          summary.textContent = `Current: ${info.version} (${info.board}). Selected: ${release.tag_name}${releaseVersion(release) ? ` (${releaseVersion(release)})` : ''}, ${(asset.size / 1048576).toFixed(1)} MiB.${fallback}${installed ? ' Already installed.' : ''}`;
          link.href = `https://github.com/${repository}/releases/download/${encodeURIComponent(release.tag_name)}/${asset.name}`;
          link.hidden = false;
          if (!installed) selected.mirrorUrl = await mirrorArchive(channel.value, release, info.board, asset);
          message(installed ? 'This release is already installed.' : 'Release found. Download and install when ready.', 'ok');
        });
      } catch (error) {
        message(`Update check failed: ${error.message}`, 'warn');
      } finally {
        controls(false);
      }
    });
    online.addEventListener('click', async () => {
      if (busy || !selected || selected.installed || !selected.mirrorUrl) return;
      const { release, asset, board, mirrorUrl } = selected;
      controls(true);
      let success = false;
      try {
        if (!await ui.confirm(`Install ${release.tag_name} for ${board}? This may replace the installed version (including downgrades). The device will reboot. Keep this page open until upload finishes.`, {
          title: 'Install firmware?', label: 'Install and reboot',
        })) return;
        await ui.runTask('Installing firmware', async () => {
          // Check the connected hardware again in case the browser now points at another device.
          if ((await deviceInfo()).board !== board) throw new Error('Connected Pi hardware changed; check releases again.');
          progress.hidden = false;
          progress.value = 0;
          message(`Downloading ${asset.name} through this browser...`);
          const blob = await downloadArchive(asset, mirrorUrl, (percent) => {
            progress.value = percent;
            message(`Downloading ${asset.name}: ${percent}%`);
          });
          await upload(blob, asset.name);
        }, { restart: true });
        success = true;
      } catch (error) {
        message(`Online update failed: ${error.message} If the browser blocks GitHub access, use the manual download link and file upload.`, 'warn');
      } finally {
        if (!success) {
          controls(false);
          if (document.activeElement?.matches('body, dialog:not([open]) *')) online.focus();
        }
      }
    });
    manual.addEventListener('click', async () => {
      if (busy) return;
      const archive = file.files[0];
      if (!archive) return message('Pick an update image first.', 'warn');
      if (archive.size < 16 || archive.size > maxArchiveSize) return message('Update archive must be between 16 bytes and 256 MiB.', 'warn');
      controls(true);
      let success = false;
      try {
        if (!await ui.confirm(`Install ${archive.name}? The device will reboot after the update is installed.`, {
          title: 'Install firmware?', label: 'Install and reboot',
        })) return;
        await ui.runTask('Installing firmware', () => upload(archive, archive.name), { restart: true });
        success = true;
      } catch (error) {
        message(error.message, 'warn');
      } finally {
        if (!success) {
          controls(false);
          if (document.activeElement?.matches('body, dialog:not([open]) *')) manual.focus();
        }
      }
    });
  }

  const exported = {
    deviceBoard, selectAsset, latestRelease, releaseVersion, checkForUpdate, cachedUpdateCheck,
    noticeVisible, dismissNotice, mirrorArchive, downloadArchive, init,
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxUpdates = exported;
})(globalThis);
