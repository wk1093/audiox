#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -r "$WORK"' EXIT
trap 'echo "FAIL: line $LINENO: $BASH_COMMAND" >&2' ERR
unset MAKEFLAGS MFLAGS MAKELEVEL LLVM_BUILD_JOBS CMAKE_BUILD_PARALLEL_LEVEL
export BUILD_TEST_WORK="$WORK"
export BUILD_TEST_NINJA
BUILD_TEST_NINJA="$(command -v ninja || true)"
mkdir -p "$WORK/bin" "$WORK/source" "$WORK/llvm/source/llvm" "$WORK/llvm/build"
touch "$WORK/config" "$WORK/menuconfig" "$WORK/llvm/source/llvm/CMakeLists.txt"

cat > "$WORK/bin/nproc" <<'EOF'
#!/usr/bin/env bash
echo 4
EOF
cat > "$WORK/bin/git" <<'EOF'
#!/usr/bin/env bash
case " $* " in
    *" rev-parse "*) echo 2078da43e25a4623cab2d0d60decddf709aaea28 ;;
    *" archive "*) cat "$BUILD_TEST_WORK/lua.tar" ;;
esac
EOF
cat > "$WORK/bin/ninja" <<'EOF'
#!/usr/bin/env bash
if [[ "$1" == --version ]]; then
    echo "${BUILD_TEST_NINJA_VERSION:-1.13.2}"
else
    exec "$BUILD_TEST_NINJA" "$@"
fi
EOF
cat > "$WORK/bin/cmake" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == --build ]] || exit 0
printf '%s\n' "$*" > "$BUILD_TEST_WORK/cmake-args"
if [[ "${BUILD_TEST_RUN_NINJA:-0}" == 1 ]]; then
    build="$2"
    shift 2
    jobs=()
    if [[ "${1:-}" == --parallel ]]; then jobs=(-j "$2"); shift 2; fi
    [[ "$1" == --target ]]
    shift
    exec ninja -C "$build" "${jobs[@]}" "$@"
fi
EOF
cat > "$WORK/worker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '+\n' >> "$BUILD_TEST_WORK/events"
sleep 0.1
printf -- '-\n' >> "$BUILD_TEST_WORK/events"
EOF
chmod +x "$WORK/bin/"* "$WORK/worker"
export PATH="$WORK/bin:$PATH"

cat > "$WORK/source/Makefile" <<'EOF'
.PHONY: all liblua.a
all: liblua.a
liblua.a: $(addprefix job,1 2 3 4 5 6 7 8)
	@mkdir -p src
	@touch liblua.a src/dnsmasq
.PHONY: job1 job2 job3 job4 job5 job6 job7 job8
job1 job2 job3 job4 job5 job6 job7 job8:
	@"$(BUILD_TEST_WORK)/worker"
EOF
printf '#define LUA_VERSION_MAJOR "5"\n#define LUA_VERSION_MINOR "4"\n#define LUA_VERSION_RELEASE "9"\n' > "$WORK/source/lua.h"
touch "$WORK/source/lauxlib.h" "$WORK/source/luaconf.h" "$WORK/source/COPYING" "$WORK/source/COPYING-v3"
tar -cf "$WORK/lua.tar" -C "$WORK/source" .
mkdir -p "$WORK/archive/dnsmasq-test" "$WORK/downloads"
cp -a "$WORK/source/." "$WORK/archive/dnsmasq-test/"
tar -cJf "$WORK/downloads/dnsmasq-test.tar.xz" -C "$WORK/archive" dnsmasq-test
checksum="$(sha256sum "$WORK/downloads/dnsmasq-test.tar.xz" | cut -d' ' -f1)"

cat > "$WORK/fixture.mk" <<'EOF'
.PHONY: parallel-probe
parallel-probe: $(LUA_STATIC_LIB) $(HOST_LUA_STATIC_LIB) $(DNSMASQ_BIN) $(PROBE_LLVM)
EOF
args=(
    --no-print-directory -C "$ROOT" -f Makefile -f "$WORK/fixture.mk"
    "AUDIOX_CONFIG_FILE=$WORK/config" "CONFIG_MENU_BIN=$WORK/menuconfig"
    -o "$WORK/config" -o "$WORK/menuconfig"
    "LUA_BUILD_ROOT=$WORK/lua" "HOST_LUA_BUILD_ROOT=$WORK/host-lua"
    LUA_COMMIT=2078da43e25a4623cab2d0d60decddf709aaea28
    "DNSMASQ_BIN=$WORK/dnsmasq/dnsmasq" DNSMASQ_VERSION=test "DNSMASQ_SHA256=$checksum"
    "SHARED_OUT_DIR=$WORK" CC=cc CXX=c++ HOSTCC=cc HOSTAR=ar
    parallel-probe
)
peak_jobs() {
    awk '/^\+$/ { active++; if (active > peak) peak = active }
         /^-$/ { active--; if (active < 0) exit 1 }
         END { if (active != 0 || peak == 0) exit 1; print peak }' "$WORK/events"
}
limits=(1 8 default)
if make --help | grep -q -- --jobserver-style; then limits+=(pipe); fi
for limit in "${limits[@]}"; do
    rm -f "$WORK/lua/lib/liblua.a" "$WORK/host-lua/lib/liblua.a" "$WORK/dnsmasq/dnsmasq"
    : > "$WORK/events"
    jobs=()
    expected=4
    case "$limit" in
        default) ;;
        pipe) jobs=(-j8 --jobserver-style=pipe); expected=8 ;;
        *) jobs=(-j"$limit"); expected="$limit" ;;
    esac
    make "${args[@]}" "${jobs[@]}" > "$WORK/make-log" 2>&1
    peak="$(peak_jobs)"
    if (( expected == 1 )); then test "$peak" = 1; else test "$peak" -gt 1; fi
    test "$peak" -le "$expected"
    ! grep -Ei 'jobserver unavailable|resetting jobserver|forced in submake' "$WORK/make-log"
