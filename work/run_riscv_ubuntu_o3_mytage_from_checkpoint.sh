#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

usage() {
    cat <<'USAGE'
Restore a RISC-V Ubuntu checkpoint, switch to O3, and run CoreMark with MyTAGE.

Examples:
  bash work/run_riscv_ubuntu_o3_mytage_from_checkpoint.sh --checkpoint-dir m5out/riscv-ubuntu-atomic-checkpoint
  bash work/run_riscv_ubuntu_atomic_checkpoint.sh --iterations 200
  bash work/run_riscv_ubuntu_o3_mytage_from_checkpoint.sh --checkpoint-dir m5out/riscv-ubuntu-atomic-checkpoint/cpt.123456
  FETCH_WIDTH=4 ENTRY_PER_SET=4 bash work/run_riscv_ubuntu_o3_mytage_from_checkpoint.sh --checkpoint-dir m5out/riscv-ubuntu-atomic-checkpoint

Options:
  --checkpoint-dir DIR          Checkpoint parent dir or direct cpt.<tick> dir
  --checkpoint-restore N        gem5 restore ordinal. Default: latest cpt.* in parent
  --iterations N                Compatibility option. Use it when taking the checkpoint.
  --outdir DIR                  gem5 output directory
  --kernel PATH                 Bootloader+vmlinux image
  --disk-image PATH             Ubuntu disk image
  --image-files-dir DIR         Host dir containing kernel/disk images
  --gem5-bin PATH               gem5 binary. Default: build/RISCV/gem5.opt
  --mem-size SIZE               Memory size. Default: 2GB
  --tage-param "field = value"  Extra system.switch_cpus[0].branchPred.<field> param
  --param "expr"                Raw gem5 --param expression
  --fs-arg ARG                  Extra argument passed after fs_linux.py
  --gem5-arg ARG                Extra argument passed before fs_linux.py
USAGE
}

py_bool() {
    local value="${1}"
    case "${value,,}" in
        1|true|yes|on) echo "True" ;;
        0|false|no|off) echo "False" ;;
        *) echo "${value}" ;;
    esac
}

first_existing() {
    for path in "$@"; do
        if [[ -n "${path}" && -e "${path}" ]]; then
            printf '%s\n' "${path}"
            return 0
        fi
    done
    return 1
}

auto_kernel() {
    first_existing \
        "${KERNEL:-}" \
        "${IMAGE_FILES_DIR}/bootloader-vmlinux-5.10" \
        "work/bootloader-vmlinux-5.10" \
        "bootloader-vmlinux-5.10" \
        "work/riscv-bootloader-vmlinux-5.10" \
        "riscv-bootloader-vmlinux-5.10" || true
}

auto_disk() {
    first_existing \
        "${DISK_IMAGE:-}" \
        "${IMAGE_FILES_DIR}/ubuntu-riscv-2gb.raw.img" \
        "${IMAGE_FILES_DIR}/ubuntu-riscv.raw.img" \
        "${IMAGE_FILES_DIR}/ubuntu-riscv-min.raw.img" \
        "work/ubuntu-riscv-2gb.raw.img" \
        "ubuntu-riscv-2gb.raw.img" \
        "work/ubuntu-riscv.raw.img" \
        "ubuntu-riscv.raw.img" || true
}

