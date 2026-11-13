#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
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
    bash "$ROOT/tools/build/resolve_modules.sh" "$KV" "$WORK/out" "$1" module-load modules_staging "$2" >/dev/null
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
bash "$ROOT/tools/build/resolve_modules.sh" "$KV" "$WORK/out" "$WORK/qemu-seeds.txt" module-load modules_staging qemu "$QEMU_ROOT" >/dev/null
expect_list module-load.base.list drivers/common
expect_list module-load.normal.list drivers/qemuonly drivers/both
[[ "$(cat "$WORK/out/modules_staging/$KV/kernel/drivers/qemuonly.ko")" == "fake module" ]]
[[ ! -e "$WORK/out/modules_staging/$KV/kernel/drivers/pi4only.ko" ]]
grep -q '^Board: qemu$' "$WORK/out/module-load.summary.txt"

# Exercise the shipped seeds, not just board-filter syntax. Model the supplied
# Pi kernel's built-in RP1 PWM provider and compressed fan module.
FAN_ROOT="$WORK/fan-modules"
while IFS= read -r MOD; do
    [[ "$MOD" == drivers/pwm/pwm-rp1 ]] && continue
    mkdir -p "$FAN_ROOT/$KV/kernel/$(dirname "$MOD")"
    printf 'fake module\n' > "$FAN_ROOT/$KV/kernel/$MOD.ko"
done < <(sed -E 's/#.*//; s/^(base|normal|auto)(\([^)]*\))?:[[:space:]]*//; s/\*/fake/g; /^[[:space:]]*$/d' \
    "$ROOT/config/modules/bootmod.txt" "$ROOT/config/modules/depmod.txt" | sort -u)
printf 'kernel/drivers/pwm/pwm-rp1.ko\n' > "$FAN_ROOT/$KV/modules.builtin"
printf 'alias of:N*T*Cbrcm,brcmstb-i2c i2c_brcmstb\n' > "$FAN_ROOT/$KV/modules.alias"
xz "$FAN_ROOT/$KV/kernel/drivers/hwmon/pwm-fan.ko"

for SEEDS in bootmod depmod; do
    for BOARD in pi4 pi5 qemu; do
        bash "$ROOT/tools/build/resolve_modules.sh" "$KV" "$WORK/out" "$ROOT/config/modules/$SEEDS.txt" \
            module-load modules_staging "$BOARD" "$FAN_ROOT" >/dev/null
        FAN_PATH="/lib/modules/$KV/kernel/drivers/hwmon/pwm-fan.ko"
        if [[ "$BOARD" == pi5 ]]; then
            grep -Fxq "$FAN_PATH" "$WORK/out/module-load.base.list"
            [[ "$(cat "$WORK/out/modules_staging/$KV/kernel/drivers/hwmon/pwm-fan.ko")" == "fake module" ]]
        else
            ! grep -Fq "$FAN_PATH" "$WORK/out/module-load.list"
            [[ ! -e "$WORK/out/modules_staging/$KV/kernel/drivers/hwmon/pwm-fan.ko" ]]
        fi
        ! grep -Fq '/drivers/pwm/pwm-rp1.ko' "$WORK/out/module-load.list"
    done
done

rm "$FAN_ROOT/$KV/kernel/drivers/hwmon/pwm-fan.ko.xz"
for SEEDS in bootmod depmod; do
    if bash "$ROOT/tools/build/resolve_modules.sh" "$KV" "$WORK/out" "$ROOT/config/modules/$SEEDS.txt" \
        module-load modules_staging pi5 "$FAN_ROOT" >"$WORK/missing-fan.log" 2>&1; then
        echo "FAIL: $SEEDS accepted a Pi 5 image without the fan driver" >&2
        exit 1
    fi
    grep -Fq 'module not found for drivers/hwmon/pwm-fan' "$WORK/missing-fan.log"
done

