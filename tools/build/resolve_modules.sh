#!/usr/bin/env bash
set -euo pipefail

KV="${1:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file> [list_prefix] [stage_dir_name] [board] [modules_root]}"
OUT_DIR="${2:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file> [list_prefix] [stage_dir_name] [board] [modules_root]}"
DEP_FILE="${3:?usage: resolve_modules.sh <kernel_version> <out_dir> <dep_file> [list_prefix] [stage_dir_name] [board] [modules_root]}"
LIST_PREFIX="${4:-module-load}"
STAGE_DIR_NAME="${5:-modules_staging}"
BOARD="${6:-}"
# Directory holding <kernel_version>/kernel/...; defaults to the Raspberry Pi firmware tree.
MODULES_ROOT="${7:-${OUT_DIR}/firmware/modules}"
KNOWN_BOARDS=(pi4 pi5 qemu)

if [[ -n "${BOARD}" && " ${KNOWN_BOARDS[*]} " != *" ${BOARD} "* ]]; then
    echo "[resolve_modules] ERROR: unknown board '${BOARD}' (known: ${KNOWN_BOARDS[*]})" >&2
    exit 1
fi

MOD_TREE_DIR="${MODULES_ROOT}/${KV}"
SRC_KERNEL_DIR="${MOD_TREE_DIR}/kernel"
STAGE_BASE_DIR="${OUT_DIR}/${STAGE_DIR_NAME}/${KV}"
STAGE_KERNEL_DIR="${STAGE_BASE_DIR}/kernel"
BASE_LOAD_LIST_FILE="${OUT_DIR}/${LIST_PREFIX}.base.list"
NORMAL_LOAD_LIST_FILE="${OUT_DIR}/${LIST_PREFIX}.normal.list"
LOAD_LIST_FILE="${OUT_DIR}/${LIST_PREFIX}.list"
SUMMARY_FILE="${OUT_DIR}/${LIST_PREFIX}.summary.txt"

if [[ ! -d "${SRC_KERNEL_DIR}" ]]; then
    echo "[resolve_modules] ERROR: module tree not found: ${SRC_KERNEL_DIR}" >&2
    echo "[resolve_modules] Ensure ${MODULES_ROOT} has ${KV}." >&2
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

# Stale metadata from older resolver versions described the whole kernel tree.
rm -f "${STAGE_BASE_DIR}"/modules.*
if [[ -f "${MOD_TREE_DIR}/modules.builtin" ]]; then
    install -m 0644 "${MOD_TREE_DIR}/modules.builtin" "${STAGE_BASE_DIR}/modules.builtin"
fi

declare -A NAME_TO_REL=()
declare -A STATE=()
declare -A IS_BASE_SEED=()
declare -A IS_NORMAL_SEED=()
declare -A IS_AUTO_SEED=()
declare -A DEPS=()
declare -A IS_SEED=()
declare -A IS_BUILTIN=()
ORDER=()
SEEDS=()
BASE_SEEDS=()
NORMAL_SEEDS=()
AUTO_SEEDS=()
ALL_RELS=()

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
    ALL_RELS+=("$(normalize_rel "${rel}")")
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
        if [[ -z "${IS_BUILTIN[${dep_rel}]:-}" ]]; then
            DEPS["${rel}"]+="${DEPS[${rel}]:+ }${dep_rel}"
        fi
    done

    STATE["${rel}"]="done"
    ORDER+=("${rel}")
}

# Seed line syntax: [<class>[(<boards>)]:] <module>
#   class: base | normal | auto (default normal)
#     base/normal: loaded unconditionally by init from the base/normal lists.
#     auto: staged only; init loads them when matching hardware appears
#       (modalias coldplug and uevent hotplug via modules.alias).
#   module: a path below kernel/ without extension, or for auto seeds a glob
#     (e.g. "drivers/gpu/drm/*"; "*" also matches "/"). A glob that matches no
#     module is an error.
#   boards: comma-separated board names; "!name" excludes a board. With any
#   positive name the seed applies only to the listed boards.
#   e.g. "base(pi5): drivers/foo", "normal(!pi5): drivers/bar"
SEED_RE='^(base|normal|auto)(\(([^()]*)\))?:[[:space:]]*(.*)$'
SKIPPED_SEEDS=()

