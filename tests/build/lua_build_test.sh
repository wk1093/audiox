#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:?Lua version}"
COMMIT="${2:?Lua commit}"
CACHE="${3:?Lua source cache}"
CC="${4:?host compiler}"
AR="${5:?host archiver}"
WORK="$(mktemp -d)"
trap 'find "$WORK" -type f -delete; find "$WORK" -depth -type d -empty -delete' EXIT
trap 'echo "FAIL: line $LINENO: $BASH_COMMAND" >&2' ERR

mkdir -p "$WORK/lua/downloads" "$WORK/bin"
cp -a "$CACHE" "$WORK/lua/downloads/lua.git"
export LUA_TEST_GIT
LUA_TEST_GIT="$(command -v git)"
cat > "$WORK/bin/git" <<'EOF'
#!/usr/bin/env bash
for argument in "$@"; do
    if [[ "$argument" == fetch ]]; then
        echo "FAIL: cached Lua build attempted a network fetch" >&2
        exit 1
    fi
done
exec "$LUA_TEST_GIT" "$@"
EOF
chmod +x "$WORK/bin/git"
export PATH="$WORK/bin:$PATH"

bash "$ROOT/tools/build/build_lua.sh" "$VERSION" "$COMMIT" "$WORK/lua" "$CC" "$AR"
test -s "$WORK/lua/lib/liblua.a"
grep -Fq 'Permission is hereby granted' "$WORK/lua/COPYRIGHT"
grep -Fq 'THE SOFTWARE IS PROVIDED "AS IS"' "$WORK/lua/COPYRIGHT"
cmp "$WORK/lua/include/lua.h" "$WORK/lua/build/lua-$VERSION/lua.h"
echo "PASS: mirror sources build offline from a pinned cache and stage headers/license"

if bash "$ROOT/tools/build/build_lua.sh" "$VERSION" main "$WORK/lua" "$CC" "$AR" > "$WORK/error" 2>&1; then
    echo "FAIL: unpinned Lua source accepted" >&2
    exit 1
fi
grep -Fq 'requires a full pinned Git commit' "$WORK/error"
if bash "$ROOT/tools/build/build_lua.sh" 0.0.0 "$COMMIT" "$WORK/lua" "$CC" "$AR" > "$WORK/error" 2>&1; then
    echo "FAIL: mismatched Lua version accepted" >&2
    exit 1
fi
grep -Fq 'does not match version' "$WORK/error"
echo "PASS: unpinned sources and mismatched Lua versions fail explicitly"