# Hardware-loaded (auto) seeds: staged with their metadata but not listed.
AUTO_ROOT="$WORK/auto-modules"
for MOD in drivers/gpu/drm/vc4 drivers/gpu/drm/panel/panel-simple drivers/input/ts/edt-ft5x06 drivers/listed drivers/other/x; do
    mkdir -p "$AUTO_ROOT/$KV/kernel/$(dirname "$MOD")"
    printf 'fake module\n' > "$AUTO_ROOT/$KV/kernel/$MOD.ko"
done
cat > "$AUTO_ROOT/$KV/modules.alias" <<'EOF'
# Aliases extracted from modules themselves.
alias of:N*T*Cbrcm,bcm2711-vc5 vc4
alias of:N*T*Cpanel-simple panel_simple
alias i2c:edt-ft5406 edt_ft5x06
alias of:N*T*Cother x
EOF
cat > "$AUTO_ROOT/$KV/modules.softdep" <<'EOF'
softdep vc4 pre: snd-soc-hdmi-codec
softdep x pre: y
EOF
cat > "$WORK/auto-seeds.txt" <<'EOF'
auto(pi4): drivers/gpu/drm/*
auto: drivers/input/ts/edt-ft5x06
auto(pi5): drivers/other/*
normal: drivers/listed
normal: drivers/gpu/drm/vc4
EOF
bash "$ROOT/tools/build/resolve_modules.sh" "$KV" "$WORK/out" "$WORK/auto-seeds.txt" module-load modules_staging pi4 "$AUTO_ROOT" >/dev/null
STAGE="$WORK/out/modules_staging/$KV"
[[ ! -s "$WORK/out/module-load.base.list" ]]
expect_list module-load.normal.list drivers/listed drivers/gpu/drm/vc4
[[ -f "$STAGE/kernel/drivers/gpu/drm/panel/panel-simple.ko" ]]
[[ -f "$STAGE/kernel/drivers/input/ts/edt-ft5x06.ko" ]]
[[ ! -e "$STAGE/kernel/drivers/other/x.ko" ]]
grep -Fxq 'alias of:N*T*Cpanel-simple panel_simple' "$STAGE/modules.alias"
grep -Fxq 'alias i2c:edt-ft5406 edt_ft5x06' "$STAGE/modules.alias"
grep -Fxq 'alias of:N*T*Cbrcm,bcm2711-vc5 vc4' "$STAGE/modules.alias"
! grep -q ' x$' "$STAGE/modules.alias"
grep -Fxq 'kernel/drivers/gpu/drm/panel/panel-simple.ko:' "$STAGE/modules.dep"
grep -Fxq 'kernel/drivers/listed.ko:' "$STAGE/modules.dep"
grep -Fxq 'softdep vc4 pre: snd-soc-hdmi-codec' "$STAGE/modules.softdep"
! grep -q 'softdep x' "$STAGE/modules.softdep"
[[ ! -e "$STAGE/modules.alias.bin" && ! -e "$STAGE/modules.dep.bin" ]]
grep -q '^Hardware-Loaded (auto) Count: 2$' "$WORK/out/module-load.summary.txt"
grep -q 'auto-seed  ] drivers/gpu/drm/panel/panel-simple' "$WORK/out/module-load.summary.txt"

expect_auto_fail() {
    local name="$1" seed="$2"
    printf '%s\n' "$seed" > "$WORK/bad.txt"
    if bash "$ROOT/tools/build/resolve_modules.sh" "$KV" "$WORK/out" "$WORK/bad.txt" module-load modules_staging pi4 "$AUTO_ROOT" >/dev/null 2>&1; then
        echo "FAIL: $name was accepted" >&2
        exit 1
    fi
}
expect_auto_fail "glob matching nothing" "auto: drivers/nothing/*"
expect_auto_fail "glob in base seed" "base: drivers/gpu/*"
expect_auto_fail "glob in normal seed" "normal: drivers/gpu/*"
rm "$AUTO_ROOT/$KV/modules.alias"
expect_auto_fail "auto seed without modules.alias" "auto: drivers/listed"

echo "resolve_modules tests passed"

