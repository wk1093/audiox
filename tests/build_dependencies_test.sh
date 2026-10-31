#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'find "$WORK" -type f -delete; find "$WORK" -depth -type d -empty -delete' EXIT
trap 'echo "FAIL: line $LINENO: $BASH_COMMAND" >&2' ERR
cd "$ROOT"

grep -Fxq 'include $(CURDIR)/tests/Makefile' Makefile
grep -Fxq '.DEFAULT_GOAL := all' Makefile
! grep -Eq '^test-[[:alnum:]_-]+:' Makefile
targets=(
    test-audio-engine test-midi-usb test-midi-script test-midi-docs
    test-display-hotplug test-soundboard-config test-boot-recovery test-config-backup
    test-audio-pcm-recover test-system-logs test-system test-gadget-setup test-mdns
    test-gadget-volume test-audio-runtime test-diagnostics test-watchdog
    test-resolve-modules test-dnsmasq-config test-audio-meters test-audio-graph
    test-audio-graph-mailbox test-audio-graph-tsan test-build-dependencies
)
for target in "${targets[@]}"; do
    grep -q "^${target}:" tests/Makefile
done
make --no-print-directory -n -B -o "$ROOT/.config" -o "$ROOT/out/host/audiox-menuconfig" \
    "${targets[@]}" audio-bench run-audio-bench BENCH_INPUT=fixture.wav \
    BENCH_ALSA_DEVICE=null BENCH_PORT=8766 > "$WORK/targets"
grep -Fq 'audiox-audio-bench" "fixture.wav" "null" "8766"' "$WORK/targets"
grep -Fq 'watchdog_service_test.sh' "$WORK/targets"
grep -Fq 'resolve_modules_test.sh' "$WORK/targets"
grep -Fq -- '--test -C' "$WORK/targets"
echo "PASS: test and benchmark rules stay available through the root Makefile"

touch "$WORK/config" "$WORK/menuconfig"
for source in init/init init/watchdog audio/setup; do
    object="$WORK/obj/runtime/$source.o"
    header="$WORK/context.hpp"
    mkdir -p "$(dirname "$object")"
    touch "$object" "$header"
    printf '%s: %s\n' "$object" "$header" > "${object%.o}.d"
    touch -t 202001010000 "$header"
    touch -t 202101010000 "$object"
    args=(
        --no-print-directory -n "OUT_DIR=$WORK" "AUDIOX_CONFIG_FILE=$WORK/config"
        "CONFIG_MENU_BIN=$WORK/menuconfig" RUNTIME_DEPS= SBC_STATIC_LIB= SBC_HEADER=
        KV=dependency-test -o "$WORK/config" -o "$WORK/menuconfig" -o "src/$source.cpp" "$object"
    )
    make "${args[@]}" > "$WORK/before"
    if grep -Fq -- "-c -o $object " "$WORK/before"; then
        echo "FAIL: $source should start up to date" >&2
        exit 1
    fi
    touch -t 202201010000 "$header"
    make "${args[@]}" > "$WORK/after"
    grep -Fq -- "-c -o $object " "$WORK/after"
done
echo "PASS: startup, watchdog and worker objects rebuild after header changes"
