const assert = require('node:assert/strict');
const { test } = require('node:test');
const { compareVersions, newestRelease } = require('../web/releases.js');

test('release precedence follows SemVer rather than dates or lexical ordering', () => {
  for (const [newer, older] of [
    ['1.14.1', '1.12.3-beta.1.nightly+123'],
    ['1.14.1', '1.14.1-beta.1.nightly+123'],
    ['1.14.2-beta.1.nightly+123', '1.14.1'],
    ['1.15.0-beta.10.nightly+123', '1.15.0-beta.2.nightly+456'],
    ['1.10.0', '1.9.9'],
    ['2.0.0', '1.99.99'],
    ['1.0.0-beta.a', '1.0.0-beta.1'],
    ['1.0.0-beta.1', '1.0.0-beta'],
  ]) {
    assert.equal(compareVersions(newer, older), 1);
    assert.equal(compareVersions(older, newer), -1);
  }
  assert.equal(compareVersions('v1.14.1', '1.14.1+build'), 0);
  assert.equal(compareVersions('1.15.0-beta.1.nightly+123', '1.15.0-beta.1.nightly+456'), 0);
  for (const invalid of ['nightly-42', '1.2', '01.2.3', '1.2.3-beta.01', '1.2.3+']) {
    assert.throws(() => compareVersions(invalid, '1.2.3'), /Invalid release version/);
  }
});

test('selection supports catalogs, release notes, legacy nightly tags and missing channels', () => {
  const stable = { tag: 'v1.14.1', date: '2026-10-01' };
  const old = { tag: 'v1.12.3-beta.1.nightly+42', date: '2026-11-01' };
  const newer = { tag_name: 'nightly-43', body: 'Version: `1.15.0-beta.1.nightly+abcdef`' };
  assert.equal(newestRelease(stable, old), stable);
  assert.equal(newestRelease(stable, newer), newer);
  assert.equal(newestRelease(stable, { version: '1.14.1+build' }), stable);
  assert.equal(newestRelease(stable, null), stable);
  assert.equal(newestRelease(null, newer), newer);
  assert.equal(newestRelease(null, null), null);
});
