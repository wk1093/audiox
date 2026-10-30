#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT
export GH_REPO=owner/repo SOURCE_REPO="$ROOT"
export BUILD_COMMIT=0123456789abcdef0123456789abcdef01234567
export NOTES_COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
export GITHUB_RUN_NUMBER=27 GITHUB_RUN_ID=123 GITHUB_SERVER_URL=https://github.com
export MOCK="$WORK/github"
mkdir -p "$MOCK/assets" "$WORK/package"
echo '[]' > "$MOCK/releases.json"
printf 'fixture image' > "$WORK/audiox.img"
for board in pi4 pi5; do
    printf 'AXUPDATE fixture %s' "$board" > "$WORK/package/audiox-update-$board.axu"
    (cd "$WORK/package" && sha256sum "audiox-update-$board.axu" > "audiox-update-$board.axu.sha256")
done

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
        elif [[ "$2" == repos/*/releases/latest ]]; then
            jq '[.[] | select(.draft == false and .prerelease == false)] | last' "$MOCK/releases.json"
        elif [[ "$2" == repos/*/releases/tags/* ]]; then
            jq --arg tag "${2##*/}" '.[] | select(.tag_name == $tag)' "$MOCK/releases.json"
        fi
        return
    fi
    local operation="$2" tag="$3"
    shift 3
    local draft=false prerelease=false target="" notes="" title="" arg file
    case "$operation" in
        create)
            while (( $# )); do
                case "$1" in
                    --target) target="$2"; shift ;;
                    --draft) draft=true ;;
                    --prerelease) prerelease=true ;;
                    --title) title="$2"; shift ;;
                esac
                shift
            done
            mkdir -p "$MOCK/assets/$tag"
            jq --arg tag "$tag" --arg commit "$target" --arg title "$title" \
                --argjson draft "$draft" --argjson prerelease "$prerelease" \
                '. + [{id: (length + 1000), tag_name: $tag, target_commitish: $commit,
                draft: $draft, prerelease: $prerelease, name: $title, body: "", assets: [],
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
                jq --arg tag "$tag" --arg name "${file##*/}" --argjson size "$(stat -c%s "$file")" \
                    'map(if .tag_name == $tag then .assets |=
                        ((. + [{name: $name, state: "uploaded", size: $size}]) | unique_by(.name)) else . end)' \
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
            local output="" directory="" matched=false clobber=false destination
            local -a patterns=()
            while (( $# )); do
                case "$1" in
                    --pattern) patterns+=("$2"); shift ;;
                    --output) output="$2"; shift ;;
                    --dir) directory="$2"; shift ;;
                    --clobber) clobber=true ;;
                esac
                shift
            done
            for arg in "${patterns[@]}"; do
                for file in "$MOCK/assets/$tag/"*; do
                    if [[ "${file##*/}" == $arg && -f "$file" ]]; then
                        destination="${output:-$directory/${file##*/}}"
                        if [[ -e "$destination" && "$clobber" != true ]]; then
                            echo "Download destination already exists: $destination" >&2
                            return 1
                        fi
                        cp "$file" "$destination"
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
jq -e '.[] | select(.tag_name == "nightly-27") |
    .name == "AudioX v1.9.1-nightly+0123456789ab"' "$MOCK/releases.json" > /dev/null
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
for board in pi4 pi5; do
    cmp "$WORK/package/audiox-update-$board.axu" "$MOCK/assets/nightly-27/audiox-update-$board.axu"
    cmp "$WORK/package/audiox-update-$board.axu" "$MOCK/assets/nightly/audiox-update-$board.axu"
    (cd "$MOCK/assets/nightly" && sha256sum --check "audiox-update-$board.axu.sha256")
done
echo "PASS: both AXU archives and checksums reach the versioned and rolling nightly releases"

echo '' > "$MOCK/calls"
CHANNEL=nightly RELEASE_TAG=nightly-26 GITHUB_RUN_NUMBER=26 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
! grep -q '^release ' "$MOCK/calls"
echo "PASS: an older run cannot overwrite the latest nightly"

# Failed publication must not prune; a retry must reuse the immutable assets.
for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/package" \
        "$board" nightly "$GH_REPO" nightly-28 2026-10-26 1.10.0-beta.3.nightly+0123456789ab
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
jq -e --arg version "1.10.0-beta.3.nightly+${BUILD_COMMIT:0:12}" \
    '.[] | select(.tag_name == "nightly-28") |
    .body | contains("Version: `" + $version + "`")' "$MOCK/releases.json" > /dev/null
jq -e '.[] | select(.tag_name == "nightly-28") |
    .name == "AudioX v1.10.0-beta.3.nightly+0123456789ab"' "$MOCK/releases.json" > /dev/null
echo "PASS: nightly release titles include the complete firmware version, including prerelease and commit metadata"
cp "$WORK/package/pi5.version" "$WORK/pi5.version.good"
echo '1.10.0-beta.2.nightly+0123456789ab' > "$WORK/package/pi5.version"
echo '' > "$MOCK/calls"
if CHANNEL=nightly RELEASE_TAG=nightly-29 GITHUB_RUN_NUMBER=29 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package" > "$WORK/failure.log" 2>&1; then
    echo "Expected mismatched board versions to fail" >&2; exit 1
fi
grep -q 'mismatched firmware version' "$WORK/failure.log"
! grep -q '^release ' "$MOCK/calls"
mv "$WORK/pi5.version.good" "$WORK/package/pi5.version"
echo "PASS: release notes carry the firmware version; mismatched board versions are rejected"

for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/stable" \
        "$board" stable "$GH_REPO" v2.0.0 2026-10-26 2.0.0
