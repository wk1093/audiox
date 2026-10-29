#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT

KV="6.0.0-test"
KERNEL_DIR="$WORK/out/firmware/modules/$KV/kernel"
for MOD in drivers/common drivers/pi4only drivers/pi5only drivers/notpi5 drivers/both; do
    mkdir -p "$KERNEL_DIR/$(dirname "$MOD")"
    printf 'fake module\n' > "$KERNEL_DIR/$MOD.ko"
done

cat > "$WORK/seeds.txt" <<'EOF'
# comment
base: drivers/common
base(pi5): drivers/pi5only
normal(pi4): drivers/pi4only   # trailing comment
normal(!pi5): drivers/notpi5
normal( pi4 , pi5 ): drivers/both
EOF

run() {
    bash "$ROOT/scripts/resolve_modules.sh" "$KV" "$WORK/out" "$1" module-load modules_staging "$2" >/dev/null
}

expect_list() {
    local list="$1"; shift
    local expected=""
    local mod
    for mod in "$@"; do
        expected+="/lib/modules/$KV/kernel/$mod.ko"$'\n'
    done
    if [[ "$(cat "$WORK/out/$list")"$'\n' != "$expected" ]]; then
        echo "FAIL: $list mismatch" >&2
        echo "expected:" >&2; printf '%s' "$expected" >&2
        echo "actual:" >&2; cat "$WORK/out/$list" >&2
        exit 1
    fi
}

run "$WORK/seeds.txt" pi4
expect_list module-load.base.list drivers/common
expect_list module-load.normal.list drivers/pi4only drivers/notpi5 drivers/both
[[ ! -e "$WORK/out/modules_staging/$KV/kernel/drivers/pi5only.ko" ]]
grep -q '^Board: pi4$' "$WORK/out/module-load.summary.txt"
grep -q 'base(pi5): drivers/pi5only' "$WORK/out/module-load.summary.txt"

run "$WORK/seeds.txt" pi5
expect_list module-load.base.list drivers/common drivers/pi5only
expect_list module-load.normal.list drivers/both
[[ ! -e "$WORK/out/modules_staging/$KV/kernel/drivers/pi4only.ko" ]]

expect_fail() {
    local name="$1" seed="$2" board="$3"
    printf '%s\n' "$seed" > "$WORK/bad.txt"
    if run "$WORK/bad.txt" "$board" 2>/dev/null; then
        echo "FAIL: $name was accepted" >&2
        exit 1
    fi
}

expect_fail "unknown board" "base(pi3): drivers/common" pi4
expect_fail "empty board list" "base(): drivers/common" pi4
expect_fail "unknown prefix" "early: drivers/common" pi4
expect_fail "missing module" "normal(pi4):" pi4
expect_fail "no board given" "base(pi4): drivers/common" ""
expect_fail "invalid board argument" "base: drivers/common" pi9

# QEMU debug build: modules come from a separate tree (the Debian kernel) with
# xz-compressed modules, passed as the optional modules root argument.
QEMU_ROOT="$WORK/qemu-kernel/modules"
for MOD in drivers/common drivers/qemuonly drivers/both; do
    mkdir -p "$QEMU_ROOT/$KV/kernel/$(dirname "$MOD")"
    printf 'fake module\n' | xz -c > "$QEMU_ROOT/$KV/kernel/$MOD.ko.xz"
done
cat > "$WORK/qemu-seeds.txt" <<'EOF'
base: drivers/common
base(!qemu): drivers/pi4only
normal(qemu): drivers/qemuonly
normal(pi4, qemu): drivers/both
EOF
bash "$ROOT/scripts/resolve_modules.sh" "$KV" "$WORK/out" "$WORK/qemu-seeds.txt" module-load modules_staging qemu "$QEMU_ROOT" >/dev/null
expect_list module-load.base.list drivers/common
expect_list module-load.normal.list drivers/qemuonly drivers/both
[[ "$(cat "$WORK/out/modules_staging/$KV/kernel/drivers/qemuonly.ko")" == "fake module" ]]
[[ ! -e "$WORK/out/modules_staging/$KV/kernel/drivers/pi4only.ko" ]]
grep -q '^Board: qemu$' "$WORK/out/module-load.summary.txt"

echo "resolve_modules tests passed"
