#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT

printf 'AudioX test image\n' > "$WORK/audiox.img"
EXTRACT_SHA256="$(sha256sum "$WORK/audiox.img" | cut -d ' ' -f 1)"

for CHANNEL in stable nightly; do
    OUTPUT="$WORK/$CHANNEL"
    if [[ "$CHANNEL" == stable ]]; then
        TAG="v2.0.0"
        PREFIX="audiox-latest"
    else
        TAG="nightly"
        PREFIX="audiox-latest-nightly"
    fi
    for BOARD in pi4 pi5; do
        bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$OUTPUT" \
            "$BOARD" "$CHANNEL" owner/repo "$TAG" 2026-10-26
        NAME="$PREFIX-$BOARD.img.xz"
        xz -dc "$OUTPUT/$NAME" | cmp "$WORK/audiox.img" -
        (cd "$OUTPUT" && sha256sum --check "$NAME.sha256")
        jq -e \
            --arg url "https://github.com/owner/repo/releases/download/$TAG/$NAME" \
            --arg device "$BOARD-64bit" \
            --arg sha256 "$EXTRACT_SHA256" \
            --arg download_sha256 "$(sha256sum "$OUTPUT/$NAME" | cut -d ' ' -f 1)" \
            --argjson extract_size "$(stat -c%s "$WORK/audiox.img")" \
            --argjson download_size "$(stat -c%s "$OUTPUT/$NAME")" \
            '.url == $url and .devices == [$device] and
             .icon == "https://raw.githubusercontent.com/owner/repo/main/logo_small.png" and
             .extract_sha256 == $sha256 and .extract_size == $extract_size and
             .image_download_sha256 == $download_sha256 and
             .image_download_size == $download_size and
             .release_date == "2026-10-26"' \
            "$OUTPUT/$BOARD.json" > /dev/null
        jq -e --arg device "$BOARD-64bit" \
            '.imager.devices | map(select(.tags == [$device])) | length == 1' \
            "$ROOT/os_list.json" > /dev/null
    done
    jq -s '{os_list: .}' "$OUTPUT/pi4.json" "$OUTPUT/pi5.json" \
        | jq -e '.os_list | length == 2' > /dev/null
done

if bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/invalid-board" \
    pi3 nightly owner/repo nightly 2026-10-26 > "$WORK/error.log" 2>&1; then
    echo "Expected unsupported board to fail" >&2
    exit 1
fi
grep -q "Unsupported board" "$WORK/error.log"
test ! -e "$WORK/invalid-board"

if bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/invalid-channel" \
    pi4 beta owner/repo beta 2026-10-26 > "$WORK/error.log" 2>&1; then
    echo "Expected unsupported channel to fail" >&2
    exit 1
fi
grep -q "Unsupported channel" "$WORK/error.log"
test ! -e "$WORK/invalid-channel"

jq -e '.os_list | map(.subitems_url) == [
    "https://github.com/wk1093/audiox/releases/latest/download/os_list.json",
    "https://github.com/wk1093/audiox/releases/download/nightly/os_list.json"
]' "$ROOT/os_list.json" > /dev/null

jq -e '
    .imager.latest_version == "1.0.0" and
    .imager.url == "https://github.com/wk1093/audiox-imager" and
    (.imager.devices | length == 2) and
    (.imager.devices | all(
        (.name | length > 0) and
        .icon == "https://cdn.jsdelivr.net/gh/selfhst/icons@main/png/raspberry-pi.png" and
        .capabilities == [] and .matching_type == "exclusive"
    )) and
    (.os_list | all(.icon == "https://raw.githubusercontent.com/wk1093/audiox/main/logo_small.png"))
' "$ROOT/os_list.json" > /dev/null

echo "Imager packaging tests passed"
