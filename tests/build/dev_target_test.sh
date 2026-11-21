#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -r "$work"' EXIT
mkdir -p "$work/bin" "$work/out"
touch "$work/config" "$work/boot-config" "$work/cmdline" "$work/kernel" "$work/bootloader" "$work/runtime"

cat > "$work/bin/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
method=GET
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -X) method="$2"; shift ;;
        --output) output="$2"; shift ;;
    esac
    shift
done
printf '%s\n' "$method" >> "$TEST_WORK/requests"
if [ "$method" = GET ]; then
    if [ "${TEST_OFFLINE:-0}" = 1 ]; then exit 7; fi
    count="$(wc -l < "$TEST_WORK/checks")"
    printf 'check\n' >> "$TEST_WORK/checks"
    if [ "$count" -gt 0 ]; then
        cat "$TEST_WORK/second"
    else
        cat "$TEST_WORK/first"
    fi
else
    printf 'ok\n' > "$output"
    printf '200'
fi
EOF
cat > "$work/bin/fake-make" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$TEST_WORK/builds"
EOF
chmod +x "$work/bin/curl" "$work/bin/fake-make"
export TEST_WORK="$work"
export PATH="$work/bin:$PATH"
unset MAKEFLAGS MFLAGS MAKELEVEL

run_dev() {
    : > "$work/requests"
    : > "$work/checks"
    : > "$work/builds"
    local pi4= pi5=
    case "$1" in pi4) pi4=y ;; pi5) pi5=y ;; esac
    if [ "${TEST_WRONG_CONFIG:-0}" = 1 ]; then pi4=y; pi5=; fi
    make -s -j1 -C "$root" dev \
        "MAKE=$work/bin/fake-make" "AUDIOX_CONFIG_FILE=$work/config" \
        "AUDIOX_BOARD=$1" "PI_HOST=example.test" "PI_PORT=1234" \
        "CONFIG_BOARD_PI4=$pi4" "CONFIG_BOARD_PI5=$pi5" \
        "OUT_DIR=$work/out" "BOOT_CONFIG_FILE=$work/boot-config" \
        "BOOT_CMDLINE_FILE=$work/cmdline" "BOOT_KERNEL_FILE=$work/kernel" \
        "INITRAMFS=$work/bootloader" "PROGRAM_INITRAMFS=$work/runtime" \
        > "$work/log" 2>&1
}

for board in 4 5; do
    printf '{"board":"pi%s","model":"Raspberry Pi %s Model B Rev 1.0"}\n' "$board" "$board" > "$work/first"
    cp "$work/first" "$work/second"
    run_dev "pi$board"
    [ "$(grep -c '^GET$' "$work/requests")" -eq 2 ]
    [ "$(grep -c '^PUT$' "$work/requests")" -eq 5 ]
    [ "$(wc -l < "$work/builds")" -eq 3 ]
done

printf '%s\n' '{"model":"Raspberry Pi 4 Model B Rev 1.5"}' > "$work/first"
cp "$work/first" "$work/second"
run_dev pi4

for info in \
    '{"board":"pi4","model":"Raspberry Pi 4 Model B Rev 1.5"}' \
    '{"board":"pi5","model":"Raspberry Pi 4 Model B Rev 1.5"}' \
    '{"board":"pi5","model":"unknown"}' \
    '{"board":"pi5","model":"Raspberry Pi 500"}' \
    '{"board":"pi5"}' \
    'not json'; do
    printf '%s\n' "$info" > "$work/first"
    cp "$work/first" "$work/second"
    if run_dev pi5; then echo "Unsafe target accepted: $info" >&2; exit 1; fi
    ! grep -q '^PUT$' "$work/requests"
    [ ! -s "$work/builds" ]
done

export TEST_OFFLINE=1
if run_dev pi5; then echo "Unreachable target accepted" >&2; exit 1; fi
! grep -q '^PUT$' "$work/requests"
[ ! -s "$work/builds" ]
unset TEST_OFFLINE

export TEST_WRONG_CONFIG=1
if run_dev pi5; then echo "Inconsistent build configuration accepted" >&2; exit 1; fi
[ ! -s "$work/requests" ]
[ ! -s "$work/builds" ]
unset TEST_WRONG_CONFIG

printf '%s\n' '{"board":"pi5","model":"Raspberry Pi 5 Model B Rev 1.0"}' > "$work/first"
printf '%s\n' '{"board":"pi4","model":"Raspberry Pi 4 Model B Rev 1.5"}' > "$work/second"
if run_dev pi5; then echo "Changed target accepted" >&2; exit 1; fi
[ "$(grep -c '^GET$' "$work/requests")" -eq 2 ]
! grep -q '^PUT$' "$work/requests"
[ "$(wc -l < "$work/builds")" -eq 3 ]

if run_dev qemu; then echo "QEMU target accepted" >&2; exit 1; fi
[ ! -s "$work/requests" ]
[ ! -s "$work/builds" ]
echo "PASS: make dev verifies hardware before building and uploading"
