#!/usr/bin/env bash
set -euo pipefail

SD_MOUNT_BOOT="${1:?usage: wait_and_export.sh <sd_mount_boot> <project_root>}"
PROJECT_ROOT="${2:?usage: wait_and_export.sh <sd_mount_boot> <project_root>}"

echo "=================================================="
echo "   AUTOMATED AUDIO APPLICATION SD EXPORTER        "
echo "=================================================="
echo "--> Waiting for SD card or USB drive to be inserted..."

sudo mkdir -p "${SD_MOUNT_BOOT}"

BASE_DEV=""
for dev in /sys/block/sd* /sys/block/mmcblk*; do
    [[ -e "${dev}" ]] && BASE_DEV+=" $(basename "${dev}")"
done

while true; do
    sleep 0.5

    CUR_DEV=""
    for dev in /sys/block/sd* /sys/block/mmcblk*; do
        [[ -e "${dev}" ]] && CUR_DEV+=" $(basename "${dev}")"
    done

    NEW_DEVS=""
    for d in ${CUR_DEV}; do
        if ! echo " ${BASE_DEV} " | grep -q " ${d} "; then
            NEW_DEVS+=" ${d}"
        fi
    done

    CHOSEN_DEV=""
    TARGET=""
    for candidate in ${NEW_DEVS}; do
        if echo "${candidate}" | grep -q "mmcblk"; then
            PART_NAME="${candidate}p1"
        else
            PART_NAME="${candidate}1"
        fi

        if [[ -e "/sys/class/block/${PART_NAME}" ]]; then
            CHOSEN_DEV="${candidate}"
            TARGET="/dev/${PART_NAME}"
            break
        fi
    done

    if [[ -n "${CHOSEN_DEV}" ]]; then
        echo "--> Valid hardware device detected: ${CHOSEN_DEV}"
        echo "--> Found partition file node: ${TARGET}"

        RETRY_COUNT=0
        while [[ ! -b "${TARGET}" ]]; do
            sleep 0.1
            RETRY_COUNT=$((RETRY_COUNT + 1))
            if [[ ${RETRY_COUNT} -gt 30 ]]; then
                echo "ERROR: Partition ${TARGET} failed to link in /dev." >&2
                exit 1
            fi
        done

        echo "--> Mounting ${TARGET} to ${SD_MOUNT_BOOT}..."
        if sudo mount -o "uid=$(id -u),gid=$(id -g)" "${TARGET}" "${SD_MOUNT_BOOT}"; then
            make -C "${PROJECT_ROOT}" export
            sync
            sudo umount "${SD_MOUNT_BOOT}"
            if command -v eject >/dev/null 2>&1; then
                sudo eject "/dev/${CHOSEN_DEV}" 2>/dev/null || true
            fi
            echo "--> Export complete! SD card is ready for use."
        else
            echo "ERROR: Failed to mount ${TARGET}. Check partition structure." >&2
        fi

        break
    fi
done
