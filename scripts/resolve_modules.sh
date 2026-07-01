#!/usr/bin/env bash
set -euo pipefail

KV="${1:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file>}"
OUT_DIR="${2:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file>}"
DEP_FILE="${3:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file>}"

FIRMWARE_DIR="${OUT_DIR}/firmware"
MOD_TREE_DIR="${FIRMWARE_DIR}/modules/${KV}"
SRC_KERNEL_DIR="${MOD_TREE_DIR}/kernel"
STAGE_BASE_DIR="${OUT_DIR}/modules_staging/${KV}"
STAGE_KERNEL_DIR="${STAGE_BASE_DIR}/kernel"
LOAD_LIST_FILE="${OUT_DIR}/module-load.list"
SUMMARY_FILE="${OUT_DIR}/module-load.summary.txt"

if [[ ! -d "${SRC_KERNEL_DIR}" ]]; then
    echo "[resolve_modules] ERROR: module tree not found: ${SRC_KERNEL_DIR}" >&2
    echo "[resolve_modules] Ensure firmware has ${KV} under modules/." >&2
    exit 1
fi

if [[ ! -f "${DEP_FILE}" ]]; then
    echo "[resolve_modules] ERROR: dependency seed file missing: ${DEP_FILE}" >&2
    exit 1
fi

if ! command -v modinfo >/dev/null 2>&1; then
    echo "[resolve_modules] ERROR: 'modinfo' is required but not found in PATH" >&2
    exit 1
fi

if ! command -v xz >/dev/null 2>&1; then
    echo "[resolve_modules] ERROR: 'xz' is required but not found in PATH" >&2
    exit 1
fi

if ! command -v find >/dev/null 2>&1; then
    echo "[resolve_modules] ERROR: 'find' is required but not found in PATH" >&2
    exit 1
fi

mkdir -p "${STAGE_KERNEL_DIR}"
rm -rf "${STAGE_KERNEL_DIR}"
mkdir -p "${STAGE_KERNEL_DIR}"

for meta in modules.dep modules.dep.bin modules.alias modules.alias.bin modules.builtin modules.builtin.bin modules.order; do
    if [[ -f "${MOD_TREE_DIR}/${meta}" ]]; then
        install -m 0644 "${MOD_TREE_DIR}/${meta}" "${STAGE_BASE_DIR}/${meta}"
    fi
done

declare -A NAME_TO_REL=()
declare -A STATE=()
declare -A IS_SEED=()
ORDER=()
SEEDS=()

normalize_rel() {
    local rel="$1"
    rel="${rel#./}"
    rel="${rel#kernel/}"
    rel="${rel%.xz}"
    rel="${rel%.ko}"
    echo "${rel}"
}

register_module_file() {
    local full_path="$1"
    local rel="${full_path#${SRC_KERNEL_DIR}/}"
    local rel_no_ext
    rel_no_ext="$(normalize_rel "${rel}")"
    local base="${rel_no_ext##*/}"

    if [[ -z "${NAME_TO_REL[${base}]:-}" ]]; then
        NAME_TO_REL["${base}"]="${rel_no_ext}"
    fi

    local alt_underscore="${base//-/_}"
    if [[ -z "${NAME_TO_REL[${alt_underscore}]:-}" ]]; then
        NAME_TO_REL["${alt_underscore}"]="${rel_no_ext}"
    fi

    local alt_hyphen="${base//_/-}"
    if [[ -z "${NAME_TO_REL[${alt_hyphen}]:-}" ]]; then
        NAME_TO_REL["${alt_hyphen}"]="${rel_no_ext}"
    fi
}

while IFS= read -r -d '' mod_file; do
    register_module_file "${mod_file}"
done < <(find "${SRC_KERNEL_DIR}" -type f \( -name '*.ko' -o -name '*.ko.xz' \) -print0)

resolve_source_file() {
    local rel
    rel="$(normalize_rel "$1")"

    if [[ -f "${SRC_KERNEL_DIR}/${rel}.ko" ]]; then
        echo "${SRC_KERNEL_DIR}/${rel}.ko"
        return 0
    fi

    if [[ -f "${SRC_KERNEL_DIR}/${rel}.ko.xz" ]]; then
        echo "${SRC_KERNEL_DIR}/${rel}.ko.xz"
        return 0
    fi

    return 1
}