resolve_checkpoint_selection() {
    local input_dir="$1"
    local requested="$2"
    local parent="${input_dir}"
    local direct_cpt=""

    if [[ "$(basename "${input_dir}")" == cpt.* ]]; then
        direct_cpt="$(basename "${input_dir}")"
        parent="$(dirname "${input_dir}")"
    fi

    if [[ ! -d "${parent}" ]]; then
        echo "error: checkpoint parent does not exist: ${parent}" >&2
        return 1
    fi

    mapfile -t cpts < <(
        find "${parent}" -maxdepth 1 -type d -name 'cpt.*' -printf '%f\n' |
        sort -V
    )
    if [[ "${#cpts[@]}" -eq 0 ]]; then
        echo "error: no cpt.* directories under: ${parent}" >&2
        return 1
    fi

    if [[ -n "${requested}" ]]; then
        CHECKPOINT_PARENT="${parent}"
        CHECKPOINT_RESTORE="${requested}"
        return 0
    fi

    if [[ -n "${direct_cpt}" ]]; then
        for i in "${!cpts[@]}"; do
            if [[ "${cpts[$i]}" == "${direct_cpt}" ]]; then
                CHECKPOINT_PARENT="${parent}"
                CHECKPOINT_RESTORE="$((i + 1))"
                return 0
            fi
        done
        echo "error: checkpoint ${direct_cpt} not found under ${parent}" >&2
        return 1
    fi

    CHECKPOINT_PARENT="${parent}"
    CHECKPOINT_RESTORE="${#cpts[@]}"
}

GEM5_BIN="${GEM5_BIN:-build/RISCV/gem5.opt}"
FS_CONFIG="${FS_CONFIG:-configs/example/riscv/fs_linux.py}"
IMAGE_FILES_DIR="${IMAGE_FILES_DIR:-/home/tarator/github_repos/gem5-boot-tests-image-files}"
KERNEL="${KERNEL:-}"
DISK_IMAGE="${DISK_IMAGE:-}"
CHECKPOINT_DIR="${CHECKPOINT_DIR:-m5out/riscv-ubuntu-atomic-checkpoint}"
CHECKPOINT_RESTORE="${CHECKPOINT_RESTORE:-}"
OUTDIR="${OUTDIR:-}"

REQUESTED_ITERATIONS="${ITERATIONS:-}"

CPU_TYPE="${CPU_TYPE:-RiscvO3CPU}"
RESTORE_WITH_CPU="${RESTORE_WITH_CPU:-RiscvAtomicSimpleCPU}"
BP_TYPE="${BP_TYPE:-MyTAGE}"
INDIRECT_BP_TYPE="${INDIRECT_BP_TYPE:-SimpleIndirectPredictor}"

MEM_SIZE="${MEM_SIZE:-2GB}"
ROOT_DEVICE="${ROOT_DEVICE:-/dev/vda1}"
COMMAND_LINE="${COMMAND_LINE:-}"
SYS_CLOCK="${SYS_CLOCK:-1GHz}"
CPU_CLOCK="${CPU_CLOCK:-3GHz}"
CACHELINE_SIZE="${CACHELINE_SIZE:-64}"
L1I_SIZE="${L1I_SIZE:-32KiB}"
L1D_SIZE="${L1D_SIZE:-64KiB}"
L2_SIZE="${L2_SIZE:-2MiB}"
USE_CACHES="${USE_CACHES:-1}"
USE_L2CACHE="${USE_L2CACHE:-1}"
REL_MAX_TICK="${REL_MAX_TICK:-}"
MAXINSTS="${MAXINSTS:-}"

FETCH_WIDTH="${FETCH_WIDTH:-4}"
DECODE_WIDTH="${DECODE_WIDTH:-4}"
ISSUE_WIDTH="${ISSUE_WIDTH:-4}"
COMMIT_WIDTH="${COMMIT_WIDTH:-4}"

BIMODAL_DEPTH="${BIMODAL_DEPTH:-256}"
GHISTORY_LENGTH="${GHISTORY_LENGTH:-250}"
PCHISTORY_LENGTH="${PCHISTORY_LENGTH:-32}"
COMP_COUNT="${COMP_COUNT:-4}"
ENTRY_PER_SET="${ENTRY_PER_SET:-2}"
FETCH_LINE_BYTES="${FETCH_LINE_BYTES:-16}"
BIMODAL_CTRS_PER_ROW="${BIMODAL_CTRS_PER_ROW:-8}"
BIMODAL_OFFSET_SHIFT="${BIMODAL_OFFSET_SHIFT:-1}"
BIMODAL_USE_ALIGNED_ADDR="${BIMODAL_USE_ALIGNED_ADDR:-True}"
TAG_USE_ALIGNED_ADDR="${TAG_USE_ALIGNED_ADDR:-True}"

