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

  function init({ setStatus }) {
    const backup = document.getElementById('btn-config-backup');
    const restore = document.getElementById('btn-config-restore');
    const file = document.getElementById('backup_file');
    const progress = document.getElementById('backup-progress');
    const status = document.getElementById('backup-status');
    let busy = false;

    function message(text, kind = '') {
      status.textContent = text;
      setStatus(text, kind);
    }
    backup.addEventListener('click', () => {
      if (busy) return;
      // Native downloads stream to disk instead of buffering gigabytes in JavaScript.
      window.open('/api/config/backup', '_blank', 'noopener,noreferrer');
      message('Backup download requested. Wait for your browser to finish audiox-backup.axb before reflashing. Server errors appear in the download tab.');
    });
    restore.addEventListener('click', async () => {
      if (busy) return;
      busy = true;
      const controls = Array.from(document.querySelectorAll('button, input, select, textarea'))
        .map((control) => ({ control, disabled: control.disabled }));
      for (const { control } of controls) control.disabled = true;
      let success = false;
      try {
        const archive = file.files[0];
        await validateFile(archive);
        if (!window.confirm(`Restore ${archive.name}? This REPLACES all saved AudioX settings, routing, MIDI/effects, Bluetooth pairings, and soundboard files, then reboots. Files not in the backup are removed. Back up the current device first. Firmware and boot files are not changed.`)) return;
        progress.hidden = false;
        progress.value = 0;
        message('Uploading backup. Keep this page open until validation finishes...');
        await new Promise((resolve, reject) => {
          const xhr = new XMLHttpRequest();
          xhr.open('PUT', '/api/config/restore');
          xhr.setRequestHeader('Content-Type', 'application/octet-stream');
          xhr.upload.addEventListener('progress', (event) => {
            if (event.lengthComputable) {
              progress.value = Math.round(event.loaded / event.total * 100);
              message(`Uploading backup: ${progress.value}%`);
            }
          });
          xhr.upload.addEventListener('load', () => {
            progress.value = 100;
            message('Upload complete. Validating and staging settings and sound files...');
          });
          xhr.addEventListener('load', () => {
            if (xhr.status === 202) {
              message(`${xhr.responseText.trim()} Wait for the device to restart, then reload this page. If it does not restart automatically, reboot it manually.`, 'ok');
              resolve();
            } else {
              reject(new Error(`Restore rejected (${xhr.status}): ${xhr.responseText.trim() || 'no response details'}`));
            }
          });
          xhr.addEventListener('error', () => reject(new Error('Connection lost. The restore may already be staged; check the device and reboot before retrying.')));
          xhr.addEventListener('abort', () => reject(new Error('Restore upload aborted.')));
          xhr.send(archive);
        });
        success = true;
      } catch (error) {
        message(`Backup restore failed: ${error.message}`, 'warn');
      } finally {
        if (!success) {
          for (const { control, disabled } of controls) control.disabled = disabled;
          busy = false;
        }
      }
    });
  }

  const exported = { validateFile, init };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxBackup = exported;
})(globalThis);
