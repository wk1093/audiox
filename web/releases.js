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

  function newestRelease(stable, nightly) {
    if (!stable) return nightly;
    if (!nightly) return stable;
    return compareVersions(versionOf(nightly), versionOf(stable)) > 0 ? nightly : stable;
  }

  const exported = { compareVersions, newestRelease };
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  else root.AudioxReleases = exported;
})(globalThis);
