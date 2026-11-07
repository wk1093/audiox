#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:?build root}"
CC="${2:?C compiler}"
CXX="${3:?C++ compiler}"
REV=2078da43e25a4623cab2d0d60decddf709aaea28
SOURCE="$ROOT/source"
BUILD="$ROOT/build"
for tool in git cmake ninja "$CC" "$CXX"; do
    command -v "$tool" >/dev/null || { echo "Missing LLVM build tool: $tool" >&2; exit 1; }
done
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
cmake --build "$BUILD" --parallel "${LLVM_BUILD_JOBS:-2}" \
    --target LLVMAArch64CodeGen LLVMCore LLVMObject
touch "$ROOT/ready"
