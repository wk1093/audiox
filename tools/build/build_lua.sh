#!/usr/bin/env bash
# Fetch verified official Lua sources; keep native and cross builds in separate roots.
set -euo pipefail

usage="usage: build_lua.sh <version> <commit> <build_root> <cc> <ar>"
VERSION="${1:?${usage}}"
COMMIT="${2:?${usage}}"
BUILD_ROOT="${3:?${usage}}"
CC="${4:?${usage}}"
AR="${5:?${usage}}"
REPOSITORY="${BUILD_ROOT}/downloads/lua.git"
SRC_DIR="${BUILD_ROOT}/build/lua-${VERSION}"

[[ "${COMMIT}" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Error: Lua requires a full pinned Git commit" >&2
    exit 1
}
[[ "${VERSION}" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || {
    echo "Error: invalid Lua version ${VERSION}" >&2
    exit 1
}
MAJOR="${BASH_REMATCH[1]}"
MINOR="${BASH_REMATCH[2]}"
RELEASE="${BASH_REMATCH[3]}"
mkdir -p "${BUILD_ROOT}/downloads" "${BUILD_ROOT}/build" "${BUILD_ROOT}/include" "${BUILD_ROOT}/lib"
if [[ ! -d "${REPOSITORY}" ]]; then
    git init --quiet --bare "${REPOSITORY}"
fi
if ! git --git-dir="${REPOSITORY}" cat-file -e "${COMMIT}^{commit}" 2>/dev/null; then
    git --git-dir="${REPOSITORY}" fetch --depth 1 https://github.com/lua/lua "${COMMIT}"
fi
actual="$(git --git-dir="${REPOSITORY}" rev-parse "${COMMIT}^{commit}")"
if [[ "${actual}" != "${COMMIT}" ]]; then
    echo "Error: Lua source commit mismatch (expected ${COMMIT}, got ${actual})" >&2
    exit 1
fi
rm -rf "${SRC_DIR}"
mkdir -p "${SRC_DIR}"
git --git-dir="${REPOSITORY}" archive "${COMMIT}" | tar -x -C "${SRC_DIR}"
for component in MAJOR MINOR RELEASE; do
    case "${component}" in
        MAJOR) value="${MAJOR}" ;;
        MINOR) value="${MINOR}" ;;
        RELEASE) value="${RELEASE}" ;;
    esac
    if ! grep -Eq "^#define LUA_VERSION_${component}[[:space:]]+\"${value}\"$" "${SRC_DIR}/lua.h"; then
        echo "Error: Lua source does not match version ${VERSION}" >&2
        exit 1
    fi
done
# Generic C99 build: no dlopen/readline or platform-specific standard libraries.
make -s -C "${SRC_DIR}" -j"$(nproc)" liblua.a \
    CC="${CC}" AR="${AR} rc" RANLIB="${AR} s" CFLAGS="-O2 -std=c99"
cp "${SRC_DIR}/lua.h" "${SRC_DIR}/luaconf.h" \
    "${SRC_DIR}/lauxlib.h" "${BUILD_ROOT}/include/"
sed -n '/Copyright (C)/,/^\**\//p' "${SRC_DIR}/lua.h" > "${BUILD_ROOT}/COPYRIGHT"
cp "${SRC_DIR}/liblua.a" "${BUILD_ROOT}/lib/liblua.a.part"
mv "${BUILD_ROOT}/lib/liblua.a.part" "${BUILD_ROOT}/lib/liblua.a"
