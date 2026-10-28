#!/usr/bin/env bash
set -euo pipefail

USAGE="usage: sync_firmware.sh <firmware_git_url> <firmware_ref> <out_dir> <kernel_image> <dtbs> [firmware_files] [overlays]"
FIRMWARE_GIT_URL="${1:?${USAGE}}"
FIRMWARE_GIT_REF="${2:?${USAGE}}"
OUT_DIR="${3:?${USAGE}}"
KERNEL_IMAGE="${4:?${USAGE}}"
BOARD_DTBS="${5:?${USAGE}}"
BOARD_FIRMWARE_FILES="${6:-}"
BOOT_OVERLAYS="${7:-}"

FIRMWARE_DIR="${OUT_DIR}/firmware"
BOOT_SRC_DIR="${FIRMWARE_DIR}/boot"
BOOT_DST_DIR="${OUT_DIR}/bootfiles"

mkdir -p "${OUT_DIR}"

# Records "<tag> <commit>" after a tag checkout so later runs can skip the network.
SYNCED_TAG_FILE="${FIRMWARE_DIR}/.git/audiox-synced-tag"

# Commit hashes and tags are immutable, so a matching checkout needs no fetch.
# Branches are always fetched so they keep tracking upstream.
ref_already_checked_out() {
    [[ -d "${FIRMWARE_DIR}/.git" ]] || return 1
    local head
    head="$(git -C "${FIRMWARE_DIR}" rev-parse -q --verify HEAD 2>/dev/null)" || return 1
    if [[ "${FIRMWARE_GIT_REF}" =~ ^[0-9a-fA-F]{7,40}$ ]]; then
        [[ "${head}" == "${FIRMWARE_GIT_REF,,}"* ]]
        return
    fi
    [[ -f "${SYNCED_TAG_FILE}" && "$(cat "${SYNCED_TAG_FILE}")" == "${FIRMWARE_GIT_REF} ${head}" ]]
}

if ref_already_checked_out; then
    echo "[sync_firmware] ${FIRMWARE_GIT_REF} already checked out; skipping fetch"
else
    if [[ ! -d "${FIRMWARE_DIR}/.git" ]]; then
        echo "[sync_firmware] Cloning firmware repository..."
        git clone --depth 1 --branch master "${FIRMWARE_GIT_URL}" "${FIRMWARE_DIR}"
    else
        echo "[sync_firmware] Updating firmware repository..."
    fi

    rm -f "${SYNCED_TAG_FILE}"
    git -C "${FIRMWARE_DIR}" fetch --depth 1 origin "${FIRMWARE_GIT_REF}"
    git -C "${FIRMWARE_DIR}" checkout --detach FETCH_HEAD
    if grep -qF "tag '${FIRMWARE_GIT_REF}' of " "${FIRMWARE_DIR}/.git/FETCH_HEAD"; then
        echo "${FIRMWARE_GIT_REF} $(git -C "${FIRMWARE_DIR}" rev-parse HEAD)" > "${SYNCED_TAG_FILE}"
    fi
fi

# Restage from scratch so files from a previously selected board are not exported.
rm -rf "${BOOT_DST_DIR}"
mkdir -p "${BOOT_DST_DIR}/overlays"

copy_boot_file() {
    local rel_path="$1"
    local dst_path="${2:-$1}"
    if [[ ! -f "${BOOT_SRC_DIR}/${rel_path}" ]]; then
        echo "[sync_firmware] ERROR: missing boot artifact ${rel_path}" >&2
        exit 1
    fi
    install -m 0644 "${BOOT_SRC_DIR}/${rel_path}" "${BOOT_DST_DIR}/${dst_path}"
}

# The kernel keeps its firmware name; the generated config.txt points kernel= at it.
copy_boot_file "${KERNEL_IMAGE}"
for file in ${BOARD_FIRMWARE_FILES}; do
    copy_boot_file "${file}"
done
for dtb in ${BOARD_DTBS}; do
    copy_boot_file "${dtb}"
done
for overlay in ${BOOT_OVERLAYS}; do
    copy_boot_file "overlays/${overlay}.dtbo"
done

echo "[sync_firmware] Boot files for ${KERNEL_IMAGE} staged in ${BOOT_DST_DIR}"
