const assert = require('node:assert/strict');
const { test } = require('node:test');
const {
  compareVersions, compareReleases, newestRelease, releaseMatchesVersion, updateAvailable,
} = require('../web/releases.js');

test('version precedence follows SemVer rather than lexical ordering', () => {
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
  const old = { tag: 'v1.12.3-beta.1.nightly+42', date: '2026-09-01' };
  const newer = { tag_name: 'nightly-43', body: 'Version: `1.15.0-beta.1.nightly+abcdef`' };
  assert.equal(newestRelease(stable, old), stable);
  assert.equal(newestRelease(stable, { ...old, date: '2026-11-01' }).tag, old.tag);
  assert.equal(newestRelease(stable, { ...old, date: stable.date }), stable);
  assert.equal(newestRelease(stable, newer), newer);
  assert.equal(newestRelease(stable, { version: '1.14.1+build' }), stable);
  assert.equal(newestRelease(stable, null), stable);
  assert.equal(newestRelease(null, newer), newer);
  assert.equal(newestRelease(null, null), null);
});

const published = (tag, at, extra = {}) => ({
  tag_name: tag, published_at: at, created_at: '2020-01-01T00:00:00Z', draft: false,
  prerelease: tag.includes('-'), ...extra,
});
const beta = published('v1.15.0-beta.1.nightly+50', '2026-05-01T10:00:00Z', {
  body: 'Version: `1.15.0-beta.1.nightly+0123456789ab`',
  target_commitish: '0123456789abcdef0123456789abcdef01234567',
});
const patch = published('v1.14.2', '2026-05-03T10:00:00Z');
const stable = published('v1.14.1', '2026-04-01T10:00:00Z');
const history = [patch, beta, stable, { tag_name: 'nightly', published_at: '2026-06-01T00:00:00Z' }];

test('releases are ordered by GitHub publication date, not version', () => {
  assert.equal(compareReleases(patch, beta), 1);
  assert.equal(compareReleases(beta, patch), -1);
  assert.equal(newestRelease(patch, beta), patch);
  assert.equal(compareReleases({ ...beta, published_at: null, created_at: null }, patch), 1, 'missing dates fall back to SemVer');
  assert.equal(compareReleases({ ...beta, published_at: patch.published_at }, patch), 1, 'equal dates fall back to SemVer');
  assert.equal(compareReleases({ ...stable, published_at: null, created_at: '2026-06-01T00:00:00Z' }, patch), 1);
});

test('installed versions match stable tags, release notes and nightly commits', () => {
  assert.ok(releaseMatchesVersion(stable, '1.14.1'));
  assert.ok(releaseMatchesVersion(stable, 'v1.14.1'));
  assert.ok(!releaseMatchesVersion(stable, '1.14.2'));
  assert.ok(releaseMatchesVersion(beta, '1.15.0-beta.1.nightly+0123456789ab'));
  assert.ok(releaseMatchesVersion({ ...beta, body: '' }, '1.15.0-beta.9.nightly+0123456789ab'));
  assert.ok(!releaseMatchesVersion({ ...beta, body: '', prerelease: false }, '1.15.0-beta.9.nightly+0123456789ab'));
  assert.ok(!releaseMatchesVersion(beta, ''));
});

test('update availability uses release history and treats unknown installs as old', () => {
  // 1.15.0-beta was published before 1.14.2, so 1.14.2 is an update for beta users.
  assert.equal(updateAvailable('1.15.0-beta.1.nightly+0123456789ab', patch, history), true);
  assert.equal(updateAvailable('1.14.1', patch, history), true);
  assert.equal(updateAvailable('1.14.2', patch, history), false);
  // A stable installed after the nightly was published is newer than that nightly.
  assert.equal(updateAvailable('1.14.2', beta, history), false);
  assert.equal(updateAvailable('9.9.9', patch, history), true, 'missing from history means old');
  assert.equal(updateAvailable('1.0.0-dev+local', patch, []), true);
  assert.equal(updateAvailable('1.14.1', patch, [patch, { ...stable, draft: true }]), true);
  assert.equal(updateAvailable('1.14.1', null, history), false);
});
