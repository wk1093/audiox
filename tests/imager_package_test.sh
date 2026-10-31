#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT

printf 'AudioX test image\n' > "$WORK/audiox.img"
EXTRACT_SHA256="$(sha256sum "$WORK/audiox.img" | cut -d ' ' -f 1)"

# channel|version|expected display version in the Imager/site name
for CASE in \
    "stable|2.0.0|2.0.0" \
    "nightly|1.9.1-nightly+0123456789ab|1.9.1" \
    "nightly|1.10.0-beta.3.nightly+0123456789ab|1.10.0-beta.3"; do
    IFS='|' read -r CHANNEL VERSION DISPLAY <<< "$CASE"
    OUTPUT="$WORK/$CHANNEL-$DISPLAY"
    if [[ "$CHANNEL" == stable ]]; then
        TAG="v2.0.0"
        PREFIX="audiox-latest"
    else
        TAG="v1.10.0-beta.3.nightly+123"
        PREFIX="audiox-latest-nightly"
    fi
    for BOARD in pi4 pi5; do
        bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$OUTPUT" \
            "$BOARD" "$CHANNEL" owner/repo "$TAG" 2026-10-26 "$VERSION"
        NAME="$PREFIX-$BOARD.img.xz"
        xz -dc "$OUTPUT/$NAME" | cmp "$WORK/audiox.img" -
        (cd "$OUTPUT" && sha256sum --check "$NAME.sha256")
        [[ "$(cat "$OUTPUT/$BOARD.version")" == "$VERSION" ]]
        jq -e \
            --arg url "https://github.com/owner/repo/releases/download/$TAG/$NAME" \
            --arg device "$BOARD-64bit" \
            --arg version "$DISPLAY" \
            --arg full_version "$VERSION" \
            --arg channel "$CHANNEL" \
            --arg model "${BOARD#pi}" \
            --arg sha256 "$EXTRACT_SHA256" \
            --arg download_sha256 "$(sha256sum "$OUTPUT/$NAME" | cut -d ' ' -f 1)" \
            --argjson extract_size "$(stat -c%s "$WORK/audiox.img")" \
            --argjson download_size "$(stat -c%s "$OUTPUT/$NAME")" \
            '.url == $url and .devices == [$device] and
             (.name | startswith("AudioX " + $version + " Raspberry Pi " + $model)) and
             (.name | endswith("(" + $channel + ")")) and
             (.description | contains($full_version)) and
             .icon == "https://raw.githubusercontent.com/owner/repo/main/logo_small.png" and
             .extract_sha256 == $sha256 and .extract_size == $extract_size and
             .image_download_sha256 == $download_sha256 and
             .image_download_size == $download_size and
             .release_date == "2026-10-26"' \
            "$OUTPUT/$BOARD.json" > /dev/null
        jq -e --arg name "AudioX $DISPLAY Raspberry Pi ${BOARD#pi} ($CHANNEL)" \
            '.name == $name' "$OUTPUT/$BOARD.json" > /dev/null
        jq -e --arg device "$BOARD-64bit" \
            '.imager.devices | map(select(.tags == [$device])) | length == 1' \
            "$ROOT/os_list.json" > /dev/null
    done
    jq -s '{os_list: .}' "$OUTPUT/pi4.json" "$OUTPUT/pi5.json" \
        | jq -e '.os_list | length == 2' > /dev/null
done

if bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/invalid-board" \
    pi3 nightly owner/repo v1.10.0-beta.3.nightly+123 2026-10-26 1.9.1 > "$WORK/error.log" 2>&1; then
    echo "Expected unsupported board to fail" >&2
    exit 1
fi
grep -q "Unsupported board" "$WORK/error.log"
test ! -e "$WORK/invalid-board"

if bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/invalid-channel" \
    pi4 beta owner/repo beta 2026-10-26 1.9.1 > "$WORK/error.log" 2>&1; then
    echo "Expected unsupported channel to fail" >&2
    exit 1
fi
grep -q "Unsupported channel" "$WORK/error.log"
test ! -e "$WORK/invalid-channel"

for CASE in \
    "stable|1.10.0-beta.3" \
    "stable|1.10.0+0123456789ab" \
    "nightly|1.10.0-beta.3+0123456789ab" \
    "nightly|1.10.0-beta.3.nightly" \
    "nightly|1.10.0-nightly+not-a-commit"; do
    IFS='|' read -r CHANNEL VERSION <<< "$CASE"
    if bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/invalid-version" \
        pi4 "$CHANNEL" owner/repo v1.10.0-beta.3.nightly+123 2026-10-26 "$VERSION" > "$WORK/error.log" 2>&1; then
        echo "Expected $CHANNEL version $VERSION to fail" >&2
        exit 1
    fi
    grep -q "does not match the $CHANNEL channel format" "$WORK/error.log"
    test ! -e "$WORK/invalid-version"
done

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
