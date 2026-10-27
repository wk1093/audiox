#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT
export GH_REPO=owner/repo BUILD_COMMIT=0123456789abcdef0123456789abcdef01234567
export GITHUB_RUN_NUMBER=27 GITHUB_RUN_ID=123 GITHUB_SERVER_URL=https://github.com
export MOCK="$WORK/github"
mkdir -p "$MOCK/assets" "$WORK/package"
echo '[]' > "$MOCK/releases.json"
printf 'fixture image' > "$WORK/audiox.img"

# Persist simulated release state across the publisher's gh invocations.
gh() {
    printf '%s\n' "$*" >> "$MOCK/calls"
    if [[ "$1" == api ]]; then
        if [[ "$2" == --paginate ]]; then
            jq '[.]' "$MOCK/releases.json"
        elif [[ "$2" == repos/*/commits/* ]]; then
            echo "$BUILD_COMMIT"
        elif [[ "$2" == repos/*/git/matching-refs/* ]]; then
            echo 1
        fi
        return
    fi
    local operation="$2" tag="$3"
    shift 3
    local draft=false prerelease=false target="" notes="" arg file
    case "$operation" in
        create)
            while (( $# )); do
                case "$1" in
                    --target) target="$2"; shift ;;
                    --draft) draft=true ;;
                    --prerelease) prerelease=true ;;
                    --title) shift ;;
                esac
                shift
            done
            mkdir -p "$MOCK/assets/$tag"
            jq --arg tag "$tag" --arg commit "$target" --argjson draft "$draft" --argjson prerelease "$prerelease" \
                '. + [{id: (length + 1000), tag_name: $tag, target_commitish: $commit,
                draft: $draft, prerelease: $prerelease, body: "", assets: [],
                published_at: "2027-01-01", created_at: "2027-01-01"}]' \
                "$MOCK/releases.json" > "$MOCK/new.json"
            mv "$MOCK/new.json" "$MOCK/releases.json"
            ;;
        upload)
            for file in "$@"; do
                [[ "$file" == --clobber ]] && continue
                if [[ "${FAIL_CATALOG:-0}" == 1 && "${file##*/}" == os_list.json ]]; then
                    echo "Simulated catalog upload failure" >&2
                    return 1
                fi
                cp "$file" "$MOCK/assets/$tag/"
                jq --arg tag "$tag" --arg name "${file##*/}" \
                    'map(if .tag_name == $tag then .assets |=
                        ((. + [{name: $name}]) | unique_by(.name)) else . end)' \
                    "$MOCK/releases.json" > "$MOCK/new.json"
                mv "$MOCK/new.json" "$MOCK/releases.json"
            done
            ;;
        edit)
            while (( $# )); do
                case "$1" in
                    --prerelease) prerelease=true ;;
                    --notes-file) notes="$(< "$2")"; shift ;;
                esac
                shift
            done
            jq --arg tag "$tag" --arg notes "$notes" --argjson prerelease "$prerelease" \
                'map(if .tag_name == $tag then .draft = false | .prerelease = $prerelease |
                    .body = $notes else . end)' "$MOCK/releases.json" > "$MOCK/new.json"
            mv "$MOCK/new.json" "$MOCK/releases.json"
            ;;
        download)
            local output="" directory="" matched=false
            local -a patterns=()
            while (( $# )); do
                case "$1" in
                    --pattern) patterns+=("$2"); shift ;;
                    --output) output="$2"; shift ;;
                    --dir) directory="$2"; shift ;;
                esac
                shift
            done
            for arg in "${patterns[@]}"; do
                for file in "$MOCK/assets/$tag/"*; do
                    if [[ "${file##*/}" == $arg && -f "$file" ]]; then
                        cp "$file" "${output:-$directory/${file##*/}}"
                        matched=true
                    fi
                done
            done
            [[ "$matched" == true ]]
            ;;
        delete)
            [[ "$*" == '--yes --cleanup-tag' ]]
            jq --arg tag "$tag" 'map(select(.tag_name != $tag))' "$MOCK/releases.json" > "$MOCK/new.json"
            mv "$MOCK/new.json" "$MOCK/releases.json"
            ;;
        *) echo "Unexpected gh operation: $operation" >&2; return 1 ;;
    esac
}
export -f gh

for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/package" \
        "$board" nightly "$GH_REPO" nightly-27 2026-10-26 1.9.1-nightly+0123456789ab
done
jq -s '{os_list: .}' "$WORK/package/pi4.json" "$WORK/package/pi5.json" > "$WORK/template.json"

seed_release() {
    local tag="$1" channel="$2" manifest="${3:-images.json}"
    local -a flags=()
    [[ "$channel" == nightly ]] && flags+=(--prerelease)
    gh release create "$tag" --target "$BUILD_COMMIT" "${flags[@]}"
    jq --arg tag "$tag" '.os_list |= map(.url |= sub("nightly-27/"; $tag + "/"))' \
        "$WORK/template.json" > "$WORK/$manifest"
    gh release upload "$tag" "$WORK/$manifest" "$WORK/package/"*.img.xz "$WORK/package/"*.sha256
}

