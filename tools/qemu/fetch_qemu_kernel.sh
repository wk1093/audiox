#!/usr/bin/env bash
# Fetches the generic arm64 Debian kernel used by the QEMU debug build
# (make debug). The Raspberry Pi kernel has no virtio drivers, so the VM boots
# this kernel instead and the runtime loads modules from the same package.
#
# Output layout:
#   <dest_dir>/vmlinuz
#   <dest_dir>/modules/<kernel_version>/{kernel/...,modules.builtin,...}
set -euo pipefail

USAGE="usage: fetch_qemu_kernel.sh <dest_dir> <kernel_version> <deb_file> <sha256> <url> [fallback_url...]"
DEST_DIR="${1:?${USAGE}}"
KV="${2:?${USAGE}}"
DEB_FILE="${3:?${USAGE}}"
SHA256="${4?${USAGE}}"
shift 4
[[ $# -gt 0 ]] || { echo "${USAGE}" >&2; exit 1; }
URLS=("$@")
FETCH="${PKG_FETCH:-curl -fsSL}"

STAMP="${DEST_DIR}/.extracted"
if [[ -f "${STAMP}" && -f "${DEST_DIR}/vmlinuz" && -d "${DEST_DIR}/modules/${KV}/kernel" ]]; then
    exit 0
fi

for tool in ar tar xz; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "[qemu-kernel] ERROR: '${tool}' is required" >&2; exit 1; }
done

verify() {
    [[ -z "${SHA256}" ]] && return 0
    [[ "$(sha256sum "$1" | awk '{print $1}')" == "${SHA256}" ]]
}

mkdir -p "$(dirname "${DEB_FILE}")"
if [[ ! -f "${DEB_FILE}" ]] || ! verify "${DEB_FILE}"; then
    rm -f "${DEB_FILE}"
    for url in "${URLS[@]}"; do
        echo "[qemu-kernel] Downloading ${url}"
        if ${FETCH} "${url}" -o "${DEB_FILE}.tmp" && verify "${DEB_FILE}.tmp"; then
            mv "${DEB_FILE}.tmp" "${DEB_FILE}"
            break
        fi
        echo "[qemu-kernel] WARN: download failed or checksum mismatch: ${url}" >&2
        rm -f "${DEB_FILE}.tmp"
    done
    [[ -f "${DEB_FILE}" ]] || { echo "[qemu-kernel] ERROR: could not fetch ${DEB_FILE##*/}" >&2; exit 1; }
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

echo "[qemu-kernel] Extracting kernel ${KV}"
(cd "${WORK}" && ar x "${DEB_FILE}")
data_tar="$(find "${WORK}" -maxdepth 1 -name 'data.tar*' | head -n 1)"
[[ -n "${data_tar}" ]] || { echo "[qemu-kernel] ERROR: no data.tar in ${DEB_FILE}" >&2; exit 1; }
mkdir -p "${WORK}/data"
tar -xf "${data_tar}" -C "${WORK}/data" --wildcards "./boot/vmlinuz-${KV}" "./usr/lib/modules/${KV}/*"

rm -rf "${DEST_DIR}"
mkdir -p "${DEST_DIR}/modules"
mv "${WORK}/data/boot/vmlinuz-${KV}" "${DEST_DIR}/vmlinuz"
mv "${WORK}/data/usr/lib/modules/${KV}" "${DEST_DIR}/modules/${KV}"
touch "${STAMP}"
echo "[qemu-kernel] Kernel ready in ${DEST_DIR}"
