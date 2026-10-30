#!/usr/bin/env bash
set -euo pipefail

FILES="${1:?usage: publish-imager.sh <packaged_files_dir>}"
: "${GH_REPO:?}" "${CHANNEL:?}" "${RELEASE_TAG:?}" "${BUILD_COMMIT:?}" "${SOURCE_REPO:?}"
: "${GITHUB_RUN_NUMBER:?}" "${GITHUB_RUN_ID:?}" "${GITHUB_SERVER_URL:?}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PUBLISH_SEQUENCE="${PUBLISH_SEQUENCE:-$GITHUB_RUN_NUMBER}"
NOTES_COMMIT="${NOTES_COMMIT:-$BUILD_COMMIT}"
NIGHTLY_LIMIT=25
if ! [[ "$CHANNEL" == stable && "$RELEASE_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ||
        "$CHANNEL" == nightly && "$RELEASE_TAG" == "nightly-$PUBLISH_SEQUENCE" ]]; then
    echo "Invalid publishing channel/tag: $CHANNEL/$RELEASE_TAG" >&2
    exit 1
fi

VERSION=""
for BOARD in pi4 pi5; do
    BOARD_VERSION="$(cat "$FILES/$BOARD.version" 2>/dev/null || true)"
    if [[ -z "$BOARD_VERSION" || -n "$VERSION" && "$BOARD_VERSION" != "$VERSION" ]]; then
        echo "Missing or mismatched firmware version: $FILES/$BOARD.version" >&2
        exit 1
    fi
    VERSION="$BOARD_VERSION"
done

WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT

list_releases() {
    gh api --paginate --slurp "repos/$GH_REPO/releases" | jq 'add' > "$WORK/releases.json"
}

release_state() {
    jq --arg tag "$1" '.[] | select(.tag_name == $tag)' "$WORK/releases.json"
}

list_releases
ROLLING="$(release_state nightly)"
if [[ "$CHANNEL" == nightly && -n "$ROLLING" ]]; then
    if ! jq -e '.prerelease or .draft' <<< "$ROLLING" > /dev/null; then
        echo "The nightly tag belongs to a stable release; refusing to overwrite it." >&2
        exit 1
    fi
    NOTES="$(jq -r '.body // ""' <<< "$ROLLING")"
    if [[ "$NOTES" =~ audiox-run-number:\ ([0-9]+) ]] && (( BASH_REMATCH[1] > PUBLISH_SEQUENCE )); then
        echo "A newer nightly has already been published; skipping this older run."
        exit 0
    fi

    # Preserve the last rolling-only build when migrating to versioned nightlies.
    if jq -e '.draft == false and ((.body // "") | contains("audiox-history: true") | not)' \
        <<< "$ROLLING" > /dev/null; then
        LEGACY_TAG="nightly-legacy-$(jq -r .id <<< "$ROLLING")"
        LEGACY="$(release_state "$LEGACY_TAG")"
        if [[ -z "$LEGACY" ]] || jq -e .draft <<< "$LEGACY" > /dev/null; then
            mkdir "$WORK/legacy"
            gh release download nightly --pattern '*.img.xz' --pattern '*.sha256' --dir "$WORK/legacy"
            gh release download nightly --pattern os_list.json --output "$WORK/legacy/old.json"
            jq --arg old "https://github.com/$GH_REPO/releases/download/nightly/" \
                --arg new "https://github.com/$GH_REPO/releases/download/$LEGACY_TAG/" \
                '.os_list |= map(.url |= if startswith($old) then $new + ltrimstr($old) else error("Unexpected legacy image URL") end)' \
                "$WORK/legacy/old.json" > "$WORK/legacy/images.json"
            if [[ -z "$LEGACY" ]]; then
                LEGACY_COMMIT="$(gh api "repos/$GH_REPO/commits/nightly" --jq .sha)"
                gh release create "$LEGACY_TAG" --target "$LEGACY_COMMIT" \
                    --title "AudioX Nightly (legacy)" --draft --prerelease --latest=false
            fi
            gh release upload "$LEGACY_TAG" "$WORK/legacy/"*.img.xz "$WORK/legacy/"*.sha256 \
                "$WORK/legacy/images.json" --clobber
            gh release edit "$LEGACY_TAG" --draft=false --prerelease --latest=false
        fi
    fi
fi

if [[ "$CHANNEL" == stable ]]; then
    TAGGED_COMMIT="$(gh api "repos/$GH_REPO/commits/$RELEASE_TAG" --jq .sha)"
    if [[ "$TAGGED_COMMIT" != "$BUILD_COMMIT" ]]; then
        echo "Release tag moved since this build started; refusing to publish." >&2
        exit 1
    fi
fi

CURRENT="$(release_state "$RELEASE_TAG")"
if [[ -n "$CURRENT" ]]; then
    if ! jq -e --arg commit "$BUILD_COMMIT" --arg channel "$CHANNEL" \
        '.target_commitish == $commit and
         (if $channel == "stable" then .draft else .prerelease end)' <<< "$CURRENT" > /dev/null; then
        echo "Release $RELEASE_TAG already exists and cannot be overwritten." >&2
        exit 1
    fi
else
    FLAGS=(--target "$BUILD_COMMIT" --draft --latest=false)
    if [[ "$CHANNEL" == nightly ]]; then
        FLAGS+=(--prerelease)
    else
        FLAGS+=(--verify-tag)
    fi
    gh release create "$RELEASE_TAG" "${FLAGS[@]}" --title "AudioX v$VERSION"
fi

