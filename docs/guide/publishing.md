# Releases, versioning, and Imager catalogs

This is a maintainer guide. For installing a release, use
[Getting started](./getting-started); for building without publishing, use
[Development](./development).

## What CI builds

The build workflow builds Pi 4 and Pi 5 separately and uploads board-suffixed
SD images, AXU updates, bootloader initramfs, and runtime initramfs. A successful
Pi 5 build is not physical hardware validation.

Main-branch pushes build artifacts but do not publish releases. These builds are
filtered by image/build inputs: runtime/header/rootfs/WebUI files,
Makefile/Kconfig, module lists, logo assets, and the build scripts named in the
workflow's `push.paths`. Pull-request runs also build artifacts without publishing.

The nightly release check compares these same inputs against published releases.
Documentation, tests, catalogs, and mirror-only edits do not create a nightly.
Keep the path filter in the build workflow and `scripts/prepare-release.sh` in sync
when adding new image-input locations. This is source-input filtering, not binary
deduplication: archives and embedded commit IDs can change checksums even when
source behavior is unchanged.

## Automatic release selection

Use **Actions > Release AudioX > Run workflow** on `main` to request a release
without entering a version. The same action runs daily at **08:00 UTC**
(GitHub may delay scheduled runs). It reads `make print-version` from the selected
commit and chooses the channel:

| Makefile version | Publication |
| --- | --- |
| `X.Y.Z` | Full stable release tagged `vX.Y.Z` |
| `X.Y.Z-alpha`, `X.Y.Z-beta.2`, `X.Y.Z-rc.1`, or another SemVer prerelease | Nightly prerelease |

Build metadata in the configured version is rejected; nightly mode adds the
commit metadata automatically. A stable version that is already published is
skipped, even if subsequent commits change firmware. Bump the Makefile version
for another stable release, or set a prerelease suffix to resume nightlies.
Draft stable releases can be retried only from their original tagged commit.

## Nightly publication

For prerelease versions, the action skips building/publishing when the selected
commit's firmware inputs are already covered by a published stable or nightly
release on its Git history. Draft releases do not count, and API/comparison
failures fail the check rather than silently skipping. The first run with no
published baseline builds a nightly.

The firmware version comes from the Makefile with nightly mode enabled. Release
runs are serialized so checks see earlier completed publications instead of
publishing the same firmware again. Stable versions never launch nightly builds.

When unreleased firmware changes exist, both board builds run. After they succeed,
the workflow:

1. Publishes a versioned SemVer nightly prerelease with images,
   checksums, and `images.json`.
2. Publishes the updated history catalog.
3. Prunes releases and tags beyond the newest **25 nightly builds**.

Versioned nightlies use tags such as
`v1.12.0-beta.1.nightly+123456789`, combining the configured prerelease version
with the workflow run ID. The firmware version in the release notes still uses
the build's commit hash, for example `1.12.0-beta.1.nightly+0123456789ab`.
The rolling release keeps its `nightly` tag and `AudioX Nightly` title; its notes
point update clients at the exact versioned tag.

Versioned releases include notes generated from commit subjects since the
appropriate published baseline. Nightlies compare with the newest published
versioned nightly or stable release; stable releases compare with the previous
stable release, so changes already described in nightlies are included again
in the next stable notes. Prefix user-facing commit subjects with `[feat]` or
`[fix]` to group them under Features or Fixes. A commit can contain multiple
markers, such as `[feat] Add MIDI mapping [fix] Preserve old settings`. Other
subjects are retained under Other changes. Each entry links to its commit.

The run ID in each versioned tag makes nightly builds unique and identifies
their workflow run. Old `nightly-N` releases are not included in the new
versioned history catalog and may be deleted; GitHub keeps them until removed.
Versioned published assets are not overwritten on reruns, and an older workflow
run cannot replace a newer nightly. Failed catalog publication does not prune
history; use **Re-run failed jobs** to finish it without repeating the release check.

