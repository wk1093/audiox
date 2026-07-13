#!/usr/bin/env bash
set -euo pipefail

IMG_FILE="${1:?usage: build_image.sh <img_file> <img_size_mb> <out_dir> <bootloader_initramfs> <program_initramfs> <vc4_overlay> [dsi_overlay>}"
IMG_SIZE_MB="${2:?usage: build_image.sh <img_file> <img_size_mb> <out_dir> <bootloader_initramfs> <program_initramfs> <vc4_overlay> [dsi_overlay>}"
OUT_DIR="${3:?usage: build_image.sh <img_file> <img_size_mb> <out_dir> <bootloader_initramfs> <program_initramfs> <vc4_overlay> [dsi_overlay>}"
INITRAMFS="${4:?usage: build_image.sh <img_file> <img_size_mb> <out_dir> <bootloader_initramfs> <program_initramfs> <vc4_overlay> [dsi_overlay>}"
PROGRAM_INITRAMFS="${5:?usage: build_image.sh <img_file> <img_size_mb> <out_dir> <bootloader_initramfs> <program_initramfs> <vc4_overlay> [dsi_overlay>}"
VC4_OVERLAY="${6:?usage: build_image.sh <img_file> <img_size_mb> <out_dir> <bootloader_initramfs> <program_initramfs> <vc4_overlay> [dsi_overlay>}"
DSI_TOUCH_OVERLAY="${7:-}"

MNT_BOOT="${OUT_DIR}/mnt_boot"

echo "Creating a blank ${IMG_SIZE_MB}MB image file..."
dd if=/dev/zero of="${IMG_FILE}" bs=1M count="${IMG_SIZE_MB}" status=progress

parted --script "${IMG_FILE}" mklabel msdos
parted --script "${IMG_FILE}" mkpart primary fat32 2048s 98303s
parted --script "${IMG_FILE}" set 1 boot on
parted --script "${IMG_FILE}" mkpart primary fat32 98304s 100%

LOOP_DEV="$(sudo losetup --find --show --partscan "${IMG_FILE}")"
cleanup() {
    if mountpoint -q "${MNT_BOOT}"; then
        sudo umount "${MNT_BOOT}" || true
    fi
    if [[ -n "${LOOP_DEV:-}" ]]; then
        sudo losetup -d "${LOOP_DEV}" || true
    fi
    rm -rf "${MNT_BOOT}"
}
trap cleanup EXIT

echo "Loop device assigned at: ${LOOP_DEV}"
echo "Formatting system partitions..."
sudo mkfs.vfat -F 32 -n "BOOT" "${LOOP_DEV}p1"
sudo mkfs.vfat -F 32 -n "STORAGE" "${LOOP_DEV}p2"

echo "Mounting image internally to stage data..."
mkdir -p "${MNT_BOOT}"
sudo mount "${LOOP_DEV}p1" "${MNT_BOOT}"

echo "Writing distribution files..."
sudo cp -r "${OUT_DIR}/bootfiles/." "${MNT_BOOT}/"
sudo cp "${INITRAMFS}" "${MNT_BOOT}/initramfs.cpio.gz"
sudo cp "${PROGRAM_INITRAMFS}" "${MNT_BOOT}/program.cpio.gz"
sudo sh -c "echo 'initramfs initramfs.cpio.gz,program.cpio.gz followkernel' > '${MNT_BOOT}/config.txt'"
sudo sh -c "echo 'dtoverlay=${VC4_OVERLAY}' >> '${MNT_BOOT}/config.txt'"
if [[ -n "${DSI_TOUCH_OVERLAY}" ]]; then
    sudo sh -c "echo 'dtoverlay=${DSI_TOUCH_OVERLAY}' >> '${MNT_BOOT}/config.txt'"
fi
sudo sh -c "echo 'dtoverlay=dwc2,dr_mode=peripheral' >> '${MNT_BOOT}/config.txt'"
sudo sh -c "echo 'console=fbcon console=tty1 quiet loglevel=3 rdinit=/init' > '${MNT_BOOT}/cmdline.txt'"

echo "SUCCESS! Flashable image generated at: ${IMG_FILE}"
