#!/usr/bin/env bash
set -euo pipefail

OUTPUT="${1:?usage: mirror-updates.sh <output_directory>}"
: "${GH_REPO:?}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUTPUT/updates"
cp "$ROOT/site/index.html" "$ROOT/site/downloads.js" "$ROOT/site/downloads.css" \
    "$ROOT/web/styles.css" "$ROOT/logo.svg" "$ROOT/logo_small.png" "$OUTPUT/"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT

gh api --paginate --slurp "repos/$GH_REPO/releases" | jq 'add' > "$WORK/releases.json"
echo '{}' > "$OUTPUT/updates.json"
for CHANNEL in stable nightly; do
    if [[ "$CHANNEL" == stable ]]; then
        COUNT="$(jq '[.[] | select(.draft == false and .prerelease == false)] | length' "$WORK/releases.json")"
        [[ "$COUNT" == 0 ]] && continue
        gh api "repos/$GH_REPO/releases/latest" > "$WORK/release.json"
    else
        jq '.[] | select(.tag_name == "nightly" and .draft == false)' "$WORK/releases.json" > "$WORK/rolling.json"
        [[ ! -s "$WORK/rolling.json" ]] && continue
        TAG="$(jq -er '
            (.body // "") as $body |
            (try ($body | capture("audiox-release-tag: (?<tag>[^ ]+)").tag) catch
             ($body | capture("audiox-run-number: (?<run>[0-9]+)") | "nightly-" + .run))
        ' "$WORK/rolling.json")"
        gh api "repos/$GH_REPO/releases/tags/$TAG" > "$WORK/release.json"
    fi
    # Older releases remain downloadable on GitHub but may not have update archives.
    COUNT="$(jq '[.assets[] | select(.state == "uploaded" and
        (.name == "audiox-update-pi4.axu" or .name == "audiox-update-pi5.axu"))] | length' "$WORK/release.json")"
    [[ "$COUNT" != 2 ]] && continue
    TAG="$(jq -r .tag_name "$WORK/release.json")"
    if [[ ! "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ &&
          ! "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+-([0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*\.)?nightly\+[0-9]+$ &&
          ! "$TAG" =~ ^nightly-[0-9]+$ ]]; then
        echo "Unsupported release tag for update mirror: $TAG" >&2
        exit 1
    fi
    DIR="$OUTPUT/updates/$TAG"
    mkdir -p "$DIR"
    gh release download "$TAG" --pattern '*.axu' --dir "$DIR"
    gh release download "$TAG" --pattern images.json --output "$WORK/images.json" --clobber
    jq --arg repository "$GH_REPO" \
        '{tag: .tag_name, commit: .target_commitish,
          version: ([(.body // "") | capture("Version: `(?<v>[^`]+)`") | .v] | first // null),
          date: ((.published_at // .created_at) | split("T")[0]),
          release_url: ("https://github.com/" + $repository + "/releases/tag/" + .tag_name),
          assets: {}}' "$WORK/release.json" > "$WORK/channel.json"
    for BOARD in pi4 pi5; do
        NAME="audiox-update-$BOARD.axu"
        SIZE="$(stat -c%s "$DIR/$NAME")"
        EXPECTED="$(jq --arg name "$NAME" '.assets[] | select(.name == $name) | .size' "$WORK/release.json")"
        if [[ "$SIZE" != "$EXPECTED" ]]; then
            echo "Mirror archive size differs from GitHub: $TAG/$NAME" >&2
            exit 1
        fi
        SHA256="$(sha256sum "$DIR/$NAME" | cut -d ' ' -f 1)"
        DIGEST="$(jq -r --arg name "$NAME" '.assets[] | select(.name == $name) | .digest // ""' "$WORK/release.json")"
        if [[ -n "$DIGEST" && "$DIGEST" != "sha256:$SHA256" ]]; then
            echo "Mirror archive checksum differs from GitHub: $TAG/$NAME" >&2
            exit 1
        fi
        jq -e --arg board "$BOARD" --arg url "updates/$TAG/$NAME" --arg sha256 "$SHA256" --argjson size "$SIZE" \
            --arg checksum "https://github.com/$GH_REPO/releases/download/$TAG/$NAME.sha256" \
            --slurpfile images "$WORK/images.json" \
            '.assets[$board] = {url: $url, size: $size, sha256: $sha256, checksum_url: $checksum} |
             ($images[0].os_list | map(select(.devices | index($board + "-64bit"))) |
                 if length == 1 then .[0] else error("Expected exactly one SD image for " + $board) end) as $image |
             .assets[$board].name = $image.name |
             .assets[$board].image = {url: $image.url, size: $image.image_download_size,
                 sha256: $image.image_download_sha256}' \
            "$WORK/channel.json" > "$WORK/new.json"
        mv "$WORK/new.json" "$WORK/channel.json"
    done
    jq --arg channel "$CHANNEL" --slurpfile entry "$WORK/channel.json" \
        '.[$channel] = $entry[0]' "$OUTPUT/updates.json" > "$WORK/new.json"
    mv "$WORK/new.json" "$OUTPUT/updates.json"
done
touch "$OUTPUT/.nojekyll"
