#!/usr/bin/env bash
set -euo pipefail

KV="${1:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file> [list_prefix] [stage_dir_name]>}"
OUT_DIR="${2:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file> [list_prefix] [stage_dir_name]>}"
DEP_FILE="${3:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file> [list_prefix] [stage_dir_name]>}"
LIST_PREFIX="${4:-module-load}"
STAGE_DIR_NAME="${5:-modules_staging}"

FIRMWARE_DIR="${OUT_DIR}/firmware"
MOD_TREE_DIR="${FIRMWARE_DIR}/modules/${KV}"
SRC_KERNEL_DIR="${MOD_TREE_DIR}/kernel"
STAGE_BASE_DIR="${OUT_DIR}/${STAGE_DIR_NAME}/${KV}"
STAGE_KERNEL_DIR="${STAGE_BASE_DIR}/kernel"
BASE_LOAD_LIST_FILE="${OUT_DIR}/${LIST_PREFIX}.base.list"
NORMAL_LOAD_LIST_FILE="${OUT_DIR}/${LIST_PREFIX}.normal.list"
LOAD_LIST_FILE="${OUT_DIR}/${LIST_PREFIX}.list"
SUMMARY_FILE="${OUT_DIR}/${LIST_PREFIX}.summary.txt"

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
declare -A IS_BASE_SEED=()
declare -A IS_NORMAL_SEED=()
declare -A IS_SEED=()
declare -A IS_BUILTIN=()
ORDER=()
SEEDS=()
BASE_SEEDS=()
NORMAL_SEEDS=()

normalize_rel() {
    local rel="$1"
    rel="${rel#./}"
    rel="${rel#kernel/}"
    rel="${rel%.xz}"
    rel="${rel%.ko}"
    echo "${rel}"
}

register_module_rel() {
    local rel="$1"
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

register_module_file() {
    local full_path="$1"
    local rel="${full_path#${SRC_KERNEL_DIR}/}"
    register_module_rel "${rel}"
}

while IFS= read -r -d '' mod_file; do
    register_module_file "${mod_file}"
done < <(find "${SRC_KERNEL_DIR}" -type f \( -name '*.ko' -o -name '*.ko.xz' \) -print0)

if [[ -f "${MOD_TREE_DIR}/modules.builtin" ]]; then
    while IFS= read -r builtin_rel; do
        [[ -z "${builtin_rel}" ]] && continue
        builtin_rel="${builtin_rel#kernel/}"
        register_module_rel "${builtin_rel}"
        IS_BUILTIN["$(normalize_rel "${builtin_rel}")"]=1
    done < "${MOD_TREE_DIR}/modules.builtin"
fi

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

    if [[ -n "${IS_BUILTIN[${rel}]:-}" ]]; then
        STATE["${rel}"]="done"
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

    seed_class="normal"
    if [[ "${line}" == base:* ]]; then
        seed_class="base"
        line="${line#base:}"
        line="${line#${line%%[![:space:]]*}}"
    elif [[ "${line}" == normal:* ]]; then
        line="${line#normal:}"
        line="${line#${line%%[![:space:]]*}}"
    fi

    [[ -z "${line}" ]] && continue

    normalized_seed="$(normalize_rel "${line}")"
    if [[ -n "${IS_SEED[${normalized_seed}]:-}" ]]; then
        if [[ "${seed_class}" == "base" && -z "${IS_BASE_SEED[${normalized_seed}]:-}" ]]; then
            IS_BASE_SEED["${normalized_seed}"]=1
            BASE_SEEDS+=("${normalized_seed}")
        fi
        continue
    fi

    IS_SEED["${normalized_seed}"]=1
    SEEDS+=("${normalized_seed}")

    if [[ "${seed_class}" == "base" ]]; then
        IS_BASE_SEED["${normalized_seed}"]=1
        BASE_SEEDS+=("${normalized_seed}")
    else
        IS_NORMAL_SEED["${normalized_seed}"]=1
        NORMAL_SEEDS+=("${normalized_seed}")
    fi

done < "${DEP_FILE}"

for base_seed in "${BASE_SEEDS[@]}"; do
    visit_module "${base_seed}"
done

BASE_ORDER_COUNT="${#ORDER[@]}"

for normal_seed in "${NORMAL_SEEDS[@]}"; do
    visit_module "${normal_seed}"
done

: > "${BASE_LOAD_LIST_FILE}"
: > "${NORMAL_LOAD_LIST_FILE}"
: > "${LOAD_LIST_FILE}"

for idx in "${!ORDER[@]}"; do
    rel="${ORDER[${idx}]}"
    src_file="$(resolve_source_file "${rel}")"
    dst_file="${STAGE_KERNEL_DIR}/${rel}.ko"
    mkdir -p "$(dirname "${dst_file}")"

    if [[ "${src_file}" == *.xz ]]; then
        xz -dc "${src_file}" > "${dst_file}"
    else
        install -m 0644 "${src_file}" "${dst_file}"
    fi

    module_path="/lib/modules/${KV}/kernel/${rel}.ko"

    if (( idx < BASE_ORDER_COUNT )); then
        echo "${module_path}" >> "${BASE_LOAD_LIST_FILE}"
    else
        echo "${module_path}" >> "${NORMAL_LOAD_LIST_FILE}"
    fi

    echo "${module_path}" >> "${LOAD_LIST_FILE}"
done

{
    echo "Kernel Version: ${KV}"
    echo "Seed Count: ${#SEEDS[@]}"
    echo "Base Seed Count: ${#BASE_SEEDS[@]}"
    echo "Normal Seed Count: ${#NORMAL_SEEDS[@]}"
    echo "Resolved Module Count: ${#ORDER[@]}"
    echo "Base Load Count: ${BASE_ORDER_COUNT}"
    echo "Normal Load Count: $((${#ORDER[@]} - BASE_ORDER_COUNT))"
    echo
    echo "Base Seed Modules:"
    for seed in "${BASE_SEEDS[@]}"; do
        echo "  ${seed}"
    done
    echo
    echo "Normal Seed Modules:"
    for seed in "${NORMAL_SEEDS[@]}"; do
        echo "  ${seed}"
    done
    echo
    echo "Resolved Load Order:"
    for idx in "${!ORDER[@]}"; do
        rel="${ORDER[${idx}]}"
        if (( idx < BASE_ORDER_COUNT )); then
            if [[ -n "${IS_BASE_SEED[${rel}]:-}" ]]; then
                echo "  [base-seed ] ${rel}"
            else
                echo "  [base-dep  ] ${rel}"
            fi
        else
            if [[ -n "${IS_NORMAL_SEED[${rel}]:-}" ]]; then
                echo "  [normal-seed] ${rel}"
            else
                echo "  [normal-dep ] ${rel}"
            fi
        fi
    done
} > "${SUMMARY_FILE}"

echo "[resolve_modules] Staged ${#ORDER[@]} modules from ${#SEEDS[@]} seeds"
echo "[resolve_modules] Base list: ${BASE_LOAD_LIST_FILE}"
echo "[resolve_modules] Normal list: ${NORMAL_LOAD_LIST_FILE}"
echo "[resolve_modules] Module load list: ${LOAD_LIST_FILE}"
echo "[resolve_modules] Summary report: ${SUMMARY_FILE}"
