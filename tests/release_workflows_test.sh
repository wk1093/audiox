#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT
trap 'echo "FAIL: line $LINENO: $BASH_COMMAND" >&2' ERR
export GH_REPO=owner/repo GITHUB_RUN_ID=123456789
FIXTURE=0

gh() {
    printf '%s\n' "$*" >> "$MOCK/calls"
    if [[ "${MOCK_API_FAIL:-0}" == 1 ]]; then
        echo "API unavailable" >&2
        return 1
    fi
    case "$*" in
        "api --paginate --slurp "*) cat "$MOCK/releases.json" ;;
        *git/matching-refs/*) echo "${MOCK_REFS:-0}" ;;
        "api repos/owner/repo/commits/"*) echo "${MOCK_TAG_COMMIT:-}" ;;
        "api --method POST "*) echo '{}' ;;
        *) echo "Unexpected gh call: $*" >&2; return 1 ;;
    esac
}
export -f gh

commit() {
    git add Makefile src docs scripts
    git -c commit.gpgsign=false commit --quiet --allow-empty -m "Fixture change"
}

fixture() {
    FIXTURE=$((FIXTURE + 1))
    export MOCK="$WORK/fixture-$FIXTURE"
    mkdir -p "$MOCK"
    cd "$MOCK"
    git init --quiet --initial-branch=main
    git config user.email test@example.invalid
    git config user.name "Release test"
    mkdir src docs scripts
    printf 'print-version:\n\t@printf "%%s\\n" "%s"\n' "${1:-2.0.0-beta.2}" > Makefile
    echo firmware > src/firmware.cpp
    echo docs > docs/guide.md
    echo mirror > scripts/mirror-updates.sh
    commit
    echo '[[]]' > releases.json
}

release() {
    local tag="$1" options="${2:-}"
    [[ -n "$options" ]] || options='{}'
    git tag "$tag"
    jq --arg tag "$tag" --argjson options "$options" '
        .[0] += [({
            tag_name: $tag, draft: false, prerelease: ($tag | startswith("nightly")),
            assets: [{name: "images.json"}], published_at: "2026-10-29T08:00:00Z"
        } + $options)]
    ' releases.json > new.json
    mv new.json releases.json
}

expect_output() {
    printf '%s\n' "$1" > expected
    bash "$ROOT/scripts/prepare-release.sh" > actual
    diff -u expected actual
}

expect_nightly() {
    expect_output "$(printf 'channel=nightly\ntag=\npublish=%s' "$1")"
}

expect_stable() {
    expect_output "$(printf 'channel=stable\ntag=v2.0.0\npublish=%s' "$1")"
}

expect_failure() {
    if bash "$ROOT/scripts/prepare-release.sh" > actual 2> error.log; then
        echo "Expected release preparation to fail" >&2
        exit 1
    fi
    [[ ! -s actual ]]
    [[ -s error.log ]]
}

fixture
expect_nightly true
release nightly-42
expect_nightly false
echo "new docs" > docs/guide.md
echo "new mirror" > scripts/mirror-updates.sh
echo "publishing only" > scripts/prepare-release.sh
commit
expect_nightly false
echo "changed firmware" > src/firmware.cpp
commit
expect_nightly true
echo "PASS: nightlies skip published firmware and non-firmware edits, but detect firmware changes"

fixture
release v1.9.0
expect_nightly false
echo "PASS: published stable firmware suppresses duplicate nightlies"

fixture
release nightly-42 '{"draft": true}'
release misc-tools
release nightly-43 '{"assets": []}'
expect_nightly true
echo "PASS: drafts, unrelated releases, and missing manifests do not suppress nightlies"

fixture
release v1.9.0 '{"published_at": "2026-10-30T08:00:00Z"}'
echo "changed firmware" > src/firmware.cpp
commit
release nightly-42
expect_nightly false
echo "PASS: all published baselines are checked, not just the most recently published"

fixture
original="$(git rev-parse HEAD)"
git checkout --quiet -b other
echo "other branch" > src/firmware.cpp
commit
release nightly-42
git checkout --quiet main
[[ "$(git rev-parse HEAD)" == "$original" ]]
expect_nightly true
echo "PASS: releases on another branch are not a firmware baseline"

fixture 2.0.0
expect_stable true
grep -Eq -- '-f ref=refs/tags/v2\.0\.0 -f sha=[a-f0-9]{40}' calls
release v2.0.0
echo "later change without a version bump" > src/firmware.cpp
commit
expect_stable false
echo "PASS: final versions create stable tags and published versions are skipped"

fixture 2.0.0
release v2.0.0 '{"draft": true}'
MOCK_REFS=1 MOCK_TAG_COMMIT="$(git rev-parse HEAD)" expect_stable true
MOCK_REFS=1 MOCK_TAG_COMMIT=different-commit expect_failure
grep -q "release tags are never moved" error.log
echo "PASS: unfinished stable releases reuse only their original commit"

for version in 2.0.0-alpha 2.0.0-alpha.1 2.0.0-beta 2.0.0-rc.1; do
    fixture "$version"
    expect_nightly true
    echo "PASS: $version selects nightly"
done

for version in 02.0.0 2.0 2.0.0-beta.01 2.0.0-beta..1 2.0.0+build; do
    fixture "$version"
    expect_failure
    grep -q "::error::" error.log
    echo "PASS: $version is rejected"
done

fixture
MOCK_API_FAIL=1 expect_failure
echo "not json" > releases.json
expect_failure
echo '[[]]' > releases.json
release nightly-42
git tag --delete nightly-42 > /dev/null
expect_failure
echo "PASS: API failures, malformed responses, and missing release tags fail explicitly"

RELEASE="$ROOT/.github/workflows/release.yml"
BUILD="$ROOT/.github/workflows/c-cpp.yml"
grep -q '^  workflow_dispatch:$' "$RELEASE"
grep -q '^  schedule:$' "$RELEASE"
grep -Fq -- "- cron: '0 8 * * *'" "$RELEASE"
! grep -Eq 'inputs:|inputs\.version' "$RELEASE"
grep -Fq 'fetch-depth: 0' "$RELEASE"
grep -Fq "if: needs.prepare.outputs.publish == 'true'" "$RELEASE"
grep -Fq 'channel: ${{ needs.prepare.outputs.channel }}' "$RELEASE"
grep -Fq 'tag: ${{ needs.prepare.outputs.tag }}' "$RELEASE"
! grep -q 'workflow_dispatch:' "$BUILD"
grep -Fq "if: needs.prepare.outputs.channel != '' && github.ref == 'refs/heads/main'" "$BUILD"
grep -Fq 'PUBLISH_SEQUENCE: ${{ github.run_id }}' "$BUILD"
grep -Fq 'tag=nightly-$GITHUB_RUN_ID' "$BUILD"
for name in "Package Imager image" "Upload Imager publishing files"; do
    grep -F -A1 -- "- name: $name" "$BUILD" |
        grep -Fq "if: needs.prepare.outputs.channel != ''"
done
echo "PASS: workflow wiring separates artifact builds from explicit publication"

fixture
awk '
    /- name: Select publishing channel/ { selected = 1 }
    selected && /run: \|/ { body = 1; next }
    body && /^      - name:/ { exit }
    body { sub(/^          /, ""); print }
' "$BUILD" > selection.sh
[[ -s selection.sh ]]
export GITHUB_OUTPUT="$MOCK/output"
for event in push pull_request workflow_dispatch schedule; do
    for channel in '' nightly stable; do
        : > "$GITHUB_OUTPUT"
        tag=""
        [[ "$channel" != stable ]] || tag=v2.0.0
        EVENT_NAME="$event" CHANNEL="$channel" RELEASE_TAG="$tag" bash -e selection.sh
        case "$channel" in
            stable) printf 'major=2\nminor=0\npatch=0\nchannel=stable\ntag=v2.0.0\n' > expected ;;
            nightly) printf 'channel=nightly\ntag=nightly-123456789\n' > expected ;;
            '') : > expected ;;
        esac
        diff -u expected "$GITHUB_OUTPUT"
    done
done
echo "PASS: build preparation honors the requested channel regardless of caller event"

cd "$ROOT"
[[ "$(make --silent print-version AUDIOX_VERSION_MAJOR=2 AUDIOX_VERSION_MINOR=3 \
    AUDIOX_VERSION_PATCH=4 AUDIOX_VERSION_PRERELEASE= AUDIOX_VERSION_BUILD= \
    AUDIOX_VERSION_NIGHTLY=0)" == 2.3.4 ]]
version="$(make --silent print-version AUDIOX_VERSION_PRERELEASE=beta.2 AUDIOX_VERSION_NIGHTLY=1)"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+-beta\.2\.nightly\+[a-f0-9]{12}$ ]]
echo "PASS: Makefile provides stable and nightly versions"
echo "Release workflow tests passed"