PC_HASH_START_FOR_IDX="${PC_HASH_START_FOR_IDX:-2}"
PC_HASH_WIDTH_FOR_IDX="${PC_HASH_WIDTH_FOR_IDX:-10}"
PC_HASH_START_FOR_TAG="${PC_HASH_START_FOR_TAG:-10}"
PC_HASH_WIDTH_FOR_TAG="${PC_HASH_WIDTH_FOR_TAG:-12}"

ALLOCATE_RANDOM_PLACEMENT="${ALLOCATE_RANDOM_PLACEMENT:-True}"
ALLOCATE_LONGER_THAN_PROVIDER="${ALLOCATE_LONGER_THAN_PROVIDER:-True}"
PERIODIC_RESET="${PERIODIC_RESET:-False}"
PERIODIC_RESET_BRANCH_PERIOD="${PERIODIC_RESET_BRANCH_PERIOD:-262144}"
USE_LFSR="${USE_LFSR:-True}"
LFSR_WIDTH="${LFSR_WIDTH:-32}"
LFSR_SEED="${LFSR_SEED:-0xACE1}"
LFSR_MISPREDICTION_UPDATE="${LFSR_MISPREDICTION_UPDATE:-True}"
HISTORY_HASH_TYPE="${HISTORY_HASH_TYPE:-CIRCULAR_SHIFT_REGISTER}"
REPLACEMENT_MODE="${REPLACEMENT_MODE:-USEFUL}"
USE_ALT_ON_NA="${USE_ALT_ON_NA:-True}"
NUM_USE_ALT_ON_NA="${NUM_USE_ALT_ON_NA:-16}"
USE_ALT_ON_NA_BITS="${USE_ALT_ON_NA_BITS:-4}"
USE_ALT_ON_NA_HASH_START="${USE_ALT_ON_NA_HASH_START:-2}"
USE_ALT_ON_NA_HASH_WIDTH="${USE_ALT_ON_NA_HASH_WIDTH:-4}"

TABLE_DEPTH="${TABLE_DEPTH:-[256, 256, 256, 256]}"
TABLE_USEFUL_WIDTH="${TABLE_USEFUL_WIDTH:-[2, 2, 2, 2]}"
TABLE_CTR_WIDTH="${TABLE_CTR_WIDTH:-[3, 3, 3, 3]}"
TABLE_TAG_WIDTH="${TABLE_TAG_WIDTH:-[8, 8, 9, 9]}"
TABLE_HISTORY_WIDTH="${TABLE_HISTORY_WIDTH:-[5, 18, 68, 250]}"
TABLE_PC_HISTORY_START="${TABLE_PC_HISTORY_START:-[0, 0, 0, 0]}"
TABLE_PC_HISTORY_WIDTH="${TABLE_PC_HISTORY_WIDTH:-[3, 5, 16, 16]}"

GEM5_STDOUT="${GEM5_STDOUT:-gem5.stdout}"
GEM5_STDERR="${GEM5_STDERR:-gem5.stderr}"

