const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../web/app.js'), 'utf8');

function fixture() {
  const elements = Object.fromEntries(
    ['sysinfo-storage', 'sysinfo-storage-meter', 'sysinfo-storage-free'].map((id) => [id, {}]),
  );
  const context = vm.createContext({ document: { getElementById: (id) => elements[id] } });
  vm.runInContext(source.slice(source.indexOf('function renderStorageUsage('),
    source.indexOf('async function loadSystemInfo(')), context);
  return { elements, render: context.renderStorageUsage };
}

test('storage meter shows used, total, available and reserved space on a large card', () => {
  const { elements, render } = fixture();
  const gib = 1024 ** 3;
  render({ total_bytes: 120 * gib, used_bytes: 30 * gib, free_bytes: 84 * gib, reserved_bytes: 6 * gib });
  assert.equal(elements['sysinfo-storage'].textContent, '30.0 GiB / 120.0 GiB used');
  assert.equal(elements['sysinfo-storage-free'].textContent, '84.0 GiB free (6.0 GiB reserved)');
  assert.equal(elements['sysinfo-storage-meter'].value, 30);
  assert.equal(elements['sysinfo-storage-meter'].hidden, false);
});

test('small, empty and full filesystems have accurate units and meter endpoints', () => {
  const { elements, render } = fixture();
  const total = 208 * 1024 ** 2;
  render({ total_bytes: total, used_bytes: 0, free_bytes: total, reserved_bytes: 0 });
  assert.equal(elements['sysinfo-storage'].textContent, '0.0 MiB / 208.0 MiB used');
  assert.equal(elements['sysinfo-storage-free'].textContent, '208.0 MiB free');
  assert.equal(elements['sysinfo-storage-meter'].value, 0);
  render({ total_bytes: total, used_bytes: total, free_bytes: 0, reserved_bytes: 0 });
  assert.equal(elements['sysinfo-storage-meter'].value, 100);
});

test('missing or invalid storage clears stale data rather than showing an empty disk', () => {
  const { elements, render } = fixture();
  for (const storage of [null, undefined, {}, { total_bytes: 0 },
    { total_bytes: 100, used_bytes: 50, free_bytes: 100, reserved_bytes: 0 },
    { total_bytes: NaN, used_bytes: 0, free_bytes: 0, reserved_bytes: 0 }]) {
    render(storage);
    assert.equal(elements['sysinfo-storage'].textContent, 'Unavailable');
    assert.equal(elements['sysinfo-storage-meter'].hidden, true);
    assert.equal(elements['sysinfo-storage-free'].textContent, 'Storage usage could not be read.');
  }
});
