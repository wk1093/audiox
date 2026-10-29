#!/usr/bin/env bash
# Builds the virtio disk for the QEMU debug build (make debug). Same layout as
# the SD card image from build_image.sh (p1 FAT32 BOOT, p2 ext4 STORAGE), but
# assembled from partition files so no root, loop devices or mounts are needed.
# The VM sees the partitions as /dev/vda1 and /dev/vda2.
set -euo pipefail

USAGE="usage: build_qemu_disk.sh <img_file> <img_size_mb> <bootloader_initramfs> <program_initramfs> <boot_config> <cmdline> [ffmpeg_bin]"
IMG_FILE="${1:?${USAGE}}"
IMG_SIZE_MB="${2:?${USAGE}}"
INITRAMFS="${3:?${USAGE}}"
PROGRAM_INITRAMFS="${4:?${USAGE}}"
BOOT_CONFIG="${5:?${USAGE}}"
BOOT_CMDLINE="${6:?${USAGE}}"
FFMPEG_BIN="${7:-}"

for tool in sfdisk mkfs.vfat mcopy mkfs.ext4; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "[qemu-disk] ERROR: '${tool}' is required (packages: util-linux, dosfstools, mtools, e2fsprogs)" >&2
        exit 1
    }
done

# Partition 1 spans sectors 2048..98303 like build_image.sh; partition 2 fills the rest.
SECTOR=512
P1_START=2048
P1_SECTORS=96256
P2_START=98304
TOTAL_SECTORS=$(( IMG_SIZE_MB * 1024 * 1024 / SECTOR ))
P2_SECTORS=$(( TOTAL_SECTORS - P2_START ))
if (( P2_SECTORS < 64 * 2048 )); then
    echo "[qemu-disk] ERROR: image size ${IMG_SIZE_MB}MiB is too small" >&2
    exit 1
fi

copy_boot_files() {
    local target="$1"
    mcopy -o -i "${target}" "${INITRAMFS}" ::/initramfs.cpio.gz
    mcopy -o -i "${target}" "${PROGRAM_INITRAMFS}" ::/program.cpio.gz
    mcopy -o -i "${target}" "${BOOT_CONFIG}" ::/config.txt
    mcopy -o -i "${target}" "${BOOT_CMDLINE}" ::/cmdline.txt
}

# An existing disk keeps its STORAGE partition (settings, recordings) and only
# gets fresh boot files; remove the image (make debug-reset-disk) to start over.
if [[ -f "${IMG_FILE}" ]]; then
    echo "[qemu-disk] Refreshing boot files in ${IMG_FILE}"
    copy_boot_files "${IMG_FILE}@@$(( P1_START * SECTOR ))"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

echo "[qemu-disk] Creating BOOT partition..."
mkfs.vfat -C -F 32 -n BOOT "${WORK}/boot.vfat" $(( P1_SECTORS * SECTOR / 1024 )) >/dev/null
copy_boot_files "${WORK}/boot.vfat"

echo "[qemu-disk] Creating STORAGE partition..."
mkdir -p "${WORK}/storage"
if [[ -n "${FFMPEG_BIN}" && -f "${FFMPEG_BIN}" ]]; then
    install -m 0755 "${FFMPEG_BIN}" "${WORK}/storage/ffmpeg"
fi
mkfs.ext4 -q -F -L STORAGE -E root_owner=0:0 -d "${WORK}/storage" "${WORK}/storage.ext4" $(( P2_SECTORS * SECTOR / 1024 ))K

echo "[qemu-disk] Assembling ${IMG_FILE} (${IMG_SIZE_MB}MiB)..."
mkdir -p "$(dirname "${IMG_FILE}")"
rm -f "${IMG_FILE}.tmp"
truncate -s "${IMG_SIZE_MB}M" "${IMG_FILE}.tmp"
sfdisk --quiet "${IMG_FILE}.tmp" <<EOF
label: dos
start=${P1_START}, size=${P1_SECTORS}, type=c, bootable
start=${P2_START}, size=${P2_SECTORS}, type=83
EOF
dd if="${WORK}/boot.vfat" of="${IMG_FILE}.tmp" bs=${SECTOR} seek=${P1_START} conv=notrunc,sparse status=none
dd if="${WORK}/storage.ext4" of="${IMG_FILE}.tmp" bs=${SECTOR} seek=${P2_START} conv=notrunc,sparse status=none
mv "${IMG_FILE}.tmp" "${IMG_FILE}"
echo "[qemu-disk] Disk ready: ${IMG_FILE}"
