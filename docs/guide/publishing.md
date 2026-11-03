# Releases and Imager catalogs

This guide covers release maintenance. Use [Getting started](./getting-started)
to install AudioX and [Development](./development) to build without publishing.

## Build workflow

CI builds Pi 4 and Pi 5 separately. Main-branch and pull-request builds produce
firmware artifacts but do not publish releases. The nightly check runs daily at
08:00 UTC and can also be started from **Actions > Release AudioX > Run
workflow** on `main`.

The workflow compares firmware inputs selected by its `push.paths` filter.
Documentation, tests, Imager catalogs, and mirror-only edits do not trigger a
nightly. Keep the workflow filter and `scripts/prepare-release.sh` aligned when
adding firmware inputs.

## Select a release channel

The workflow reads `make print-version` from the selected commit:

| Version | Channel |
| --- | --- |
| `X.Y.Z` | Stable, tagged `vX.Y.Z` |
| SemVer prerelease such as `X.Y.Z-beta.2` | Nightly prerelease |

Set `AUDIOX_VERSION_MAJOR`, `AUDIOX_VERSION_MINOR`, and
`AUDIOX_VERSION_PATCH` in the Makefile. Leave prerelease and build metadata
empty for Stable. Set `AUDIOX_VERSION_PRERELEASE` for Nightly. A stable version
already published is skipped; use a new version for another Stable release.
Configured build metadata is rejected because the workflow supplies it.

## Publish a Stable release

1. Commit the intended version in the Makefile.
2. Select `main` and run **Actions > Release AudioX > Run workflow**, or wait
   for the scheduled run.
3. CI builds both boards and stages images, checksums, manifests, and AXU
   archives in a draft release.
4. After uploads succeed, the workflow publishes the release as the latest
   Stable.

The tag must identify the same commit when a release is retried. Use
**Re-run failed jobs** for a failed publication; do not move a published tag.
Stable downloads include `audiox-latest-pi4.img.xz` and
`audiox-latest-pi5.img.xz`.

## Nightly releases

Nightly versions append `.nightly` to a configured prerelease and use the
workflow run ID as tag metadata, for example
`v1.12.0-beta.1.nightly+123456789`. The firmware version uses the commit hash,
for example `1.12.0-beta.1.nightly+0123456789ab`.

The workflow publishes versioned prereleases and refreshes the rolling
`nightly` release. It retains the latest **25 versioned Nightly builds**.
Versioned assets are not overwritten on reruns, and an older run cannot replace
a newer Nightly. The rolling release includes fixed download names such as
`audiox-latest-nightly-pi4.img.xz`, `audiox-latest-nightly-pi5.img.xz`, and
`os_list.json`; its catalog points to versioned assets.

Release notes use commit subjects and group subjects prefixed with `[feat]` or
`[fix]`. Use clear subjects for user-facing changes; document test-only and
documentation-only commits plainly.

## Version format

AudioX uses SemVer. The Makefile settings are:

- `AUDIOX_VERSION_MAJOR`, `AUDIOX_VERSION_MINOR`, `AUDIOX_VERSION_PATCH`
- `AUDIOX_VERSION_PRERELEASE`, such as `beta.3`, or empty for a final version
- `AUDIOX_VERSION_BUILD`, the metadata after `+`
- `AUDIOX_VERSION_NIGHTLY`, `0` or `1`

Inspect the effective version with `make print-version`. Nightly mode appends
`.nightly` to a prerelease, or uses `nightly` when none is set; build metadata
defaults to the 12-character Git commit.

| Configuration | Normal | Nightly |
| --- | --- | --- |
| Version `1.10.0`, prerelease `beta.3` | `1.10.0-beta.3` | `1.10.0-beta.3.nightly+<commit>` |
| Version `1.10.1`, no prerelease | `1.10.1` | `1.10.1-nightly+<commit>` |

Stable packaging takes `X.Y.Z` from the tag and clears prerelease/build
metadata. Nightly packaging retains the configured prerelease.

## Imager catalogs and permissions

The AudioX Imager repository is
`https://raw.githubusercontent.com/wk1093/audiox/main/os_list.json`. The
catalog provides Stable and Nightly entries for Pi 4 and Pi 5, with board tags,
image sizes, icons, and SHA-256 hashes. Forks must replace repository-specific
URLs before publication.

Release workflows use `GITHUB_TOKEN` with release-content and tag-management
permissions. Repository policy must allow replacement of rolling assets and
Nightly pruning. The Pages workflow separately builds the documentation and
download/update mirror; firmware publication does not deploy Pages.

## Validate release tooling

These tests use local mocks and do not publish a release:

```sh
bash tests/imager_package_test.sh
bash tests/imager_publish_test.sh
bash tests/release_workflows_test.sh
python3 tests/release_notes_test.py
node --test tests/site_navigation_test.js tests/docs_site_test.js
```

The shell tests require tools including `jq` and `xz`. See
`scripts/package-imager.sh`, `scripts/publish-imager.sh`, and
`scripts/mirror-updates.sh` for the maintained entry points.