resolve_dep_to_rel() {
    local dep_name="$1"
    dep_name="${dep_name//[[:space:]]/}"

    if [[ -z "${dep_name}" ]]; then
        return 1
    fi

    if [[ "${dep_name}" == */* ]]; then
        local normalized
        normalized="$(normalize_rel "${dep_name}")"
        if resolve_source_file "${normalized}" >/dev/null; then
            echo "${normalized}"
            return 0
        fi
    fi

    if [[ -n "${NAME_TO_REL[${dep_name}]:-}" ]]; then
        echo "${NAME_TO_REL[${dep_name}]}"
        return 0
    fi

    local swap_hyphen="${dep_name//-/_}"
    if [[ -n "${NAME_TO_REL[${swap_hyphen}]:-}" ]]; then
        echo "${NAME_TO_REL[${swap_hyphen}]}"
        return 0
    fi

    local swap_underscore="${dep_name//_/-}"
    if [[ -n "${NAME_TO_REL[${swap_underscore}]:-}" ]]; then
        echo "${NAME_TO_REL[${swap_underscore}]}"
        return 0
    fi

    return 1
}

visit_module() {
    local rel
    rel="$(normalize_rel "$1")"

    if [[ "${STATE[${rel}]:-}" == "done" ]]; then
        return 0
    fi

    if [[ "${STATE[${rel}]:-}" == "visiting" ]]; then
        echo "[resolve_modules] WARN: dependency cycle around ${rel}; continuing" >&2
        return 0
    fi

    local src_file
    if ! src_file="$(resolve_source_file "${rel}")"; then
        echo "[resolve_modules] ERROR: module not found for ${rel}" >&2
        return 1
    fi

    STATE["${rel}"]="visiting"

    local depends
    depends="$(modinfo -F depends "${src_file}" 2>/dev/null || true)"

    IFS=',' read -r -a dep_items <<< "${depends}"
    local dep dep_rel
    for dep in "${dep_items[@]}"; do
        dep="${dep//[[:space:]]/}"
        [[ -z "${dep}" ]] && continue

        if ! dep_rel="$(resolve_dep_to_rel "${dep}")"; then
            echo "[resolve_modules] ERROR: unresolved dependency '${dep}' required by '${rel}'" >&2
            return 1
        fi

        visit_module "${dep_rel}"
    done

    STATE["${rel}"]="done"
    ORDER+=("${rel}")
}

while IFS= read -r raw_line; do
    line="${raw_line%%#*}"
    line="${line#${line%%[![:space:]]*}}"
    line="${line%${line##*[![:space:]]}}"
    [[ -z "${line}" ]] && continue

    normalized_seed="$(normalize_rel "${line}")"
    if [[ -n "${IS_SEED[${normalized_seed}]:-}" ]]; then
        continue
    fi
    IS_SEED["${normalized_seed}"]=1
    SEEDS+=("${normalized_seed}")

    visit_module "${normalized_seed}"
done < "${DEP_FILE}"

: > "${LOAD_LIST_FILE}"

for rel in "${ORDER[@]}"; do
    src_file="$(resolve_source_file "${rel}")"
    dst_file="${STAGE_KERNEL_DIR}/${rel}.ko"
    mkdir -p "$(dirname "${dst_file}")"

    if [[ "${src_file}" == *.xz ]]; then
        xz -dc "${src_file}" > "${dst_file}"
    else
        install -m 0644 "${src_file}" "${dst_file}"
    fi

    echo "/lib/modules/${KV}/kernel/${rel}.ko" >> "${LOAD_LIST_FILE}"
done

{
    echo "Kernel Version: ${KV}"
    echo "Seed Count: ${#SEEDS[@]}"
    echo "Resolved Module Count: ${#ORDER[@]}"
    echo
    echo "Seed Modules:"
    for seed in "${SEEDS[@]}"; do
        echo "  ${seed}"
    done
    echo
    echo "Resolved Load Order:"
    for rel in "${ORDER[@]}"; do
        if [[ -n "${IS_SEED[${rel}]:-}" ]]; then
            echo "  [seed] ${rel}"
        else
            echo "  [dep ] ${rel}"
        fi
    done
} > "${SUMMARY_FILE}"

echo "[resolve_modules] Staged ${#ORDER[@]} modules from ${#SEEDS[@]} seeds"
echo "[resolve_modules] Module load list: ${LOAD_LIST_FILE}"
echo "[resolve_modules] Summary report: ${SUMMARY_FILE}"
