#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'find "$WORK" -type f -delete; find "$WORK" -depth -type d -empty -delete' EXIT
trap 'echo "FAIL: line $LINENO: $BASH_COMMAND" >&2' ERR
cd "$ROOT"

grep -Fxq 'include $(CURDIR)/tests/Makefile' Makefile
grep -Fxq '.DEFAULT_GOAL := all' Makefile
! grep -Eq '^test-[[:alnum:]_-]+:' Makefile
targets=(
    test-midi-usb test-script test-script-docs
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
    "${targets[@]}" > "$WORK/targets"
grep -Fq 'tests/native/audio/audio_runtime_test.cpp' "$WORK/targets"
! grep -Eq 'graph_processor|audio-bench|audio/engine.cpp|AUDIOX_USE_SHARED_GRAPH_PROCESSOR' "$WORK/targets"
grep -Fq 'watchdog_service_test.sh' "$WORK/targets"
grep -Fq 'resolve_modules_test.sh' "$WORK/targets"
grep -Fq -- '--test -C' "$WORK/targets"
echo "PASS: host tests use the direct audio runtime through the root Makefile"

for test in audio_meters display_hotplug gadget_volume; do
    grep -E "tests/native/(audio|hardware)/${test}_test.cpp.*src/audio/plugin_host.cpp" "$WORK/targets" >/dev/null
done
echo "PASS: AudioContext host fixtures link the real plugin-host lifecycle"

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
    # Inherited -B would invalidate the up-to-date baseline being tested.
    MAKEFLAGS= MFLAGS= make "${args[@]}" > "$WORK/before"
    if grep -Fq -- "-c -o $object " "$WORK/before"; then
        echo "FAIL: $source should start up to date" >&2
        exit 1
    fi
    touch -t 202201010000 "$header"
    MAKEFLAGS= MFLAGS= make "${args[@]}" > "$WORK/after"
    grep -Fq -- "-c -o $object " "$WORK/after"
done
echo "PASS: startup, watchdog and worker objects rebuild after header changes"
