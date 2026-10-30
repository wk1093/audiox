#!/usr/bin/env bash
set -euo pipefail

: "${GH_REPO:?}"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT

version="$(make --silent print-version AUDIOX_VERSION_NIGHTLY=0)"
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-([0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*))?$ ]]; then
    echo "::error::Makefile version must be X.Y.Z or X.Y.Z-<prerelease>, without build metadata." >&2
    exit 1
fi
prerelease="${BASH_REMATCH[5]}"
if [[ -n "$prerelease" ]]; then
    IFS=. read -r -a identifiers <<< "$prerelease"
    for identifier in "${identifiers[@]}"; do
        if [[ "$identifier" =~ ^0[0-9]+$ ]]; then
            echo "::error::Numeric prerelease identifiers must not have leading zeroes: $version" >&2
            exit 1
        fi
    done
    channel=nightly
    tag=""
else
    channel=stable
    tag="v$version"
fi

finish() {
    printf 'channel=%s\ntag=%s\npublish=%s\n' "$channel" "$tag" "$1"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        printf '## Release check\n\nMakefile version: `%s`\n\nChannel: `%s`\n\nPublish: `%s`\n' \
            "$version" "$channel" "$1" >> "$GITHUB_STEP_SUMMARY"
    fi
}

gh api --paginate --slurp "repos/$GH_REPO/releases" > "$WORK/pages.json"
jq -e 'add // []' "$WORK/pages.json" > "$WORK/releases.json"

if [[ "$channel" == stable ]]; then
    if jq -e --arg tag "$tag" 'any(.[]; .tag_name == $tag and .draft == false and .prerelease == false)' \
        "$WORK/releases.json" > /dev/null; then
        echo "$tag is already published; bump the Makefile version for another stable release." >&2
        finish false
        exit 0
    fi
    refs="$(gh api "repos/$GH_REPO/git/matching-refs/tags/$tag" \
        --jq "map(select(.ref == \"refs/tags/$tag\")) | length")"
    commit="$(git rev-parse HEAD)"
    if [[ "$refs" != 0 ]]; then
        tagged_commit="$(gh api "repos/$GH_REPO/commits/$tag" --jq .sha)"
        if [[ "$tagged_commit" != "$commit" ]]; then
            echo "::error::$tag points to a different commit. Bump the Makefile version; release tags are never moved." >&2
            exit 1
        fi
    else
        gh api --method POST "repos/$GH_REPO/git/refs" \
            -f ref="refs/tags/$tag" -f sha="$commit" > /dev/null
    fi
    finish true
    exit 0
fi

jq -r '
    map(select(.draft == false and
        (.tag_name | test("^(v[0-9]+\\.[0-9]+\\.[0-9]+|nightly(-[0-9]+|-legacy-[0-9]+)?)$")) and
        any(.assets[]; .name == "images.json" or .name == "os_list.json"))) |
    sort_by(.published_at // .created_at) | reverse | .[].tag_name
' "$WORK/releases.json" > "$WORK/tags"

while IFS= read -r tag; do
    commit="$(git rev-parse --verify "refs/tags/$tag^{commit}")"
    if git merge-base --is-ancestor "$commit" HEAD; then
        # Match the firmware input filter in c-cpp.yml; publishing/docs changes alone do not count.
        if git diff --quiet "$commit" HEAD -- src include root web scripts Makefile Kconfig \
            bootmod.txt depmod.txt ':(top,glob)logo*.png' ':(top,glob)logo*.svg' ':(top,glob)logo*.ppm' \
            ':(exclude)scripts/mirror-updates.sh' ':(exclude)scripts/prepare-release.sh'; then
            echo "No firmware changes since published release $tag." >&2
            tag=""
            finish false
            exit 0
        else
            status=$?
            if (( status != 1 )); then
                echo "Failed to compare firmware inputs against $tag." >&2
                exit "$status"
            fi
        fi
    else
        status=$?
        if (( status != 1 )); then
            echo "Failed to check ancestry of $tag." >&2
            exit "$status"
        fi
    fi
done < "$WORK/tags"

echo "Firmware inputs are not covered by a published release; building a nightly." >&2
tag=""
finish true
