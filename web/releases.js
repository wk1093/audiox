(function (root) {
  'use strict';

  function parseVersion(version) {
    const match = /^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/.exec(version);
    if (!match) throw new Error(`Invalid release version: ${version}`);
    const prerelease = match[4] ? match[4].split('.') : [];
    if (prerelease.some((id) => /^\d+$/.test(id) && id.length > 1 && id[0] === '0')) {
      throw new Error(`Invalid release version: ${version}`);
    }
    return { core: match.slice(1, 4).map(BigInt), prerelease };
  }

  function compareVersions(left, right) {
    const a = parseVersion(left);
    const b = parseVersion(right);
    for (let i = 0; i < a.core.length; i++) {
      if (a.core[i] !== b.core[i]) return a.core[i] > b.core[i] ? 1 : -1;
    }
    if (!a.prerelease.length || !b.prerelease.length) {
      return Number(!a.prerelease.length) - Number(!b.prerelease.length);
    }
    for (let i = 0; i < Math.max(a.prerelease.length, b.prerelease.length); i++) {
      if (a.prerelease[i] === undefined) return -1;
      if (b.prerelease[i] === undefined) return 1;
      const x = a.prerelease[i];
      const y = b.prerelease[i];
      if (x === y) continue;
      const numericX = /^\d+$/.test(x);
      const numericY = /^\d+$/.test(y);
      if (numericX !== numericY) return numericX ? -1 : 1;
      return (numericX ? BigInt(x) > BigInt(y) : x > y) ? 1 : -1;
    }
    return 0;
  }

  function versionOf(release) {
    const notes = /Version:\s*`([^`]+)`/.exec(release.body || '');
    return release.version || (notes && notes[1]) || release.tag_name || release.tag;
  }

  function releaseDate(release) {
    // GitHub's created_at is the tagged commit's date, so it is only a last resort.
    const time = Date.parse(release?.published_at || release?.date || release?.created_at || '');
    return Number.isFinite(time) ? time : null;
  }

  // Releases are ordered by publication time, so a 1.14.2 published after 1.15.0-beta is newer.
  // SemVer only breaks ties when a date is missing or identical (catalogs record whole days).
  function compareReleases(left, right) {
    const a = releaseDate(left);
    const b = releaseDate(right);
    if (a !== null && b !== null && a !== b) return a > b ? 1 : -1;
    return compareVersions(versionOf(left), versionOf(right));
  }

  function newestRelease(stable, nightly) {
    if (!stable) return nightly;
    if (!nightly) return stable;
    return compareReleases(nightly, stable) > 0 ? nightly : stable;
  }

  function releaseMatchesVersion(release, version) {
    const installed = String(version || '').replace(/^v/, '');
    if (!release || !installed) return false;
    const notes = /Version:\s*`([^`]+)`/.exec(release.body || '');
    for (const candidate of [release.version, notes && notes[1], release.tag_name, release.tag]) {
      if (candidate && String(candidate).replace(/^v/, '') === installed) return true;
    }
    const commit = release.target_commitish || '';
    return Boolean(release.prerelease) && /^[a-f0-9]{40}$/i.test(commit) &&
      installed.endsWith(`+${commit.slice(0, 12)}`);
  }

  // `history` is GitHub's release list. An installed version that is not in the history
  // (deleted release, local build) is treated as older than every release.
  function updateAvailable(installedVersion, latest, history) {
    if (!latest || releaseMatchesVersion(latest, installedVersion)) return false;
    const installed = (Array.isArray(history) ? history : [])
      .filter((release) => release && !release.draft && release.tag_name !== 'nightly' &&
        releaseMatchesVersion(release, installedVersion))
      .reduce((best, release) => (!best || compareReleases(release, best) > 0 ? release : best), null);
    if (!installed) return true;
    return compareReleases(latest, installed) > 0;
  }

  const exported = { compareVersions, compareReleases, newestRelease, releaseMatchesVersion, updateAvailable };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxReleases = exported;
})(globalThis);
