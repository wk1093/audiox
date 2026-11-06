(function (root) {
  'use strict';

  const maxArchiveSize = 4 * 1024 * 1024 * 1024;

  async function validateFile(file) {
    if (!file) throw new Error('Select an AudioX backup first.');
    if (file.size < 20 || file.size > maxArchiveSize) throw new Error('Backup must be between 20 bytes and 4 GiB.');
    const header = await file.slice(0, 16).arrayBuffer();
    if (header.byteLength !== 16 ||
        String.fromCharCode(...new Uint8Array(header, 0, 8)) !== 'AXBACKUP' ||
        new DataView(header).getUint32(8, true) !== 1) {
      throw new Error('Not a supported AudioX .axb backup. Firmware .axu files cannot be restored here.');
    }
  }

  function init({ setStatus, ui }) {
    const backup = document.getElementById('btn-config-backup');
    const restore = document.getElementById('btn-config-restore');
    const file = document.getElementById('backup_file');
    const progress = document.getElementById('backup-progress');
    const status = document.getElementById('backup-status');
    let busy = false;

    function message(text, kind = '') {
      status.textContent = ui.formatMessage(text);
      setStatus(text, kind, 'backup');
    }
    backup.addEventListener('click', () => {
      if (busy) return;
      root.AudioxApi.download('/api/config/backup');
      message('Backup download requested. Wait for your browser to finish audiox-backup.axb before reflashing. Server errors appear in the download tab.');
    });
    restore.addEventListener('click', async () => {
      if (busy) return;
      busy = true;
      const controls = Array.from(document.querySelectorAll('main button, main input, main select, main textarea'))
        .map((control) => ({ control, disabled: control.disabled }));
      for (const { control } of controls) control.disabled = true;
      let success = false;
      try {
        const archive = file.files[0];
        await validateFile(archive);
        if (!await ui.confirm(`Restore ${archive.name}? This REPLACES all saved AudioX settings, routing, MIDI/effects, Bluetooth pairings, and soundboard files, then reboots. Files not in the backup are removed. Back up the current device first. Firmware and boot files are not changed.`, {
          title: 'Replace configuration?', label: 'Restore and reboot',
        })) return;
        await ui.runTask('Restoring configuration', async () => {
          message('Checking available restore storage...');
          const check = await apiFetch(`/api/config/restore/check?size=${archive.size}`, { cache: 'no-store' });
          if (!check.ok) {
            const details = (await check.text()).trim();
            throw new Error(`Restore preflight rejected (${check.status}): ${details || 'no response details'}`);
          }
          progress.hidden = false;
          progress.value = 0;
          message('Uploading backup. Keep this page open until validation finishes...');
          let result;
          try {
            result = await root.AudioxApi.upload('/api/config/restore', archive, {
              onProgress(loaded, total) {
                progress.value = Math.round(loaded / total * 100);
                message(`Uploading backup: ${progress.value}%`);
              },
              onSent() {
                progress.value = 100;
                message('Upload complete. Validating and staging settings and sound files...');
              },
            });
          } catch (error) {
            throw new Error(error.reason === 'abort' ? 'Restore upload aborted.'
              : 'Connection lost. The restore may already be staged; check the device and reboot before retrying.');
          }
          if (result.status !== 202) {
            throw new Error(`Restore rejected (${result.status}): ${result.text.trim() || 'no response details'}`);
          }
          message('Backup validated and staged. Waiting for the device to restart...');
        }, { restart: true });
        success = true;
      } catch (error) {
        message(`Backup restore failed: ${error.message}`, 'warn');
      } finally {
        if (!success) {
          for (const { control, disabled } of controls) control.disabled = disabled;
          busy = false;
          if (document.activeElement?.matches('body, dialog:not([open]) *')) restore.focus();
        }
      }
    });
  }

  const exported = { validateFile, init };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxBackup = exported;
})(globalThis);