for number in $(seq 1 26); do seed_release "nightly-$number" nightly; done
seed_release v1.8.0 stable os_list.json
seed_release v1.9.0 stable
seed_release nightly nightly os_list.json
printf '<!-- audiox-history: true -->\n<!-- audiox-run-number: 26 -->\n' > "$WORK/notes"
gh release edit nightly --prerelease --notes-file "$WORK/notes"
echo '' > "$MOCK/calls"

CHANNEL=nightly RELEASE_TAG=nightly-27 bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
jq -e '[.[] | select(.tag_name | test("^nightly-[0-9]+$"))] | length == 25' "$MOCK/releases.json" > /dev/null
jq -e '.os_list | length == 50 and
    (.[0].url | contains("/nightly-27/")) and (.[-1].url | contains("/nightly-3/"))' \
    "$MOCK/assets/nightly/os_list.json" > /dev/null
test "$(grep -c '^release delete ' "$MOCK/calls")" == 2
grep -q '^release delete nightly-1 --yes --cleanup-tag$' "$MOCK/calls"
grep -q '^release delete nightly-2 --yes --cleanup-tag$' "$MOCK/calls"
EDIT_LINE="$(grep -n '^release edit nightly ' "$MOCK/calls" | cut -d: -f1)"
DELETE_LINE="$(grep -n '^release delete ' "$MOCK/calls" | head -1 | cut -d: -f1)"
(( EDIT_LINE < DELETE_LINE ))
echo "PASS: keeps exactly 25 nightlies, newest first; prunes only after catalog publication"

echo '' > "$MOCK/calls"
CHANNEL=nightly RELEASE_TAG=nightly-26 GITHUB_RUN_NUMBER=26 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
! grep -q '^release ' "$MOCK/calls"
echo "PASS: an older run cannot overwrite the latest nightly"

# Failed publication must not prune; a retry must reuse the immutable assets.
for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/package" \
        "$board" nightly "$GH_REPO" nightly-28 2026-10-26 1.9.1-nightly+0123456789ab
done
echo '' > "$MOCK/calls"
if CHANNEL=nightly RELEASE_TAG=nightly-28 GITHUB_RUN_NUMBER=28 FAIL_CATALOG=1 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package" > "$WORK/failure.log" 2>&1; then
    echo "Expected catalog upload to fail" >&2; exit 1
fi
grep -q 'Simulated catalog upload failure' "$WORK/failure.log"
! grep -q '^release delete ' "$MOCK/calls"
echo '' > "$MOCK/calls"
CHANNEL=nightly RELEASE_TAG=nightly-28 GITHUB_RUN_NUMBER=28 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
! grep -q '^release upload nightly-28 ' "$MOCK/calls"
grep -q '^release delete nightly-3 --yes --cleanup-tag$' "$MOCK/calls"
echo "PASS: failed uploads do not prune history; retries preserve published nightly assets"

for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/stable" \
        "$board" stable "$GH_REPO" v2.0.0 2026-10-26 2.0.0
done
echo '' > "$MOCK/calls"
if CHANNEL=stable RELEASE_TAG=v2.0.0 FAIL_CATALOG=1 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/stable" > "$WORK/failure.log" 2>&1; then
    echo "Expected stable catalog upload to fail" >&2; exit 1
fi
jq -e 'any(.[]; .tag_name == "v2.0.0" and .draft)' "$MOCK/releases.json" > /dev/null
! grep -q '^release delete ' "$MOCK/calls"
CHANNEL=stable RELEASE_TAG=v2.0.0 bash "$ROOT/scripts/publish-imager.sh" "$WORK/stable"
jq -e '.os_list | length == 6 and (.[0].url | contains("/v2.0.0/"))' \
    "$MOCK/assets/v2.0.0/os_list.json" > /dev/null
! grep -q '^release delete ' "$MOCK/calls"
if CHANNEL=stable RELEASE_TAG=v2.0.0 bash "$ROOT/scripts/publish-imager.sh" "$WORK/stable" \
    > "$WORK/failure.log" 2>&1; then
    echo "Expected an existing stable release to be protected" >&2; exit 1
fi
echo "PASS: stable catalog includes all stable releases, including legacy manifests"

# Start with just the original rolling nightly and verify migration preserves it.
echo '[]' > "$MOCK/releases.json"
for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/package" \
        "$board" nightly "$GH_REPO" nightly-27 2026-10-26 1.9.1-nightly+0123456789ab
done
seed_release nightly nightly os_list.json
gh release edit nightly --prerelease
CHANNEL=nightly RELEASE_TAG=nightly-27 bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
jq -e '.os_list | length == 4 and any(.[]; .url | contains("/nightly-legacy-1000/"))' \
    "$MOCK/assets/nightly/os_list.json" > /dev/null
echo "PASS: the existing rolling nightly is archived before replacement"

echo '[]' > "$MOCK/releases.json"
CHANNEL=nightly RELEASE_TAG=nightly-27 bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
jq -e '.os_list | length == 2 and all(.[]; .url | contains("/nightly-27/"))' \
    "$MOCK/assets/nightly/os_list.json" > /dev/null
echo "PASS: first nightly publication works without an existing rolling release"
echo "Imager publishing tests passed"