done
echo "PASS: Lua, host Lua and dnsmasq share Make's default, -j8 and -j1 budgets"
grep -Fxq $'\t$(MAKE) -C "$(ALSA_BUILD_DIR)"' "$ROOT/Makefile"
grep -Fxq $'\t$(MAKE) -C "$(SBC_BUILD_DIR)"' "$ROOT/Makefile"
echo "PASS: ALSA and SBC inherit Make's job limit without resetting the jobserver"

llvm_args=("$WORK/llvm" cc c++)
for limit in 1 8; do
    MAKELEVEL=1 MAKEFLAGS="-j$limit --jobserver-auth=3,4" BUILD_TEST_NINJA_VERSION=1.11.1 \
        bash "$ROOT/tools/build/build_graph_llvm.sh" "${llvm_args[@]}"
    grep -Fq -- "--parallel $limit --target" "$WORK/cmake-args"
done
MAKELEVEL=1 MAKEFLAGS='-j8 --jobserver-auth=fifo:/test' \
    bash "$ROOT/tools/build/build_graph_llvm.sh" "${llvm_args[@]}"
! grep -Fq -- --parallel "$WORK/cmake-args"
MAKELEVEL=1 MAKEFLAGS='-j8 --jobserver-auth=fifo:/test' LLVM_BUILD_JOBS=3 \
    bash "$ROOT/tools/build/build_graph_llvm.sh" "${llvm_args[@]}"
grep -Fq -- '--parallel 3 --target' "$WORK/cmake-args"
bash "$ROOT/tools/build/build_graph_llvm.sh" "${llvm_args[@]}"
grep -Fq -- '--parallel 4 --target' "$WORK/cmake-args"
if LLVM_BUILD_JOBS=invalid bash "$ROOT/tools/build/build_graph_llvm.sh" "${llvm_args[@]}" > "$WORK/error" 2>&1; then
    echo "FAIL: invalid LLVM job limit accepted" >&2
    exit 1
fi
grep -Fq 'LLVM_BUILD_JOBS must be a positive integer' "$WORK/error"
echo "PASS: LLVM jobserver selection, legacy Ninja limits, standalone default and explicit override"

# Exercise real Ninja token sharing when the installed tools support it.
if [[ -n "$BUILD_TEST_NINJA" ]] &&
   [[ "$("$BUILD_TEST_NINJA" --version)" =~ ^1\.(1[3-9]|[2-9][0-9])\. ]] &&
   make --help | grep -q -- --jobserver-style; then
    {
        printf 'rule compile\n  command = %s/worker\n' "$WORK"
        for job in {1..8}; do printf 'build job%s: compile\n' "$job"; done
        printf 'build LLVMAArch64CodeGen LLVMCore LLVMObject LLVMRuntimeDyld LLVMExecutionEngine LLVMPasses: phony job1 job2 job3 job4 job5 job6 job7 job8\n'
    } > "$WORK/llvm/build/build.ninja"
    for limit in 1 8; do
        rm -f "$WORK/lua/lib/liblua.a" "$WORK/host-lua/lib/liblua.a" "$WORK/dnsmasq/dnsmasq" "$WORK/llvm/ready"
        : > "$WORK/events"
        BUILD_TEST_RUN_NINJA=1 make "${args[@]}" -j"$limit" --jobserver-style=fifo \
            "PROBE_LLVM=$WORK/llvm/ready" "GRAPH_LLVM_ROOT=$WORK/llvm" > "$WORK/make-log" 2>&1
        peak="$(peak_jobs)"
        if (( limit == 1 )); then test "$peak" = 1; else test "$peak" -gt 1; fi
        test "$peak" -le "$limit"
    done
    echo "PASS: real Ninja and dependency submakes share one global job budget"
fi

rm -f "$WORK/lua/lib/liblua.a" "$WORK/host-lua/lib/liblua.a" "$WORK/dnsmasq/dnsmasq" "$WORK/llvm/ready" "$WORK/cmake-args"
make "${args[@]}" -n -B "PROBE_LLVM=$WORK/llvm/ready" "GRAPH_LLVM_ROOT=$WORK/llvm" > "$WORK/dry-run"
test ! -e "$WORK/lua/lib/liblua.a"
test ! -e "$WORK/dnsmasq/dnsmasq"
test ! -e "$WORK/llvm/ready"
test ! -e "$WORK/cmake-args"
echo "PASS: dry runs do not execute recursive dependency build scripts"
if make "${args[@]}" -q > "$WORK/query-log" 2>&1; then
    echo "FAIL: Make query reported missing dependencies as up to date" >&2
    exit 1
else
    test "$?" = 1
fi
for script in build_lua.sh build_dnsmasq.sh build_graph_llvm.sh; do
    MAKEFLAGS=t bash "$ROOT/tools/build/$script"
done
test ! -e "$WORK/cmake-args"
echo "PASS: query and touch modes do not execute dependency builds"
