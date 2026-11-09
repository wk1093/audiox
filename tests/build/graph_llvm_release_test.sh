#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT
SCRIPT="$ROOT/tools/build/graph_llvm_release.sh"
export GH_REPO=owner/repo MOCK="$WORK/mock"
mkdir -p "$MOCK" "$WORK/bundle/build/lib" "$WORK/bundle/build/include/llvm/Config" \
    "$WORK/bundle/source/llvm/include/llvm/IR" "$WORK/bundle/source/llvm/lib/Support/BLAKE3"
tag="$(bash "$SCRIPT" tag)"
[[ "$tag" == llvm-21.1.8-aarch64-ubuntu24-gcc13-* ]]
recipe="${tag##*-}"
printf 'revision=2078da43e25a4623cab2d0d60decddf709aaea28\nrecipe=%s\ntarget=aarch64-linux-gnu\n' "$recipe" > "$WORK/bundle/manifest"
for file in source/llvm/include/llvm/IR/Module.h build/include/llvm/Config/llvm-config.h \
    source/llvm/LICENSE.TXT source/llvm/lib/Support/BLAKE3/LICENSE; do
    echo fixture > "$WORK/bundle/$file"
done
for lib in AArch64CodeGen Core Object RuntimeDyld ExecutionEngine Passes; do
    echo fixture > "$WORK/bundle/build/lib/libLLVM$lib.a"
done
archive() {
    tar -czf "$MOCK/audiox-llvm-aarch64.tar.gz" -C "$WORK/bundle" manifest source build
    (cd "$MOCK" && sha256sum audiox-llvm-aarch64.tar.gz > audiox-llvm-aarch64.tar.gz.sha256)
}
gh() {
    case "$1 $2" in
        "api "*)
            if [[ "${MOCK_STATUS:-200}" != 200 ]]; then
                echo "gh: error (HTTP $MOCK_STATUS)" >&2
                return 1
            fi
            echo '{}'
            ;;
        "release download")
            while (($#)); do
                if [[ "$1" == --dir ]]; then
                    cp "$MOCK"/audiox-llvm-aarch64.tar.gz* "$2/"
                    return
                fi
                shift
            done
            return 1
            ;;
        "release create") printf '%s\n' "$*" > "$MOCK/publish" ;;
        *) return 1 ;;
    esac
}
export -f gh
archive
bash "$SCRIPT" fetch "$WORK/installed"
test -f "$WORK/installed/ready"
cmp "$WORK/bundle/manifest" "$WORK/installed/manifest"
if MOCK_STATUS=404 bash "$SCRIPT" fetch "$WORK/missing"; then exit 1; else test "$?" = 3; fi
if MOCK_STATUS=403 bash "$SCRIPT" fetch "$WORK/denied"; then exit 1; else test "$?" = 1; fi
printf corrupt >> "$MOCK/audiox-llvm-aarch64.tar.gz"
if bash "$SCRIPT" fetch "$WORK/corrupt"; then exit 1; fi
test ! -e "$WORK/corrupt/ready"
echo wrong > "$WORK/bundle/manifest"
archive
if bash "$SCRIPT" fetch "$WORK/wrong"; then exit 1; fi
test ! -e "$WORK/wrong/ready"
LLVM_RELEASE_COMMIT=123 bash "$SCRIPT" publish "$MOCK"
grep -Fq -- '--prerelease --latest=false' "$MOCK/publish"
grep -Fq "github.event_name != 'pull_request' && github.event_name != 'pull_request_target'" "$ROOT/.github/workflows/llvm.yml"
grep -Fq 'cancel-in-progress: false' "$ROOT/.github/workflows/llvm.yml"
compiler_rule="$(sed -n '/^$(GRAPH_COMPILER_BIN):/,/^$(HOST_GRAPH_COMPILER):/p' "$ROOT/Makefile")"
grep -Fq '$(GRAPH_LLVM_ROOT)/ready Makefile' <<< "$compiler_rule"
grep -Fq -- '-ffunction-sections -fdata-sections -Wl,--gc-sections' <<< "$compiler_rule"
grep -Fq '$(STRIP) --strip-all $(PROGRAM_STAGE_DIR)/program/init $(PROGRAM_STAGE_DIR)/program/sbin/*' "$ROOT/Makefile"
echo "LLVM release tests passed: install, missing release, API errors, checksum, manifest, and prerelease publication"
