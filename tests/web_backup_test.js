const assert = require('node:assert/strict');
const { test } = require('node:test');
const backup = require('../web/backup.js');
const ui = require('../web/ui.js');

function archive() {
  const bytes = Buffer.alloc(20);
  bytes.write('AXBACKUP');
  bytes.writeUInt32LE(1, 8);
  bytes.writeUInt32LE(3, 12);
  return new File([bytes], 'backup.axb');
}

function controller() {
  const elements = new Map();
  for (const id of ['btn-config-backup', 'btn-config-restore', 'backup_file', 'backup-progress',
    'backup-status', 'previously-disabled', 'other-control']) {
    elements.set(id, {
      listeners: {}, disabled: id === 'previously-disabled', files: [],
      addEventListener(event, callback) { this.listeners[event] = callback; },
    });
  }
  global.document = {
    getElementById: (id) => elements.get(id),
    querySelectorAll: () => Array.from(elements.values()),
  };
  const downloads = [], uploads = [];
  const popup = { ...ui, confirm: async () => true, runTask: async (_title, action) => action() };
  global.window = { open: (...args) => downloads.push(args) };
  let mode = 'success';
  const checks = [];
  global.fetch = async (url, options) => {
    checks.push(url);
    assert.equal(url, '/api/config/restore/check?size=20');
    assert.equal(options.cache, 'no-store');
    return { ok: mode !== 'storage', status: mode === 'storage' ? 507 : 200,
      text: async () => 'insufficient storage for restore: need 110100480 bytes, have 4096 bytes free; no settings changed' };
  };
  global.XMLHttpRequest = class {
    constructor() {
      this.events = {};
      this.progress = {};
      this.upload = { addEventListener: (event, callback) => { this.progress[event] = callback; } };
    }
    open(method, path) {
      assert.equal(method, 'PUT');
      assert.equal(path, '/api/config/restore');
    }
    setRequestHeader(name, value) {
      assert.equal(name, 'Content-Type');
      assert.equal(value, 'application/octet-stream');
    }
    addEventListener(event, callback) { this.events[event] = callback; }
    send(file) {
      uploads.push(file);
      assert.equal(elements.get('other-control').disabled, true);
      this.progress.progress({ lengthComputable: true, loaded: 10, total: 20 });
      assert.equal(elements.get('backup-progress').value, 50);
      this.progress.load();
      this.status = mode === 'success' ? 202 : 400;
      this.responseText = mode === 'success' ? 'Backup validated and staged; rebooting.' : 'Backup checksum mismatch';
      if (mode === 'network') this.events.error();
      else if (mode === 'abort') this.events.abort();
      else this.events.load();
    }
  };
  backup.init({ setStatus: () => {}, ui: popup });
  return {
    elements, downloads, uploads, popup, checks,
    click: (id) => elements.get(id).listeners.click(),
    mode: (value) => { mode = value; },
  };
}

test('backup uses native streaming download instead of buffering sound files', () => {
  const ui = controller();
  ui.click('btn-config-backup');
  assert.deepEqual(ui.downloads, [['/api/config/backup', '_blank', 'noopener,noreferrer']]);
  assert.match(ui.elements.get('backup-status').textContent, /Wait for your browser to finish/);
});

test('restore rejects missing, oversized, truncated and firmware files before upload', async () => {
  await assert.rejects(backup.validateFile(), /Select/);
  await assert.rejects(backup.validateFile({ size: 4 * 1024 ** 3 + 1 }), /4 GiB/);
  await assert.rejects(backup.validateFile(new File(['AXBACKUP'], 'short.axb')), /20 bytes/);
  await assert.rejects(backup.validateFile(new File(['AXUPDATE'.padEnd(20)], 'firmware.axu')), /not be restored/i);
  const ui = controller();
  await ui.click('btn-config-restore');
  assert.equal(ui.uploads.length, 0);
  assert.match(ui.elements.get('backup-status').textContent, /Select an AudioX backup/);
  assert.equal(ui.elements.get('btn-config-restore').disabled, false);
  await backup.validateFile(archive());
});

test('cancelled restore does not change settings or upload anything', async () => {
  const ui = controller();
  ui.elements.get('backup_file').files = [archive()];
  ui.popup.confirm = async (text) => {
    assert.match(text, /REPLACES/);
    assert.match(text, /soundboard/);
    assert.match(text, /reboots/);
    return false;
  };
  await ui.click('btn-config-restore');
  assert.equal(ui.uploads.length, 0);
  assert.equal(ui.elements.get('other-control').disabled, false);
});

test('restore sends the File with progress and locks controls until reboot', async () => {
  const ui = controller();
  const file = archive();
  ui.elements.get('backup_file').files = [file];
  await ui.click('btn-config-restore');
  assert.deepEqual(ui.uploads, [file]);
  assert.equal(ui.checks.length, 1);
  assert.equal(ui.elements.get('backup-progress').value, 100);
  assert.match(ui.elements.get('backup-status').textContent, /validated and staged/);
  assert.equal(ui.elements.get('other-control').disabled, true);
  ui.click('btn-config-backup');
  await ui.click('btn-config-restore');
  assert.equal(ui.downloads.length, 0);
  assert.equal(ui.uploads.length, 1);
});

test('insufficient restore storage is reported before starting the upload', async () => {
  const ui = controller();
  ui.mode('storage');
  ui.elements.get('backup_file').files = [archive()];
  await ui.click('btn-config-restore');
  assert.equal(ui.uploads.length, 0);
  assert.match(ui.elements.get('backup-status').textContent, /507.*insufficient storage.*no settings changed/);
  assert.equal(ui.elements.get('other-control').disabled, false);
});

test('server rejection and connection failures are explicit and recover original disabled states', async () => {
  for (const mode of ['failure', 'network', 'abort']) {
    const ui = controller();
    ui.mode(mode);
    ui.elements.get('backup_file').files = [archive()];
    await ui.click('btn-config-restore');
    assert.match(ui.elements.get('backup-status').textContent, /failed/);
    assert.equal(ui.elements.get('other-control').disabled, false);
    assert.equal(ui.elements.get('previously-disabled').disabled, true);
    if (mode === 'failure') assert.match(ui.elements.get('backup-status').textContent, /checksum mismatch/);
    if (mode === 'network') assert.match(ui.elements.get('backup-status').textContent, /may already be staged/);
  }
});