EXTRA_GEM5_ARGS=()
EXTRA_FS_ARGS=()
EXTRA_PARAM_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --checkpoint-dir)
            CHECKPOINT_DIR="$2"
            shift 2
            ;;
        --checkpoint-restore)
            CHECKPOINT_RESTORE="$2"
            shift 2
            ;;
        --iterations)
            REQUESTED_ITERATIONS="$2"
            shift 2
            ;;
        --outdir)
            OUTDIR="$2"
            shift 2
            ;;
        --kernel)
            KERNEL="$2"
            shift 2
            ;;
        --disk-image)
            DISK_IMAGE="$2"
            shift 2
            ;;
        --image-files-dir)
            IMAGE_FILES_DIR="$2"
            shift 2
            ;;
        --gem5-bin)
            GEM5_BIN="$2"
            shift 2
            ;;
        --mem-size)
            MEM_SIZE="$2"
            shift 2
            ;;
        --fetch-width)
            FETCH_WIDTH="$2"
            shift 2
            ;;
        --decode-width)
            DECODE_WIDTH="$2"
            shift 2
            ;;
        --issue-width)
            ISSUE_WIDTH="$2"
            shift 2
            ;;
        --commit-width)
            COMMIT_WIDTH="$2"
            shift 2
            ;;
        --entry-per-set)
            ENTRY_PER_SET="$2"
            shift 2
            ;;
        --fetch-line-bytes)
            FETCH_LINE_BYTES="$2"
            shift 2
            ;;
        --bimodal-ctrs-per-row)
            BIMODAL_CTRS_PER_ROW="$2"
            shift 2
            ;;
        --bimodal-offset-shift)
            BIMODAL_OFFSET_SHIFT="$2"
            shift 2
            ;;
        --history-hash-type)
            HISTORY_HASH_TYPE="$2"
            shift 2
            ;;
        --replacement-mode)
            REPLACEMENT_MODE="$2"
            shift 2
            ;;
        --tage-param)
            EXTRA_PARAM_ARGS+=(--param "system.switch_cpus[0].branchPred.$2")
            shift 2
            ;;
        --param)
            EXTRA_PARAM_ARGS+=(--param "$2")
            shift 2
            ;;
        --fs-arg)
            EXTRA_FS_ARGS+=("$2")
            shift 2
            ;;
        --gem5-arg)
            EXTRA_GEM5_ARGS+=("$2")
            shift 2
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

KERNEL="$(auto_kernel)"
DISK_IMAGE="$(auto_disk)"
resolve_checkpoint_selection "${CHECKPOINT_DIR}" "${CHECKPOINT_RESTORE}"

if [[ -z "${OUTDIR}" ]]; then
    OUTDIR="m5out/riscv-ubuntu-o3-mytage-coremark-$(date +%Y%m%d-%H%M%S)"
fi
if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "error: gem5 binary not found or not executable: ${GEM5_BIN}" >&2
    echo "hint: build it with: scons -Q -j12 build/RISCV/gem5.opt" >&2
    exit 1
fi
if [[ -z "${KERNEL}" || ! -f "${KERNEL}" ]]; then
    echo "error: bootloader-vmlinux-5.10 not found." >&2
    echo "hint: pass --kernel /path/to/bootloader-vmlinux-5.10" >&2
    exit 1
fi
if [[ -z "${DISK_IMAGE}" || ! -f "${DISK_IMAGE}" ]]; then
    echo "error: Ubuntu disk image not found." >&2
    echo "hint: pass --disk-image /path/to/ubuntu-riscv-2gb.raw.img" >&2
    exit 1
fi

mkdir -p "${OUTDIR}"

if [[ -z "${COMMAND_LINE}" ]]; then
    COMMAND_LINE="console=ttyS0 root=${ROOT_DEVICE} rw rootwait init=/sbin/init systemd.unit=multi-user.target"
fi

FS_ARGS=(
    "${FS_CONFIG}"
    --kernel="${KERNEL}"
    --disk-image="${DISK_IMAGE}"
    --cpu-type="${CPU_TYPE}"
    --restore-with-cpu="${RESTORE_WITH_CPU}"
    --checkpoint-dir="${CHECKPOINT_PARENT}"
    --checkpoint-restore="${CHECKPOINT_RESTORE}"
    --bp-type="${BP_TYPE}"
    --indirect-bp-type="${INDIRECT_BP_TYPE}"
    --num-cpus=1
    --mem-size="${MEM_SIZE}"
    --sys-clock="${SYS_CLOCK}"
    --cpu-clock="${CPU_CLOCK}"
    --cacheline_size="${CACHELINE_SIZE}"
    --l1i_size="${L1I_SIZE}"
    --l1d_size="${L1D_SIZE}"
    --l2_size="${L2_SIZE}"
    --root-device="${ROOT_DEVICE}"
    --command-line="${COMMAND_LINE}"
)

