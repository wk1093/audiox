#!/usr/bin/env bash
# Fetch verified official Lua sources; keep native and cross builds in separate roots.
set -euo pipefail

usage="usage: build_lua.sh <version> <sha256> <build_root> <cc> <ar> [fetch_command]"
VERSION="${1:?${usage}}"
SHA256="${2:?${usage}}"
BUILD_ROOT="${3:?${usage}}"
CC="${4:?${usage}}"
AR="${5:?${usage}}"
FETCH="${6:-curl -fsSL}"
TARBALL="${BUILD_ROOT}/downloads/lua-${VERSION}.tar.gz"
SRC_DIR="${BUILD_ROOT}/build/lua-${VERSION}"

mkdir -p "${BUILD_ROOT}/downloads" "${BUILD_ROOT}/build" "${BUILD_ROOT}/include" "${BUILD_ROOT}/lib"
if [[ ! -f "${TARBALL}" ]]; then
    ${FETCH} "https://www.lua.org/ftp/lua-${VERSION}.tar.gz" -o "${TARBALL}.part"
    mv "${TARBALL}.part" "${TARBALL}"
fi
actual="$(sha256sum "${TARBALL}" | awk '{print $1}')"
if [[ "${actual}" != "${SHA256}" ]]; then
    echo "Error: Lua archive checksum mismatch (expected ${SHA256}, got ${actual})" >&2
    rm -f "${TARBALL}"
    exit 1
fi
rm -rf "${SRC_DIR}"
tar -xzf "${TARBALL}" -C "${BUILD_ROOT}/build"
# Generic C99 build: no dlopen/readline or platform-specific standard libraries.
make -s -C "${SRC_DIR}/src" -j"$(nproc)" liblua.a \
    CC="${CC}" AR="${AR} rc" RANLIB="${AR} s" CFLAGS="-O2 -std=c99"
cp "${SRC_DIR}/src/lua.h" "${SRC_DIR}/src/luaconf.h" \
    "${SRC_DIR}/src/lauxlib.h" "${BUILD_ROOT}/include/"
cp "${SRC_DIR}/doc/readme.html" "${BUILD_ROOT}/COPYRIGHT.html"
cp "${SRC_DIR}/src/liblua.a" "${BUILD_ROOT}/lib/liblua.a.part"
mv "${BUILD_ROOT}/lib/liblua.a.part" "${BUILD_ROOT}/lib/liblua.a"
