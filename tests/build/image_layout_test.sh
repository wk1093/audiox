#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/bin" "${WORK}/out/bootfiles"
export IMAGE_TEST_LOG="${WORK}/commands"
cat > "${WORK}/bin/mock" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
tool=$(basename "$0")
if [[ "${tool}" == sudo ]]; then
    tool="$1"
    shift
fi
echo "${tool} $*" >> "${IMAGE_TEST_LOG}"
case "${tool}" in
    mountpoint) exit 1 ;;
    losetup)
        if [[ "$1" != -d ]]; then echo /dev/audiox-image-test; fi ;;
    dd|parted|mkfs.vfat|mkfs.ext4|mount|umount|cp|chmod) ;;
    *) echo "Unexpected mocked command: ${tool}" >&2; exit 1 ;;
esac
EOF
chmod +x "${WORK}/bin/mock"
for tool in sudo dd parted mountpoint; do
    ln -s mock "${WORK}/bin/${tool}"
done
truncate -s 60M "${WORK}/program"
truncate -s 1M "${WORK}/loader"
truncate -s 16M "${WORK}/out/bootfiles/kernel.img"
truncate -s 103M "${WORK}/ffmpeg"
touch "${WORK}/config" "${WORK}/cmdline"
build() {
    PATH="${WORK}/bin:${PATH}" bash "${ROOT}/tools/build/build_image.sh" \
        "${WORK}/image" "$1" "${WORK}/out" "${WORK}/loader" "${WORK}/program" \
        "${WORK}/config" "${WORK}/cmdline" "${2:-}"
}
build 512 "${WORK}/ffmpeg" > "${WORK}/output"
grep -q 'mkpart primary fat32 2048s 264191s' "${IMAGE_TEST_LOG}"
grep -q 'mkpart primary ext4 264192s 100%' "${IMAGE_TEST_LOG}"
grep -q 'SUCCESS!' "${WORK}/output"
grep -q '^IMG_SIZE_MB = 512$' "${ROOT}/Makefile"
grep -q 'P1_SECTORS=$(( 128 \* 2048 ))' "${ROOT}/tools/qemu/build_qemu_disk.sh"

# Refuse undersized images before touching an existing output, with or without FFmpeg.
for args in '256 ffmpeg' '192 none'; do
    read -r size converter <<< "${args}"
    : > "${IMAGE_TEST_LOG}"
    ffmpeg=""
    if [[ "${converter}" == ffmpeg ]]; then ffmpeg="${WORK}/ffmpeg"; fi
    if build "${size}" "${ffmpeg}" > "${WORK}/output" 2>&1; then
        echo "Expected undersized image rejection" >&2
        exit 1
    fi
    grep -q 'image size too small' "${WORK}/output"
    test ! -s "${IMAGE_TEST_LOG}"
done

truncate -s 120M "${WORK}/program"
: > "${IMAGE_TEST_LOG}"
if build 512 > "${WORK}/output" 2>&1; then
    echo "Expected oversized boot payload rejection" >&2
    exit 1
fi
grep -q 'boot payload requires' "${WORK}/output"
test ! -s "${IMAGE_TEST_LOG}"
echo "Image partition layout and preflight tests passed"