# Returns 0 if the board list applies to ${BOARD}; exits on malformed lists.
board_matches() {
    local list="$1" line_no="$2" item name
    local has_positive=0 positive_match=0
    IFS=',' read -r -a items <<< "${list}"
    if [[ "${#items[@]}" -eq 0 ]]; then
        echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: empty board list" >&2
        exit 1
    fi
    for item in "${items[@]}"; do
        item="${item//[[:space:]]/}"
        name="${item#!}"
        if [[ -z "${name}" || " ${KNOWN_BOARDS[*]} " != *" ${name} "* ]]; then
            echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: unknown board '${item}' (known: ${KNOWN_BOARDS[*]})" >&2
            exit 1
        fi
        if [[ -z "${BOARD}" ]]; then
            echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: board-specific seed but no board given" >&2
            exit 1
        fi
        if [[ "${item}" == !* ]]; then
            [[ "${name}" == "${BOARD}" ]] && return 1
        else
            has_positive=1
            [[ "${name}" == "${BOARD}" ]] && positive_match=1
        fi
    done
    (( ! has_positive || positive_match ))
}

line_no=0
while IFS= read -r raw_line || [[ -n "${raw_line}" ]]; do
    line_no=$((line_no + 1))
    line="${raw_line%%#*}"
    line="${line#${line%%[![:space:]]*}}"
    line="${line%${line##*[![:space:]]}}"
    [[ -z "${line}" ]] && continue

    seed_class="normal"
    if [[ "${line}" =~ ${SEED_RE} ]]; then
        seed_class="${BASH_REMATCH[1]}"
        board_list="${BASH_REMATCH[3]}"
        has_board_list="${BASH_REMATCH[2]}"
        line="${BASH_REMATCH[4]}"
        if [[ -n "${has_board_list}" ]] && ! board_matches "${board_list}" "${line_no}"; then
            SKIPPED_SEEDS+=("${raw_line#${raw_line%%[![:space:]]*}}")
            continue
        fi
    elif [[ "${line}" == *:* ]]; then
        echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: unrecognized seed prefix in '${line}'" >&2
        echo "[resolve_modules] Use base:, normal:, auto: with an optional (<boards>) list" >&2
        exit 1
    fi

    if [[ -z "${line}" ]]; then
        echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: missing module path" >&2
        exit 1
    fi

    if [[ "${line}" == *[\*\?\[]* ]]; then
        if [[ "${seed_class}" != "auto" ]]; then
            echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: globs are only allowed for auto: seeds" >&2
            exit 1
        fi
        matched=0
        for candidate in "${ALL_RELS[@]}"; do
            # shellcheck disable=SC2053
            if [[ "${candidate}" == ${line} ]]; then
                matched=1
                if [[ -z "${IS_SEED[${candidate}]:-}" ]]; then
                    IS_SEED["${candidate}"]=1
                    IS_AUTO_SEED["${candidate}"]=1
                    AUTO_SEEDS+=("${candidate}")
                fi
            fi
        done
        if (( ! matched )); then
            echo "[resolve_modules] ERROR: ${DEP_FILE}:${line_no}: glob '${line}' matches no module" >&2
            exit 1
        fi
        continue
    fi

    normalized_seed="$(normalize_rel "${line}")"
    if [[ "${seed_class}" == "auto" ]]; then
        if [[ -n "${IS_BUILTIN[${normalized_seed}]:-}" ]]; then
            continue
        fi
        if [[ -z "${IS_SEED[${normalized_seed}]:-}" ]]; then
            IS_SEED["${normalized_seed}"]=1
            IS_AUTO_SEED["${normalized_seed}"]=1
            AUTO_SEEDS+=("${normalized_seed}")
        fi
        continue
    fi
    if [[ -n "${IS_SEED[${normalized_seed}]:-}" && -z "${IS_AUTO_SEED[${normalized_seed}]:-}" ]]; then
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

# An explicit base/normal seed wins over an auto glob that also matched it.
FILTERED_AUTO=()
for auto_seed in "${AUTO_SEEDS[@]}"; do
    if [[ -n "${IS_BASE_SEED[${auto_seed}]:-}" || -n "${IS_NORMAL_SEED[${auto_seed}]:-}" ]]; then
        unset 'IS_AUTO_SEED[${auto_seed}]'
    else
        FILTERED_AUTO+=("${auto_seed}")
    fi
done
AUTO_SEEDS=("${FILTERED_AUTO[@]}")

for base_seed in "${BASE_SEEDS[@]}"; do
    visit_module "${base_seed}"
done

BASE_ORDER_COUNT="${#ORDER[@]}"

for normal_seed in "${NORMAL_SEEDS[@]}"; do
    visit_module "${normal_seed}"
done

LISTED_ORDER_COUNT="${#ORDER[@]}"

for auto_seed in "${AUTO_SEEDS[@]}"; do
    visit_module "${auto_seed}"
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

    if (( idx >= LISTED_ORDER_COUNT )); then
        continue
    fi

    if (( idx < BASE_ORDER_COUNT )); then
        echo "${module_path}" >> "${BASE_LOAD_LIST_FILE}"
    else
        echo "${module_path}" >> "${NORMAL_LOAD_LIST_FILE}"
    fi

    echo "${module_path}" >> "${LOAD_LIST_FILE}"
done

# Runtime metadata limited to staged modules. init's hardware loader reads
# modules.alias (modalias -> module), modules.dep (module -> dependencies,
# paths relative to the staged tree) and modules.softdep (pre/post modules).
module_name() {
    local name="${1##*/}"
    echo "${name//-/_}"
}

declare -A STAGED_NAME=()
for rel in "${ORDER[@]}"; do
    STAGED_NAME["$(module_name "${rel}")"]=1
done

{
    echo "# Dependencies of the staged modules, generated by resolve_modules.sh."
    for rel in "${ORDER[@]}"; do
        line="kernel/${rel}.ko:"
        for dep in ${DEPS[${rel}]:-}; do
            line+=" kernel/${dep}.ko"
        done
        echo "${line}"
    done
} > "${STAGE_BASE_DIR}/modules.dep"

ALIAS_COUNT=0
{
    echo "# Aliases of the staged modules, generated by resolve_modules.sh."
    if [[ -f "${MOD_TREE_DIR}/modules.alias" ]]; then
        while read -r keyword pattern name; do
            [[ "${keyword}" == "alias" && -n "${name}" && -n "${STAGED_NAME[${name}]:-}" ]] || continue
            echo "alias ${pattern} ${name}"
            ALIAS_COUNT=$((ALIAS_COUNT + 1))
        done < "${MOD_TREE_DIR}/modules.alias"
    fi
} > "${STAGE_BASE_DIR}/modules.alias"

if (( ${#AUTO_SEEDS[@]} > 0 )) && [[ ! -f "${MOD_TREE_DIR}/modules.alias" ]]; then
    echo "[resolve_modules] ERROR: auto seeds need ${MOD_TREE_DIR}/modules.alias" >&2
    exit 1
fi

{
    echo "# Soft dependencies of the staged modules, generated by resolve_modules.sh."
    if [[ -f "${MOD_TREE_DIR}/modules.softdep" ]]; then
        while read -r keyword name rest; do
            [[ "${keyword}" == "softdep" && -n "${STAGED_NAME[${name}]:-}" ]] || continue
            echo "softdep ${name} ${rest}"
        done < "${MOD_TREE_DIR}/modules.softdep"
    fi
} > "${STAGE_BASE_DIR}/modules.softdep"

{
    echo "Kernel Version: ${KV}"
    echo "Board: ${BOARD:-(any)}"
    echo "Seed Count: ${#SEEDS[@]}"
    echo "Base Seed Count: ${#BASE_SEEDS[@]}"
    echo "Normal Seed Count: ${#NORMAL_SEEDS[@]}"
    echo "Auto Seed Count: ${#AUTO_SEEDS[@]}"
    echo "Resolved Module Count: ${#ORDER[@]}"
    echo "Base Load Count: ${BASE_ORDER_COUNT}"
    echo "Normal Load Count: $((LISTED_ORDER_COUNT - BASE_ORDER_COUNT))"
    echo "Hardware-Loaded (auto) Count: $((${#ORDER[@]} - LISTED_ORDER_COUNT))"
    echo "Staged Alias Count: ${ALIAS_COUNT}"
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
    echo "Auto (hardware-loaded) Seed Modules:"
    for seed in "${AUTO_SEEDS[@]}"; do
        echo "  ${seed}"
    done
    echo
    echo "Skipped Seeds (other boards):"
    for seed in "${SKIPPED_SEEDS[@]}"; do
        echo "  ${seed}"
    done
    echo
    echo "Resolved Load Order:"
    for idx in "${!ORDER[@]}"; do
        rel="${ORDER[${idx}]}"
        if (( idx >= LISTED_ORDER_COUNT )); then
            if [[ -n "${IS_AUTO_SEED[${rel}]:-}" ]]; then
                echo "  [auto-seed  ] ${rel}"
            else
                echo "  [auto-dep   ] ${rel}"
            fi
        elif (( idx < BASE_ORDER_COUNT )); then
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

echo "[resolve_modules] Staged ${#ORDER[@]} modules from ${#SEEDS[@]} seeds (${#AUTO_SEEDS[@]} hardware-loaded seeds)"
echo "[resolve_modules] Base list: ${BASE_LOAD_LIST_FILE}"
echo "[resolve_modules] Normal list: ${NORMAL_LOAD_LIST_FILE}"
echo "[resolve_modules] Module load list: ${LOAD_LIST_FILE}"
echo "[resolve_modules] Summary report: ${SUMMARY_FILE}"
