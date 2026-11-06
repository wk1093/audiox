#!/usr/bin/env bash
# Boots the AudioX QEMU debug build (make debug) on a generic arm64 virtual
# machine with a serial console in this terminal (quit with Ctrl-A X).
#
#   - /dev/vda: virtio disk (vda1 BOOT, vda2 STORAGE), persists between runs
#   - eth0: QEMU user networking with host port forwards
#       127.0.0.1:<QEMU_HTTP_PORT>  -> web UI (port 80)
#       127.0.0.1:<QEMU_USBIP_PORT> -> USB/IP exporter for the usbip-vudc gadget
#   - QEMU_USBIP_ATTACH=1 attaches the gadget to this host once it is up (and
#     again whenever the VM drops the link), so it shows up as a USB audio
#     interface and USB network adapter (needs usbip, vhci-hcd and sudo).
#   - virtio-gpu provides /dev/fb0 at the touchscreen's 800x480 (not shown by
#     default; add QEMU_EXTRA_ARGS="-display gtk" to see it).
set -euo pipefail

: "${QEMU_KERNEL:?QEMU_KERNEL is required}"
: "${QEMU_INITRD:?QEMU_INITRD is required}"
: "${QEMU_DISK:?QEMU_DISK is required}"
QEMU_BIN="${QEMU_BIN:-qemu-system-aarch64}"
QEMU_CMDLINE="${QEMU_CMDLINE:-console=ttyAMA0 rdinit=/init loglevel=4}"
QEMU_MEMORY="${QEMU_MEMORY:-1024}"
QEMU_SMP="${QEMU_SMP:-4}"
QEMU_HTTP_PORT="${QEMU_HTTP_PORT:-8080}"
QEMU_USBIP_PORT="${QEMU_USBIP_PORT:-3240}"
QEMU_USBIP_ATTACH="${QEMU_USBIP_ATTACH:-1}"
QEMU_USBIP_BUSID="${QEMU_USBIP_BUSID:-usbip-vudc.0}"
QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-}"
SCRIPTS_DIR="$(cd "$(dirname "$0")" && pwd)"

command -v "${QEMU_BIN}" >/dev/null 2>&1 || { echo "[qemu] ERROR: ${QEMU_BIN} not found (install qemu-system-aarch64)" >&2; exit 1; }
for f in "${QEMU_KERNEL}" "${QEMU_INITRD}" "${QEMU_DISK}"; do
    [[ -f "${f}" ]] || { echo "[qemu] ERROR: missing ${f}" >&2; exit 1; }
done

port_busy() {
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}
for port in "${QEMU_HTTP_PORT}" "${QEMU_USBIP_PORT}"; do
    if port_busy "${port}"; then
        echo "[qemu] ERROR: host port ${port} is already in use (another VM, or usbipd?)." >&2
        echo "[qemu] Override with QEMU_HTTP_PORT=... / QEMU_USBIP_PORT=..." >&2
        exit 1
    fi
done

accel=(-accel tcg,thread=multi -cpu cortex-a72)
if [[ "$(uname -m)" == "aarch64" && -w /dev/kvm ]]; then
    accel=(-accel kvm -cpu host)
fi

attach_pid=""
if [[ "${QEMU_USBIP_ATTACH}" == "1" ]]; then
    if ! command -v usbip >/dev/null 2>&1 && [[ ! -x /usr/sbin/usbip ]]; then
        echo "[qemu] usbip not installed; skipping USB/IP attach. Install usbip, then run 'make debug-attach' while the VM runs."
    else
        sudo_cmd="sudo -n"
        if [[ "${EUID}" -eq 0 ]]; then
            sudo_cmd=""
        elif ! sudo -v; then
            echo "[qemu] sudo unavailable; skipping USB/IP attach. Run 'make debug-attach' while the VM runs."
            sudo_cmd="skip"
        fi
        if [[ "${sudo_cmd}" != "skip" ]]; then
            SUDO="${sudo_cmd}" "${SCRIPTS_DIR}/qemu_usbip_attach.sh" 127.0.0.1 "${QEMU_USBIP_PORT}" "${QEMU_USBIP_BUSID}" 300 1 &
            attach_pid=$!
        fi
    fi
fi
cleanup() {
    if [[ -n "${attach_pid}" ]]; then
        kill "${attach_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

cat <<EOF
[qemu] Booting AudioX debug VM (serial console below; quit with Ctrl-A X)
[qemu]   web UI:  http://127.0.0.1:${QEMU_HTTP_PORT}/
[qemu]   USB/IP:  usbip --tcp-port ${QEMU_USBIP_PORT} attach -r 127.0.0.1 -b ${QEMU_USBIP_BUSID}  (or: make debug-attach)
EOF

# shellcheck disable=SC2086
"${QEMU_BIN}" \
    -M virt \
    "${accel[@]}" \
    -smp "${QEMU_SMP}" \
    -m "${QEMU_MEMORY}" \
    -kernel "${QEMU_KERNEL}" \
    -initrd "${QEMU_INITRD}" \
    -append "${QEMU_CMDLINE}" \
    -drive "if=none,id=disk0,format=raw,file=${QEMU_DISK}" \
    -device virtio-blk-pci,drive=disk0 \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${QEMU_HTTP_PORT}-:80,hostfwd=tcp:127.0.0.1:${QEMU_USBIP_PORT}-:3240" \
    -device virtio-net-pci,netdev=net0 \
    -device virtio-rng-pci \
    -device virtio-gpu-pci,xres=800,yres=480 \
    -display none \
    -serial mon:stdio \
    ${QEMU_EXTRA_ARGS}
