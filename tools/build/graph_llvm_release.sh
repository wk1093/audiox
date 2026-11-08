#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REV=2078da43e25a4623cab2d0d60decddf709aaea28
recipe="$(cat "$SCRIPT_DIR/build_graph_llvm.sh" "$0" | sha256sum | cut -c1-16)"
TAG="llvm-21.1.8-aarch64-ubuntu24-gcc13-$recipe"
ASSET=audiox-llvm-aarch64.tar.gz
manifest() {
    printf 'revision=%s\nrecipe=%s\ntarget=aarch64-linux-gnu\n' "$REV" "$recipe"
}
validate() {
    local root="$1"
    manifest > "$WORK/expected"
    cmp "$WORK/expected" "$root/manifest"
    for file in source/llvm/include/llvm/IR/Module.h \
                build/include/llvm/Config/llvm-config.h \
                source/llvm/LICENSE.TXT source/llvm/lib/Support/BLAKE3/LICENSE \
                build/lib/libLLVMAArch64CodeGen.a build/lib/libLLVMCore.a \
                build/lib/libLLVMObject.a build/lib/libLLVMRuntimeDyld.a \
                build/lib/libLLVMExecutionEngine.a build/lib/libLLVMPasses.a; do
        [[ -s "$root/$file" ]] || { echo "Incomplete LLVM bundle: $file" >&2; exit 1; }
    done
}

mode="${1:?mode: tag, package, fetch, publish}"
if [[ "$mode" == tag ]]; then printf '%s\n' "$TAG"; exit 0; fi
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT
case "$mode" in
package)
    root="${2:?LLVM build root}"
    destination="${3:?archive directory}"
    [[ -f "$root/ready" ]]
    [[ "$(git -C "$root/source" rev-parse HEAD)" == "$REV" ]]
    mkdir -p "$WORK/bundle/build/lib" "$WORK/bundle/build/include" \
        "$WORK/bundle/source/llvm/lib/Support/BLAKE3" "$destination"
    cp -a "$root/source/llvm/include" "$WORK/bundle/source/llvm/"
    cp -a "$root/build/include/." "$WORK/bundle/build/include/"
    cp "$root"/build/lib/libLLVM*.a "$WORK/bundle/build/lib/"
    cp "$root/source/llvm/LICENSE.TXT" "$WORK/bundle/source/llvm/"
    cp "$root/source/llvm/lib/Support/BLAKE3/LICENSE" "$WORK/bundle/source/llvm/lib/Support/BLAKE3/"
    for file in regex_impl.h regstrlcpy.c regfree.c regutils.h regex2.h regexec.c regerror.c regcomp.c \
                xxhash.cpp UnicodeNameToCodepointGenerated.cpp MD5.cpp SHA256.cpp ConvertUTF.cpp SHA1.cpp; do
        cp "$root/source/llvm/lib/Support/$file" "$WORK/bundle/source/llvm/lib/Support/"
    done
    manifest > "$WORK/bundle/manifest"
    validate "$WORK/bundle"
    tar -czf "$destination/$ASSET" -C "$WORK/bundle" manifest source build
    (cd "$destination" && sha256sum "$ASSET" > "$ASSET.sha256")
    ;;
fetch)
    root="${2:?LLVM destination}"
    : "${GH_REPO:?}"
    if ! gh api "repos/$GH_REPO/releases/tags/$TAG" > "$WORK/release.json" 2> "$WORK/error"; then
        if grep -Fq '(HTTP 404)' "$WORK/error"; then
            echo "LLVM release $TAG missing; source build required." >&2
            exit 3
        fi
        cat "$WORK/error" >&2
        exit 1
    fi
    gh release download "$TAG" --repo "$GH_REPO" --pattern "$ASSET" \
        --pattern "$ASSET.sha256" --dir "$WORK"
    # Accept only the expected checksum filename, not arbitrary paths.
    [[ "$(wc -l < "$WORK/$ASSET.sha256")" == 1 ]]
    grep -Eq "^[0-9a-f]{64}  $ASSET\$" "$WORK/$ASSET.sha256"
    (cd "$WORK" && sha256sum -c "$ASSET.sha256")
    tar -tzf "$WORK/$ASSET" > "$WORK/entries"
    tar -tvzf "$WORK/$ASSET" > "$WORK/types"
    if grep -Eq '^[^-d]' "$WORK/types"; then
        echo "LLVM archive must contain only regular files and directories." >&2
        exit 1
    fi
    while IFS= read -r entry; do
        case "$entry" in
            /*|../*|*/../*|*/..) echo "Unsafe LLVM archive path: $entry" >&2; exit 1 ;;
            manifest|source/*|build/*) ;;
            *) echo "Unexpected LLVM archive path: $entry" >&2; exit 1 ;;
        esac
    done < "$WORK/entries"
    mkdir "$WORK/bundle"
    tar -xzf "$WORK/$ASSET" -C "$WORK/bundle" --no-same-owner --no-same-permissions
    validate "$WORK/bundle"
    mkdir -p "$root"
    cp -a "$WORK/bundle/." "$root/"
    touch "$root/ready"
    echo "Installed verified LLVM release $TAG"
    ;;
publish)
    directory="${2:?archive directory}"
    : "${GH_REPO:?}" "${LLVM_RELEASE_COMMIT:?}"
    gh release create "$TAG" "$directory/$ASSET" "$directory/$ASSET.sha256" \
        --repo "$GH_REPO" --target "$LLVM_RELEASE_COMMIT" --prerelease \
        --latest=false --title "AudioX LLVM 21.1.8 AArch64 ($recipe)" \
        --notes "Build dependency, not AudioX firmware. LLVM revision: $REV. Recipe: $recipe. Ubuntu 24.04/GCC 13, Release, static AArch64 libraries. Includes public/generated headers and license notices. Do not replace these assets; recipe changes use a new tag."
    ;;
*) echo "Unknown LLVM release mode: $mode" >&2; exit 1 ;;
esac