if [[ "${USE_CACHES}" == "1" || "${USE_CACHES,,}" == "true" ]]; then
    FS_ARGS+=(--caches)
fi
if [[ "${USE_L2CACHE}" == "1" || "${USE_L2CACHE,,}" == "true" ]]; then
    FS_ARGS+=(--l2cache)
fi
if [[ -n "${REL_MAX_TICK}" ]]; then
    FS_ARGS+=(--rel-max-tick="${REL_MAX_TICK}")
fi
if [[ -n "${MAXINSTS}" ]]; then
    FS_ARGS+=(-I "${MAXINSTS}")
fi
FS_ARGS+=("${EXTRA_FS_ARGS[@]}")

CPU_PARAM_ARGS=(
    --param "system.switch_cpus[0].fetchWidth = ${FETCH_WIDTH}"
    --param "system.switch_cpus[0].decodeWidth = ${DECODE_WIDTH}"
    --param "system.switch_cpus[0].issueWidth = ${ISSUE_WIDTH}"
    --param "system.switch_cpus[0].commitWidth = ${COMMIT_WIDTH}"
)

TAGE_PARAM_ARGS=(
    --param "system.switch_cpus[0].branchPred.bimodalDepth = ${BIMODAL_DEPTH}"
    --param "system.switch_cpus[0].branchPred.ghistoryLength = ${GHISTORY_LENGTH}"
    --param "system.switch_cpus[0].branchPred.pchistoryLength = ${PCHISTORY_LENGTH}"
    --param "system.switch_cpus[0].branchPred.compCount = ${COMP_COUNT}"
    --param "system.switch_cpus[0].branchPred.entryPerSet = ${ENTRY_PER_SET}"
    --param "system.switch_cpus[0].branchPred.fetchLineBytes = ${FETCH_LINE_BYTES}"
    --param "system.switch_cpus[0].branchPred.bimodalCtrsPerRow = ${BIMODAL_CTRS_PER_ROW}"
    --param "system.switch_cpus[0].branchPred.bimodalOffsetShift = ${BIMODAL_OFFSET_SHIFT}"
    --param "system.switch_cpus[0].branchPred.bimodalUseAlignedAddr = $(py_bool "${BIMODAL_USE_ALIGNED_ADDR}")"
    --param "system.switch_cpus[0].branchPred.tagUseAlignedAddr = $(py_bool "${TAG_USE_ALIGNED_ADDR}")"
    --param "system.switch_cpus[0].branchPred.pcHashStartForIdx = ${PC_HASH_START_FOR_IDX}"
    --param "system.switch_cpus[0].branchPred.pcHashWidthForIdx = ${PC_HASH_WIDTH_FOR_IDX}"
    --param "system.switch_cpus[0].branchPred.pcHashStartForTag = ${PC_HASH_START_FOR_TAG}"
    --param "system.switch_cpus[0].branchPred.pcHashWidthForTag = ${PC_HASH_WIDTH_FOR_TAG}"
    --param "system.switch_cpus[0].branchPred.allocateRandomPlacement = $(py_bool "${ALLOCATE_RANDOM_PLACEMENT}")"
    --param "system.switch_cpus[0].branchPred.allocateLongerThanProvider = $(py_bool "${ALLOCATE_LONGER_THAN_PROVIDER}")"
    --param "system.switch_cpus[0].branchPred.periodicReset = $(py_bool "${PERIODIC_RESET}")"
    --param "system.switch_cpus[0].branchPred.periodicResetBranchPeriod = ${PERIODIC_RESET_BRANCH_PERIOD}"
    --param "system.switch_cpus[0].branchPred.useLfsr = $(py_bool "${USE_LFSR}")"
    --param "system.switch_cpus[0].branchPred.lfsrWidth = ${LFSR_WIDTH}"
    --param "system.switch_cpus[0].branchPred.lfsrSeed = ${LFSR_SEED}"
    --param "system.switch_cpus[0].branchPred.lfsrMispredictionUpdate = $(py_bool "${LFSR_MISPREDICTION_UPDATE}")"
    --param "system.switch_cpus[0].branchPred.historyHashType = '${HISTORY_HASH_TYPE}'"
    --param "system.switch_cpus[0].branchPred.replacementMode = '${REPLACEMENT_MODE}'"
    --param "system.switch_cpus[0].branchPred.useAltOnNA = $(py_bool "${USE_ALT_ON_NA}")"
    --param "system.switch_cpus[0].branchPred.numUseAltOnNa = ${NUM_USE_ALT_ON_NA}"
    --param "system.switch_cpus[0].branchPred.useAltOnNaBits = ${USE_ALT_ON_NA_BITS}"
    --param "system.switch_cpus[0].branchPred.useAltOnNaHashStart = ${USE_ALT_ON_NA_HASH_START}"
    --param "system.switch_cpus[0].branchPred.useAltOnNaHashWidth = ${USE_ALT_ON_NA_HASH_WIDTH}"
    --param "system.switch_cpus[0].branchPred.tableDepth = ${TABLE_DEPTH}"
    --param "system.switch_cpus[0].branchPred.tableUsefulWidth = ${TABLE_USEFUL_WIDTH}"
    --param "system.switch_cpus[0].branchPred.tableCtrWidth = ${TABLE_CTR_WIDTH}"
    --param "system.switch_cpus[0].branchPred.tableTagWidth = ${TABLE_TAG_WIDTH}"
    --param "system.switch_cpus[0].branchPred.tableHistoryWidth = ${TABLE_HISTORY_WIDTH}"
    --param "system.switch_cpus[0].branchPred.tablePcHistoryStart = ${TABLE_PC_HISTORY_START}"
    --param "system.switch_cpus[0].branchPred.tablePcHistoryWidth = ${TABLE_PC_HISTORY_WIDTH}"
)