The rolling `nightly` release also offers fixed download names, including
`audiox-latest-nightly-pi4.img.xz`, `audiox-latest-nightly-pi5.img.xz`, and
`os_list.json`. Its catalog points at versioned assets. A direct download during
rolling-asset replacement is not atomic; Imager's versioned URLs avoid that
replacement window. Refresh catalogs that refer to pruned nightlies.

## Publish a stable release

1. Open **Actions > Release AudioX > Run workflow**, or wait for the daily run.
2. Select `main`. No version input is needed: the workflow reads
   `make print-version` from that commit. First commit the desired
   `AUDIOX_VERSION_MAJOR`, `AUDIOX_VERSION_MINOR`, and `AUDIOX_VERSION_PATCH`
   values in the Makefile, with prerelease/build metadata empty. A prerelease
   suffix instead selects nightly publication.
3. For a Makefile version of `2.0.0`, the workflow creates `v2.0.0` and invokes the
   board builds.
4. Assets are staged in a draft and published as the latest stable after all
   uploads succeed. There is no additional approval step.

Existing tags are reused only when they point to the same commit, never moved.
Use **Re-run failed jobs** to retry the same release. Bump the Makefile version
before releasing a different commit. Pushing a tag alone does not publish a stable
release, and publishing a stable release does not launch a nightly.

Stable assets include `audiox-latest-pi4.img.xz` and
`audiox-latest-pi5.img.xz`. Stable releases with Imager manifests appear in the
Stable history and are not pruned or overwritten.

## Firmware version strings

Versions use SemVer and these Makefile variables:

- `AUDIOX_VERSION_MAJOR`, `AUDIOX_VERSION_MINOR`, `AUDIOX_VERSION_PATCH`.
- `AUDIOX_VERSION_PRERELEASE`, such as `beta.3`, empty for a final version.
- `AUDIOX_VERSION_BUILD`, the metadata after `+`.
- `AUDIOX_VERSION_NIGHTLY`, either `0` or `1`.

Use `make print-version` to inspect the result.
Nightly mode appends `.nightly` to an existing prerelease (or uses `nightly`
alone) and defaults build metadata to the twelve-character Git commit.

| Base configuration | Normal | Nightly example |
| --- | --- | --- |
| `1.10.0` with `beta.3` | `1.10.0-beta.3` | `1.10.0-beta.3.nightly+<commit>` |
| `1.10.1` with no prerelease | `1.10.1` | `1.10.1-nightly+<commit>` |

Stable CI takes `X.Y.Z` from the tag and clears prerelease/build metadata.
Nightly CI keeps the configured prerelease. Packaging rejects versions that do
not match the selected release channel.

Imager/download entry names omit nightly metadata for readability; descriptions,
release notes, and device update checks retain the full firmware version.

## Imager catalogs and permissions

The Imager repository URL is:

```text
https://raw.githubusercontent.com/wk1093/audiox/main/os_list.json
```

The checked-in catalog selects generated Stable and Nightly histories with Pi 4
and Pi 5 choices, hardware tags (`pi4-64bit`, `pi5-64bit`), sizes, icons, and
SHA-256 hashes. Categories become usable after their first successful
publication. Forks must replace repository-specific URLs before publishing.

Publication uses the repository's `GITHUB_TOKEN`. Actions need release-content
and tag-management permissions. Release immutability must be disabled for rolling
asset replacement and nightly pruning. Review repository policy before enabling
the release workflows.

The Pages workflow separately builds the documentation and download/update mirror.
A successful firmware publication and a successful Pages deployment are distinct.
The mirror must retain downloads while adding the generated docs under `docs/`.

## Validate tooling without publishing

```sh
bash tests/imager_package_test.sh
bash tests/imager_publish_test.sh
bash tests/release_workflows_test.sh
python3 tests/release_notes_test.py
node --test tests/site_navigation_test.js tests/docs_site_test.js
```

The shell tests use local mocks and need tools including `jq` and `xz`; they do
not publish a release. See `scripts/package-imager.sh`,
`scripts/publish-imager.sh`, and `scripts/mirror-updates.sh` for the maintained
packaging/publication entry points.