python3 "$SCRIPT_DIR/release-notes.py" \
    --source "$SOURCE_REPO" \
    --releases "$WORK/releases.json" \
    --channel "$CHANNEL" \
    --current-tag "$RELEASE_TAG" \
    --commit "$NOTES_COMMIT" \
    --repository "$GH_REPO" > "$WORK/changes.md"
{
    cat "$WORK/changes.md"
    printf '\n\n---\n\nVersion: `%s`\n\nBuild commit: `%s`\n\n[Workflow run](%s/%s/actions/runs/%s)\n\n<!-- audiox-run-number: %s -->\n<!-- audiox-history: true -->\n' \
        "$VERSION" "$BUILD_COMMIT" "$GITHUB_SERVER_URL" "$GH_REPO" "$GITHUB_RUN_ID" \
        "$PUBLISH_SEQUENCE"
} > "$WORK/notes.md"

# Published versioned nightly assets are never overwritten, including on reruns.
if [[ -z "$CURRENT" ]] || jq -e .draft <<< "$CURRENT" > /dev/null; then
    for BOARD in pi4 pi5; do
        if [[ ! -s "$FILES/audiox-update-$BOARD.axu" ]]; then
            echo "Missing update archive: $FILES/audiox-update-$BOARD.axu" >&2
            exit 1
        fi
    done
    jq -s '{os_list: .}' "$FILES/pi4.json" "$FILES/pi5.json" > "$WORK/images.json"
    gh release upload "$RELEASE_TAG" "$FILES/"*.img.xz "$FILES/"*.axu "$FILES/"*.sha256 \
        "$WORK/images.json" --clobber
    if [[ "$CHANNEL" == nightly ]]; then
        gh release edit "$RELEASE_TAG" --draft=false --prerelease --latest=false --notes-file "$WORK/notes.md"
    fi
fi

list_releases
jq --arg channel "$CHANNEL" --arg current "$RELEASE_TAG" '
    [.[] | select((.draft == false or .tag_name == $current) and
        any(.assets[]; .name == "images.json" or .name == "os_list.json")) |
        select(if $channel == "nightly" then
            .prerelease and (.tag_name | test("^nightly-([0-9]+|legacy-[0-9]+)$"))
        else
            .prerelease == false and (.tag_name | test("^v[0-9]+\\.[0-9]+\\.[0-9]+$"))
        end)] |
    if $channel == "nightly" then
        sort_by(if .tag_name | test("^nightly-[0-9]+$") then
            .tag_name | ltrimstr("nightly-") | tonumber else 0 end) | reverse
    else sort_by(.published_at // .created_at) | reverse end
' "$WORK/releases.json" > "$WORK/candidates.json"

if [[ "$CHANNEL" == nightly ]]; then
    jq --argjson limit "$NIGHTLY_LIMIT" '.[:$limit]' "$WORK/candidates.json" > "$WORK/retained.json"
else
    cp "$WORK/candidates.json" "$WORK/retained.json"
fi
mkdir "$WORK/manifests"
MANIFESTS=()
while IFS=$'\t' read -r TAG ASSET; do
    gh release download "$TAG" --pattern "$ASSET" --output "$WORK/download.json"
    # Older stable os_list.json assets may already contain history; keep only this release's images.
    jq --arg prefix "https://github.com/$GH_REPO/releases/download/$TAG/" '
        {os_list: [.os_list[] | select(.url | startswith($prefix))]} |
        if (.os_list | length) > 0 then . else error("Release manifest has no matching images") end
    ' "$WORK/download.json" > "$WORK/manifests/$TAG.json"
    MANIFESTS+=("$WORK/manifests/$TAG.json")
    rm "$WORK/download.json"
done < <(jq -r '.[] | [.tag_name,
    (if any(.assets[]; .name == "images.json") then "images.json" else "os_list.json" end)] | @tsv' "$WORK/retained.json")
if (( ${#MANIFESTS[@]} == 0 )); then
    echo "No published Imager builds found for $CHANNEL." >&2
    exit 1
fi
jq -s '{os_list: [.[].os_list[]]}' "${MANIFESTS[@]}" > "$WORK/os_list.json"

if [[ "$CHANNEL" == stable ]]; then
    gh release upload "$RELEASE_TAG" "$WORK/os_list.json" --clobber
    gh release edit "$RELEASE_TAG" --draft=false --prerelease=false --latest --notes-file "$WORK/notes.md"
else
    if [[ -z "$ROLLING" ]]; then
        gh release create nightly --target "$BUILD_COMMIT" --title "AudioX Nightly" \
            --draft --prerelease --latest=false
    fi
    mkdir "$WORK/latest"
    gh release download "$RELEASE_TAG" --pattern '*.img.xz' --pattern '*.axu' --pattern '*.sha256' --dir "$WORK/latest"
    gh release upload nightly "$WORK/latest/"* --clobber
    gh release upload nightly "$WORK/os_list.json" --clobber
    REFS="$(gh api "repos/$GH_REPO/git/matching-refs/tags/nightly" \
        --jq 'map(select(.ref == "refs/tags/nightly")) | length')"
    if [[ "$REFS" == 0 ]]; then
        gh api --method POST "repos/$GH_REPO/git/refs" -f ref=refs/tags/nightly -f sha="$BUILD_COMMIT"
    else
        gh api --method PATCH "repos/$GH_REPO/git/refs/tags/nightly" -f sha="$BUILD_COMMIT" -F force=true
    fi
    gh release edit nightly --draft=false --prerelease --latest=false --notes-file "$WORK/notes.md"

    # Remove expired builds only after the replacement catalog is public.
    while IFS= read -r TAG; do
        gh release delete "$TAG" --yes --cleanup-tag
    done < <(jq -r --argjson limit "$NIGHTLY_LIMIT" '.[$limit:][].tag_name' "$WORK/candidates.json")
fi
