# Releases, versioning, and Imager catalogs

This is a maintainer guide. For installing a release, use
[Getting started](./getting-started); for building without publishing, use
[Development](./development).

## What CI builds

The build workflow builds Pi 4 and Pi 5 separately and uploads board-suffixed
SD images, AXU updates, bootloader initramfs, and runtime initramfs. A successful
Pi 5 build is not physical hardware validation.

Automatic main-branch nightly builds are filtered by image/build inputs:
runtime/header/rootfs/WebUI files, Makefile/Kconfig, module lists, logo assets,
and the build scripts named in the workflow's `push.paths`. Documentation,
tests, catalogs, and publishing-only edits alone do not trigger a firmware
nightly. Mixed commits containing image inputs still do. Update the filter when
adding new image-input locations.

This is input filtering, not binary deduplication: generated archives and embedded
commit IDs can change checksums even when source behavior is unchanged.
Pull-request runs build artifacts without publishing releases.

## Nightly publication

After both board builds succeed, the workflow:

1. Publishes a versioned `nightly-<workflow-run-number>` prerelease with images,
   checksums, and `images.json`.
2. Publishes the updated history catalog.
3. Prunes releases and tags beyond the newest **25 nightly builds**.

Versioned published assets are not overwritten on reruns, and an older workflow
run cannot replace a newer nightly. Failed catalog publication does not prune
history; rerun the workflow to finish it.

The rolling `nightly` release also offers fixed download names, including
`audiox-latest-nightly-pi4.img.xz`, `audiox-latest-nightly-pi5.img.xz`, and
`os_list.json`. Its catalog points at versioned assets. A direct download during
rolling-asset replacement is not atomic; Imager's versioned URLs avoid that
replacement window. Refresh catalogs that refer to pruned nightlies.

## Publish a stable release

1. Open **Actions > Release AudioX Stable > Run workflow**.
2. Select `main` and enter a version such as `2.0.0`, without a `v` prefix.
3. The workflow creates `v2.0.0` at the selected main commit and invokes the
   board builds.
4. Assets are staged in a draft and published as the latest stable after all
   uploads succeed. There is no additional approval step.

Existing tags are reused only when they point to the same commit, never moved.
Use **Re-run failed jobs** to retry the same release. The image-build workflow
can also be manually run for an existing `vX.Y.Z` tag. Pushing a tag alone does
not publish a stable release.

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
node --test tests/site_navigation_test.js tests/docs_site_test.js
```

The shell tests use local mocks and need tools including `jq` and `xz`; they do
not publish a release. See `scripts/package-imager.sh`,
`scripts/publish-imager.sh`, and `scripts/mirror-updates.sh` for the maintained
packaging/publication entry points.
