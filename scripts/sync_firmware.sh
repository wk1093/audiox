#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_GIT_URL="${1:?usage: sync_firmware.sh <firmware_git_url> <out_dir> <vc4_overlay> [dsi_overlay]>}"
OUT_DIR="${2:?usage: sync_firmware.sh <firmware_git_url> <out_dir> <vc4_overlay> [dsi_overlay]>}"
VC4_OVERLAY="${3:?usage: sync_firmware.sh <firmware_git_url> <out_dir> <vc4_overlay> [dsi_overlay]>}"
DSI_TOUCH_OVERLAY="${4:-}"

FIRMWARE_DIR="${OUT_DIR}/firmware"
BOOT_SRC_DIR="${FIRMWARE_DIR}/boot"
BOOT_DST_DIR="${OUT_DIR}/bootfiles"

mkdir -p "${OUT_DIR}"

if [[ ! -d "${FIRMWARE_DIR}/.git" ]]; then
    echo "[sync_firmware] Cloning firmware repository..."
    git clone --depth 1 "${FIRMWARE_GIT_URL}" "${FIRMWARE_DIR}"
else
    echo "[sync_firmware] Updating firmware repository..."
    git -C "${FIRMWARE_DIR}" fetch --depth 1 origin
    git -C "${FIRMWARE_DIR}" reset --hard origin/master
fi

mkdir -p "${BOOT_DST_DIR}/overlays"

copy_boot_file() {
    local rel_path="$1"
    if [[ ! -f "${BOOT_SRC_DIR}/${rel_path}" ]]; then
        echo "[sync_firmware] ERROR: missing boot artifact ${rel_path}" >&2
        exit 1
    fi
    install -m 0644 "${BOOT_SRC_DIR}/${rel_path}" "${BOOT_DST_DIR}/${rel_path}"
}

copy_boot_file "kernel8.img"
copy_boot_file "bootcode.bin"
copy_boot_file "fixup.dat"
copy_boot_file "start.elf"
copy_boot_file "start_cd.elf"
copy_boot_file "start4.elf"
copy_boot_file "start4cd.elf"
copy_boot_file "bcm2711-rpi-4-b.dtb"
copy_boot_file "overlays/${VC4_OVERLAY}.dtbo"
if [[ -n "${DSI_TOUCH_OVERLAY}" ]]; then
    copy_boot_file "overlays/${DSI_TOUCH_OVERLAY}.dtbo"
fi

echo "[sync_firmware] Boot files staged in ${BOOT_DST_DIR}"