done
cp "$WORK/package/"*.axu "$WORK/package/"*.axu.sha256 "$WORK/stable/"
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
for board in pi4 pi5; do
    cmp "$WORK/stable/audiox-update-$board.axu" "$MOCK/assets/v2.0.0/audiox-update-$board.axu"
done
echo "PASS: both AXU archives are published on stable releases"

GH_REPO="$GH_REPO" bash "$ROOT/scripts/mirror-updates.sh" "$WORK/pages"
jq -e '.stable.tag == "v2.0.0" and .nightly.tag == "nightly-28" and
    .stable.version == "2.0.0" and .nightly.version == "1.10.0-beta.3.nightly+0123456789ab" and
    (.nightly.assets.pi4.name == "AudioX 1.10.0-beta.3 Raspberry Pi 4 (nightly)") and
    (.stable.assets | keys == ["pi4", "pi5"]) and
    (.nightly.assets | keys == ["pi4", "pi5"])' "$WORK/pages/updates.json" > /dev/null
for board in pi4 pi5; do
    cmp "$WORK/stable/audiox-update-$board.axu" "$WORK/pages/updates/v2.0.0/audiox-update-$board.axu"
    cmp "$WORK/package/audiox-update-$board.axu" "$WORK/pages/updates/nightly-28/audiox-update-$board.axu"
    HASH="$(sha256sum "$WORK/stable/audiox-update-$board.axu" | cut -d ' ' -f 1)"
    jq -e --arg board "$board" --arg hash "$HASH" '.stable.assets[$board].sha256 == $hash' \
        "$WORK/pages/updates.json" > /dev/null
    jq -e --arg board "$board" --slurpfile images "$MOCK/assets/v2.0.0/images.json" '
        ($images[0].os_list[] | select(.devices | index($board + "-64bit"))) as $image |
        .stable.assets[$board] as $asset |
        $asset.name == $image.name and $asset.image.url == $image.url and
        $asset.image.size == $image.image_download_size and
        $asset.image.sha256 == $image.image_download_sha256 and
        ($asset.checksum_url | endswith(".axu.sha256"))' "$WORK/pages/updates.json" > /dev/null
done
for file in index.html downloads.css downloads.js; do
    cmp "$ROOT/site/$file" "$WORK/pages/$file"
done
cmp "$ROOT/web/styles.css" "$WORK/pages/styles.css"
cmp "$ROOT/logo.svg" "$WORK/pages/logo.svg"
cmp "$ROOT/logo_small.png" "$WORK/pages/logo_small.png"
[[ -f "$WORK/pages/.nojekyll" ]]
echo "PASS: download site includes styles, logos, matching board images, AXUs and checksums"

cp "$MOCK/assets/v2.0.0/images.json" "$WORK/images-backup.json"
jq '.os_list |= map(select(.devices | index("pi4-64bit")))' "$WORK/images-backup.json" \
    > "$MOCK/assets/v2.0.0/images.json"
if bash "$ROOT/scripts/mirror-updates.sh" "$WORK/missing-image" > "$WORK/failure.log" 2>&1; then
    echo "Expected missing board image metadata to fail deployment" >&2; exit 1
fi
grep -q 'Expected exactly one SD image for pi5' "$WORK/failure.log"
cp "$WORK/images-backup.json" "$MOCK/assets/v2.0.0/images.json"

cp "$MOCK/releases.json" "$WORK/releases-backup.json"
jq 'map(if .tag_name == "v2.0.0" then .assets |= map(.digest = "sha256:invalid") else . end)' \
    "$WORK/releases-backup.json" > "$MOCK/releases.json"
if bash "$ROOT/scripts/mirror-updates.sh" "$WORK/bad-digest" > "$WORK/failure.log" 2>&1; then
    echo "Expected an archive checksum mismatch to fail deployment" >&2; exit 1
fi
grep -q 'Mirror archive checksum differs from GitHub' "$WORK/failure.log"
cp "$WORK/releases-backup.json" "$MOCK/releases.json"
echo "PASS: invalid image metadata or archive digests prevent deployment"

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
bash "$ROOT/scripts/mirror-updates.sh" "$WORK/empty-pages"
jq -e '. == {}' "$WORK/empty-pages/updates.json" > /dev/null
[[ -f "$WORK/empty-pages/index.html" ]]
echo "PASS: download site can deploy before the first release"
CHANNEL=nightly RELEASE_TAG=nightly-27 bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
jq -e '.os_list | length == 2 and all(.[]; .url | contains("/nightly-27/"))' \
    "$MOCK/assets/nightly/os_list.json" > /dev/null
echo "PASS: first nightly publication works without an existing rolling release"

for board in pi4 pi5; do
    bash "$ROOT/scripts/package-imager.sh" "$WORK/audiox.img" "$WORK/package" \
        "$board" nightly "$GH_REPO" nightly-1001 2026-10-26 1.9.1-nightly+0123456789ab
done
CHANNEL=nightly RELEASE_TAG=nightly-1001 PUBLISH_SEQUENCE=1001 GITHUB_RUN_NUMBER=1 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
jq -e '.os_list | length == 4 and (.[0].url | contains("/nightly-1001/"))' \
    "$MOCK/assets/nightly/os_list.json" > /dev/null
jq -e '.[] | select(.tag_name == "nightly") |
    .body | contains("<!-- audiox-run-number: 1001 -->")' "$MOCK/releases.json" > /dev/null
echo '' > "$MOCK/calls"
CHANNEL=nightly RELEASE_TAG=nightly-28 GITHUB_RUN_NUMBER=28 \
    bash "$ROOT/scripts/publish-imager.sh" "$WORK/package"
! grep -q '^release ' "$MOCK/calls"
echo "PASS: run-ID sequences avoid tag collisions and block older push-workflow publications"
echo "Imager publishing tests passed"
