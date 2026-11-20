#!/usr/bin/env bash
set -euo pipefail

# Recursive recipes run in non-building Make modes too.
if [[ "${MAKEFLAGS:-}" =~ ^[^[:space:]-]*q ]]; then exit 1; fi
if [[ "${MAKEFLAGS:-}" =~ ^[^[:space:]-]*[nt] ]]; then exit 0; fi

ROOT="${1:?build root}"
CC="${2:?C compiler}"
CXX="${3:?C++ compiler}"
REV=2078da43e25a4623cab2d0d60decddf709aaea28
SOURCE="$ROOT/source"
BUILD="$ROOT/build"
for tool in git cmake ninja "$CC" "$CXX"; do
    command -v "$tool" >/dev/null || { echo "Missing LLVM build tool: $tool" >&2; exit 1; }
done
build_jobs=()
if [[ -n "${LLVM_BUILD_JOBS:-}" ]]; then
    [[ "$LLVM_BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || {
        echo "LLVM_BUILD_JOBS must be a positive integer" >&2; exit 1;
    }
    build_jobs=(--parallel "$LLVM_BUILD_JOBS")
else
    # Ninja 1.13+ shares GNU Make 4.4+'s FIFO jobserver unless -j is supplied.
    ninja_version="$(ninja --version)"
    IFS=. read -r ninja_major ninja_minor _ <<< "$ninja_version"
    if [[ "${MAKEFLAGS:-}" == *"--jobserver-auth=fifo:"* ]] &&
       (( ninja_major > 1 || (ninja_major == 1 && ninja_minor >= 13) )); then
        unset CMAKE_BUILD_PARALLEL_LEVEL
    else
        # Older Ninja cannot share tokens; honor Make's limit rather than its
        # own CPU-count default. Standalone builds use the available CPU count.
        jobs="$(nproc)"
        if [[ "${MAKELEVEL:-0}" != 0 ]]; then
            jobs=1
            read -ra make_flags <<< "${MAKEFLAGS:-}"
            for flag in "${make_flags[@]}"; do
                [[ "$flag" != -- ]] || break
                case "$flag" in
                    -j) jobs="$(nproc)" ;;
                    -j[0-9]*) jobs="${flag#-j}" ;;
                esac
            done
        fi
        build_jobs=(--parallel "$jobs")
    fi
fi
if [[ ! -f "$SOURCE/llvm/CMakeLists.txt" ]]; then
    mkdir -p "$SOURCE"
    git -C "$SOURCE" init
    git -C "$SOURCE" sparse-checkout init --cone
    git -C "$SOURCE" sparse-checkout set llvm cmake third-party
    git -C "$SOURCE" fetch --depth 1 --filter=blob:none https://github.com/llvm/llvm-project.git "$REV"
    git -C "$SOURCE" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$SOURCE" rev-parse HEAD)" == "$REV" ]] || {
    echo "LLVM source revision mismatch" >&2; exit 1;
}
cmake -S "$SOURCE/llvm" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-gnu \
    -DLLVM_HOST_TRIPLE=aarch64-linux-gnu -DLLVM_ENABLE_PROJECTS= \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF
cmake --build "$BUILD" "${build_jobs[@]}" \
    --target LLVMAArch64CodeGen LLVMCore LLVMObject LLVMRuntimeDyld LLVMExecutionEngine LLVMPasses
touch "$ROOT/ready"
