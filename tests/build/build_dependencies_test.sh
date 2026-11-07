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
grep -Fq -- '-isystem "$(GRAPH_LLVM_ROOT)/source/llvm/include"' Makefile
grep -Fq -- '-isystem "$(GRAPH_LLVM_ROOT)/build/include"' Makefile
grep -Fq -- '-isystem "$$($(HOST_LLVM_CONFIG) --includedir)"' Makefile
echo "PASS: graph compiler builds treat LLVM headers as system headers"

for test in audio_meters display_hotplug gadget_volume; do
    grep -E "tests/native/(audio|hardware)/${test}_test.cpp.*src/audio/plugin_host.cpp" "$WORK/targets" >/dev/null
done
echo "PASS: AudioContext host fixtures link the real plugin-host lifecycle"

touch "$WORK/config" "$WORK/menuconfig"
version_file="$WORK/obj/runtime/audiox-version"
version_args=(
    --no-print-directory "OUT_DIR=$WORK" "AUDIOX_CONFIG_FILE=$WORK/config"
    "CONFIG_MENU_BIN=$WORK/menuconfig" -o "$WORK/config" -o "$WORK/menuconfig"
    AUDIOX_VERSION_MAJOR=1 AUDIOX_VERSION_MINOR=16 AUDIOX_VERSION_PATCH=0
    AUDIOX_VERSION_PRERELEASE=beta.1 AUDIOX_VERSION_NIGHTLY=0 AUDIOX_VERSION_BUILD=
)
MAKEFLAGS= MFLAGS= make "${version_args[@]}" "$version_file"
grep -Fxq '1.16.0-beta.1' "$version_file"
touch -t 202001010000 "$version_file"
version_timestamp="$(stat -c %Y "$version_file")"
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
        -o "$version_file"
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

cat > "$WORK/compiler" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$COMPILER_LOG"
while (($#)); do
    if [[ "$1" == -o ]]; then touch "$2"; exit 0; fi
    shift
done
exit 1
EOF
chmod +x "$WORK/compiler"
export COMPILER_LOG="$WORK/compile-log"
object="$WORK/obj/runtime/http/api.o"
probe_args=(
    "${version_args[@]}" "RUNTIME_COMPILER=$WORK/compiler"
    RUNTIME_DEPS= SBC_STATIC_LIB= SBC_HEADER= LUA_STATIC_LIB= LUA_HEADERS=
    KV=dependency-test -o src/http/api.cpp "$object"
)
MAKEFLAGS= MFLAGS= make "${version_args[@]}" "$version_file"
test "$(stat -c %Y "$version_file")" = "$version_timestamp"
MAKEFLAGS= MFLAGS= make "${probe_args[@]}" > "$WORK/version-before"
test "$(wc -l < "$COMPILER_LOG")" = 1
MAKEFLAGS= MFLAGS= make "${probe_args[@]}" > "$WORK/version-unchanged"
test "$(wc -l < "$COMPILER_LOG")" = 1
sleep 1
MAKEFLAGS= MFLAGS= make "${probe_args[@]}" AUDIOX_VERSION_PRERELEASE=beta.2 > "$WORK/version-after"
grep -Fxq '1.16.0-beta.2' "$version_file"
test "$(wc -l < "$COMPILER_LOG")" = 2
grep -Fq -- '-DAUDIOX_VERSION_STRING="1.16.0-beta.2"' "$COMPILER_LOG"
sleep 1
MAKEFLAGS= MFLAGS= make "${probe_args[@]}" > "$WORK/version-restored"
grep -Fxq '1.16.0-beta.1' "$version_file"
test "$(wc -l < "$COMPILER_LOG")" = 3
echo "PASS: version changes rebuild the API and unchanged versions preserve incremental builds"