CMD=(
    "${GEM5_BIN}"
    --outdir="${OUTDIR}"
    "${EXTRA_GEM5_ARGS[@]}"
    "${FS_ARGS[@]}"
    "${CPU_PARAM_ARGS[@]}"
    "${TAGE_PARAM_ARGS[@]}"
    "${EXTRA_PARAM_ARGS[@]}"
)

printf '%q ' "${CMD[@]}" > "${OUTDIR}/command.txt"
printf '\n' >> "${OUTDIR}/command.txt"

echo "[gem5] outdir: ${OUTDIR}"
echo "[gem5] checkpoint parent: ${CHECKPOINT_PARENT}"
echo "[gem5] checkpoint restore index: ${CHECKPOINT_RESTORE}"
echo "[gem5] stdout: ${OUTDIR}/${GEM5_STDOUT}"
echo "[gem5] stderr: ${OUTDIR}/${GEM5_STDERR}"
echo "[guest] workload: image /work/run.sh resumes after ./checkpointer"
echo "[guest] console: ${OUTDIR}/system.pc.com_1.device"
if [[ -n "${REQUESTED_ITERATIONS}" ]]; then
    echo "[guest] note: --iterations is baked when taking the checkpoint; current value requested: ${REQUESTED_ITERATIONS}"
fi
echo "[mytage] entryPerSet=${ENTRY_PER_SET} bimodalCtrsPerRow=${BIMODAL_CTRS_PER_ROW} fetchWidth=${FETCH_WIDTH}"

set +e
"${CMD[@]}" >"${OUTDIR}/${GEM5_STDOUT}" 2>"${OUTDIR}/${GEM5_STDERR}"
status=$?
set -e

echo "[gem5] exit status: ${status}"
if [[ -s "${OUTDIR}/stats.txt" ]]; then
    echo "[gem5] stats written: ${OUTDIR}/stats.txt"
else
    echo "[gem5] warning: stats.txt missing or empty." >&2
fi

exit "${status}"
