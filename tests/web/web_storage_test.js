const assert = require('node:assert/strict');
const { apiContext } = require('./support/api-context.js');
const { test } = require('node:test');
const vm = require('node:vm');

const source = require('./support/app-source.js').appSource();

function fixture() {
  const meter = () => ({
    hidden: true,
    dataset: {},
    attributes: {},
    style: { setProperty(key, value) { this[key] = value; } },
    setAttribute(key, value) { this.attributes[key] = value; },
  });
  const elements = Object.fromEntries(
    ['sysinfo-storage', 'sysinfo-storage-free'].map((id) => [id, {}]),
  );
  elements['sysinfo-storage-meter'] = meter();
  elements['sysinfo-temperature-meter'] = meter();
  const context = apiContext({ document: { getElementById: (id) => elements[id] } });
  vm.runInContext(source.slice(source.indexOf('function renderResourceMeter('),
    source.indexOf('async function loadSystemInfo(')), context);
  return {
    elements,
    render: context.renderStorageUsage,
    renderMeter: context.renderResourceMeter,
    cpuUtilization: context.calculateCpuUtilization,
  };
}

test('storage meter shows used, total, available and reserved space on a large card', () => {
  const { elements, render } = fixture();
  const gib = 1024 ** 3;
  render({ total_bytes: 120 * gib, used_bytes: 30 * gib, free_bytes: 84 * gib, reserved_bytes: 6 * gib });
  assert.equal(elements['sysinfo-storage'].textContent, '30.0 GiB / 120.0 GiB used');
  assert.equal(elements['sysinfo-storage-free'].textContent, '84.0 GiB free (6.0 GiB reserved)');
  assert.equal(elements['sysinfo-storage-meter'].style['--meter-level'], '30%');
  assert.equal(elements['sysinfo-storage-meter'].dataset.state, 'normal');
  assert.equal(elements['sysinfo-storage-meter'].attributes['aria-valuetext'], '30.0% used or reserved');
  assert.equal(elements['sysinfo-storage-meter'].hidden, false);
});

test('small, empty and full filesystems have accurate units and meter endpoints', () => {
  const { elements, render } = fixture();
  const total = 208 * 1024 ** 2;
  render({ total_bytes: total, used_bytes: 0, free_bytes: total, reserved_bytes: 0 });
  assert.equal(elements['sysinfo-storage'].textContent, '0.0 MiB / 208.0 MiB used');
  assert.equal(elements['sysinfo-storage-free'].textContent, '208.0 MiB free');
  assert.equal(elements['sysinfo-storage-meter'].style['--meter-level'], '0%');
  assert.equal(elements['sysinfo-storage-meter'].dataset.state, 'normal');
  render({ total_bytes: total, used_bytes: total, free_bytes: 0, reserved_bytes: 0 });
  assert.equal(elements['sysinfo-storage-meter'].style['--meter-level'], '100%');
  assert.equal(elements['sysinfo-storage-meter'].dataset.state, 'critical');
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

test('resource meter clamps display values and maps thresholds to accessible warning states', () => {
  const { elements, renderMeter } = fixture();
  renderMeter('sysinfo-storage-meter', 82, 100, 70, 90, '82% used');
  assert.equal(elements['sysinfo-storage-meter'].dataset.state, 'warning');
  assert.equal(elements['sysinfo-storage-meter'].attributes['aria-valuenow'], '82');
  assert.equal(elements['sysinfo-storage-meter'].attributes['aria-valuetext'], '82% used');
  const warmColor = elements['sysinfo-storage-meter'].style['--meter-color'];
  renderMeter('sysinfo-storage-meter', 140, 100, 70, 90);
  assert.equal(elements['sysinfo-storage-meter'].style['--meter-level'], '100%');
  assert.equal(elements['sysinfo-storage-meter'].dataset.state, 'critical');
  assert.notEqual(elements['sysinfo-storage-meter'].style['--meter-color'], warmColor);
  renderMeter('sysinfo-storage-meter', NaN, 100, 70, 90);
  assert.equal(elements['sysinfo-storage-meter'].hidden, true);
});

test('temperature colors warm smoothly while keeping warning thresholds accessible', () => {
  const { elements, renderMeter } = fixture();
  renderMeter('sysinfo-temperature-meter', 40, 90, 60, 75);
  const coolColor = elements['sysinfo-temperature-meter'].style['--meter-color'];
  assert.equal(elements['sysinfo-temperature-meter'].dataset.state, 'normal');
  renderMeter('sysinfo-temperature-meter', 65, 90, 60, 75);
  assert.notEqual(elements['sysinfo-temperature-meter'].style['--meter-color'], coolColor);
  assert.equal(elements['sysinfo-temperature-meter'].dataset.state, 'warning');
  renderMeter('sysinfo-temperature-meter', 80, 90, 60, 75);
  assert.equal(elements['sysinfo-temperature-meter'].dataset.state, 'critical');
});

test('CPU utilization is calculated from valid cumulative tick deltas', () => {
  const { cpuUtilization } = fixture();
  assert.equal(cpuUtilization(null, { total_ticks: 100, idle_ticks: 50 }), null);
  assert.equal(cpuUtilization(
    { total_ticks: 100, idle_ticks: 50 },
    { total_ticks: 200, idle_ticks: 100 },
  ), 50);
  assert.equal(cpuUtilization(
    { total_ticks: 100, idle_ticks: 50 },
    { total_ticks: 200, idle_ticks: 50 },
  ), 100);
  assert.equal(cpuUtilization(
    { total_ticks: 100, idle_ticks: 50 },
    { total_ticks: 200, idle_ticks: 150 },
  ), 0);
  for (const current of [
    { total_ticks: 100, idle_ticks: 50 },
    { total_ticks: 200, idle_ticks: 201 },
    { total_ticks: 200, idle_ticks: -1 },
    { total_ticks: Number.MAX_SAFE_INTEGER + 1, idle_ticks: 100 },
    null,
  ]) {
    assert.equal(cpuUtilization({ total_ticks: 100, idle_ticks: 50 }, current), null);
  }
});
