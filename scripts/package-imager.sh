#!/usr/bin/env bash
set -euo pipefail

USAGE="usage: package-imager.sh <image> <output_dir> <pi4|pi5> <stable|nightly> <owner/repo> <tag> <YYYY-MM-DD> <version>"
IMAGE="${1:?${USAGE}}"
OUTPUT_DIR="${2:?${USAGE}}"
BOARD="${3:?${USAGE}}"
CHANNEL="${4:?${USAGE}}"
REPOSITORY="${5:?${USAGE}}"
TAG="${6:?${USAGE}}"
RELEASE_DATE="${7:?${USAGE}}"
VERSION="${8:?${USAGE}}"
SEMVER_CORE='(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
PRERELEASE_ID='[0-9A-Za-z-]+'

case "$BOARD" in
    pi4) MODEL="4" ;;
    pi5) MODEL="5 (untested)" ;;
    *) echo "Unsupported board: $BOARD" >&2; exit 1 ;;
esac
case "$CHANNEL" in
    stable) NAME="audiox-latest-$BOARD.img.xz" ;;
    nightly) NAME="audiox-latest-nightly-$BOARD.img.xz" ;;
    *) echo "Unsupported channel: $CHANNEL" >&2; exit 1 ;;
esac
# Stable: X.Y.Z. Nightly: X.Y.Z-[<prerelease>.]nightly+<commit>, e.g. 1.10.0-beta.3.nightly+<commit>.
if [[ "$CHANNEL" == stable && ! "$VERSION" =~ ^${SEMVER_CORE}$ ||
      "$CHANNEL" == nightly && ! "$VERSION" =~ ^${SEMVER_CORE}-(${PRERELEASE_ID}\.)*nightly\+[0-9a-f]{7,40}$ ]]; then
    echo "Version '$VERSION' does not match the $CHANNEL channel format." >&2
    exit 1
fi
# The Imager/site name drops build metadata and the nightly marker; the channel is shown separately.
DISPLAY_VERSION="${VERSION%%+*}"
DISPLAY_VERSION="${DISPLAY_VERSION%.nightly}"
DISPLAY_VERSION="${DISPLAY_VERSION%-nightly}"

mkdir -p "$OUTPUT_DIR"
xz -T2 -6 -c "$IMAGE" > "$OUTPUT_DIR/$NAME"
EXTRACT_SHA256="$(sha256sum "$IMAGE" | cut -d ' ' -f 1)"
DOWNLOAD_SHA256="$(sha256sum "$OUTPUT_DIR/$NAME" | cut -d ' ' -f 1)"
printf '%s  %s\n' "$DOWNLOAD_SHA256" "$NAME" > "$OUTPUT_DIR/$NAME.sha256"
printf '%s\n' "$VERSION" > "$OUTPUT_DIR/$BOARD.version"

jq -n \
    --arg name "AudioX $DISPLAY_VERSION Raspberry Pi $MODEL ($CHANNEL)" \
    --arg description "AudioX $VERSION - $CHANNEL ($TAG), $RELEASE_DATE" \
    --arg url "https://github.com/$REPOSITORY/releases/download/$TAG/$NAME" \
    --arg website "https://github.com/$REPOSITORY" \
    --arg icon "https://raw.githubusercontent.com/$REPOSITORY/main/logo_small.png" \
    --arg device "$BOARD-64bit" \
    --arg date "$RELEASE_DATE" \
    --arg extract_sha256 "$EXTRACT_SHA256" \
    --arg download_sha256 "$DOWNLOAD_SHA256" \
    --argjson extract_size "$(stat -c%s "$IMAGE")" \
    --argjson download_size "$(stat -c%s "$OUTPUT_DIR/$NAME")" \
    '{name: $name, description: $description, url: $url, website: $website, icon: $icon,
      devices: [$device], release_date: $date,
      extract_size: $extract_size, extract_sha256: $extract_sha256,
      image_download_size: $download_size, image_download_sha256: $download_sha256}' \
    > "$OUTPUT_DIR/$BOARD.json"
