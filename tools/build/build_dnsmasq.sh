#!/usr/bin/env bash
# Fetch, verify, and statically cross-compile dnsmasq for the runtime image.
set -euo pipefail

usage="usage: build_dnsmasq.sh <version> <sha256> <out_dir> <output_binary> [cc] [fetch_command]"
VERSION="${1:?${usage}}"
SHA256="${2:?${usage}}"
OUT_DIR="${3:?${usage}}"
OUTPUT="${4:?${usage}}"
CC="${5:-aarch64-linux-gnu-gcc}"
FETCH="${6:-curl -fsSL}"

URL="https://thekelleys.org.uk/dnsmasq/dnsmasq-${VERSION}.tar.xz"
TARBALL="${OUT_DIR}/downloads/dnsmasq-${VERSION}.tar.xz"
SRC_DIR="${OUT_DIR}/build/dnsmasq-${VERSION}"
STRIP="${CC%gcc}strip"

# Only the DHCPv4 server and DNS forwarder are used; everything else is compiled out.
# Static glibc warns about getpwnam/getgrnam; dnsmasq.conf sets empty user= and
# group= so those NSS lookups are never made on the device.
COPTS="-DNO_DHCP6 -DNO_TFTP -DNO_SCRIPT -DNO_AUTH -DNO_IPSET -DNO_LOOP -DNO_INOTIFY -DNO_DUMPFILE -DNO_DBUS -DNO_IDN -DNO_CONNTRACK"

mkdir -p "${OUT_DIR}/downloads" "${OUT_DIR}/build" "$(dirname "${OUTPUT}")"

if [[ ! -f "${TARBALL}" ]]; then
    echo "Fetching dnsmasq ${VERSION} from ${URL}..."
    ${FETCH} "${URL}" -o "${TARBALL}.tmp"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi

actual="$(sha256sum "${TARBALL}" | awk '{print $1}')"
if [[ "${actual}" != "${SHA256}" ]]; then
    echo "Error: dnsmasq archive checksum mismatch" >&2
    echo "Expected: ${SHA256}" >&2
    echo "Actual:   ${actual}" >&2
    rm -f "${TARBALL}"
    exit 1
fi

rm -rf "${SRC_DIR}"
tar -xJf "${TARBALL}" -C "${OUT_DIR}/build"

echo "Building static dnsmasq ${VERSION} with ${CC}..."
make -C "${SRC_DIR}" -j"$(nproc)" \
    CC="${CC}" \
    COPTS="${COPTS}" \
    CFLAGS="-Wall -W -O2" \
    LDFLAGS="-static" \
    PKG_CONFIG=/bin/false

cp "${SRC_DIR}/src/dnsmasq" "${OUTPUT}.tmp"
if command -v "${STRIP}" >/dev/null 2>&1; then
    "${STRIP}" "${OUTPUT}.tmp"
fi
chmod 0755 "${OUTPUT}.tmp"
cp "${SRC_DIR}/COPYING" "${SRC_DIR}/COPYING-v3" "$(dirname "${OUTPUT}")/"
mv "${OUTPUT}.tmp" "${OUTPUT}"
echo "Static dnsmasq ready: ${OUTPUT}"
