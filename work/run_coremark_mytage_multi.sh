#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

usage() {
    cat <<'USAGE'
Run CoreMark in gem5 SE mode with the MyTAGE multi-entry predictor.

Examples:
  bash work/run_coremark_mytage_multi.sh
  bash work/run_coremark_mytage_multi.sh --iterations 1 --entry-per-set 8
  bash work/run_coremark_mytage_multi.sh --tage-param "ghistoryLength = 256"
  ENTRY_PER_SET=16 TABLE_HISTORY_WIDTH='[8, 32, 96, 192]' bash work/run_coremark_mytage_multi.sh

Common options:
  --iterations N                 CoreMark iteration argument. Default: 1
  --outdir DIR                   gem5 output directory
  --gem5-bin PATH                gem5 binary. Default: build/RISCV/gem5.opt
  --coremark-bin PATH            CoreMark binary
  --entry-per-set N              Tagged entries read from each TAGE row
  --fetch-width N                O3 fetch width and default bimodal counters/row
  --fetch-line-bytes N           Aligned fetch-line bytes used by MyTAGE
  --bimodal-ctrs-per-row N       Bimodal counters per aligned row
  --bimodal-offset-shift N       PC offset shift for bimodal bank select
  --history-hash-type NAME       FOLDED or CSR
  --replacement-mode NAME        USEFUL or PLRU
  --tage-param "field = value"   Extra system.cpu[0].branchPred.<field> param
  --param "expr"                 Raw gem5 --param expression
  --se-arg ARG                   Extra argument passed after se.py
  --gem5-arg ARG                 Extra argument passed before se.py
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

GEM5_BIN="${GEM5_BIN:-build/RISCV/gem5.opt}"
SE_CONFIG="${SE_CONFIG:-configs/deprecated/example/se.py}"
COREMARK_DIR="${COREMARK_DIR:-work/benchs/coremark/coremark}"
COREMARK_BIN="${COREMARK_BIN:-${COREMARK_DIR}/coremark.exe}"
ITERATIONS="${ITERATIONS:-1}"
COREMARK_ARGS="${COREMARK_ARGS:-}"
OUTDIR="${OUTDIR:-}"

CPU_TYPE="${CPU_TYPE:-RiscvO3CPU}"
BP_TYPE="${BP_TYPE:-MyTAGE}"
INDIRECT_BP_TYPE="${INDIRECT_BP_TYPE:-SimpleIndirectPredictor}"

FETCH_WIDTH="${FETCH_WIDTH:-4}"
DECODE_WIDTH="${DECODE_WIDTH:-4}"
ISSUE_WIDTH="${ISSUE_WIDTH:-4}"
COMMIT_WIDTH="${COMMIT_WIDTH:-4}"

BIMODAL_DEPTH="${BIMODAL_DEPTH:-1024}"
GHISTORY_LENGTH="${GHISTORY_LENGTH:-250}"
PCHISTORY_LENGTH="${PCHISTORY_LENGTH:-32}"
COMP_COUNT="${COMP_COUNT:-4}"
ENTRY_PER_SET="${ENTRY_PER_SET:-4}"
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

TABLE_DEPTH="${TABLE_DEPTH:-[128, 128, 128, 128]}"
TABLE_USEFUL_WIDTH="${TABLE_USEFUL_WIDTH:-[2, 2, 2, 2]}"
TABLE_CTR_WIDTH="${TABLE_CTR_WIDTH:-[3, 3, 3, 3]}"
TABLE_TAG_WIDTH="${TABLE_TAG_WIDTH:-[8, 8, 9, 9]}"
TABLE_HISTORY_WIDTH="${TABLE_HISTORY_WIDTH:-[5, 18, 68, 250]}"
TABLE_PC_HISTORY_START="${TABLE_PC_HISTORY_START:-[0, 0, 0, 0]}"
TABLE_PC_HISTORY_WIDTH="${TABLE_PC_HISTORY_WIDTH:-[3, 5, 16, 16]}"

MAXINSTS="${MAXINSTS:-}"
COREMARK_STDIN="${COREMARK_STDIN:-}"
COREMARK_STDOUT="${COREMARK_STDOUT:-coremark.stdout}"
COREMARK_STDERR="${COREMARK_STDERR:-coremark.stderr}"
GEM5_STDOUT="${GEM5_STDOUT:-gem5.stdout}"
GEM5_STDERR="${GEM5_STDERR:-gem5.stderr}"

EXTRA_GEM5_ARGS=()
EXTRA_SE_ARGS=()
EXTRA_PARAM_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --iterations)
            ITERATIONS="$2"
            shift 2
            ;;
        --coremark-args)
            COREMARK_ARGS="$2"
            shift 2
            ;;
        --outdir)
            OUTDIR="$2"
            shift 2
            ;;
        --gem5-bin)
            GEM5_BIN="$2"
            shift 2
            ;;
        --coremark-bin)
            COREMARK_BIN="$2"
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
            EXTRA_PARAM_ARGS+=(--param "system.cpu[0].branchPred.$2")
            shift 2
            ;;
        --param)
            EXTRA_PARAM_ARGS+=(--param "$2")
            shift 2
            ;;
        --se-arg)
            EXTRA_SE_ARGS+=("$2")
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

if [[ -z "${COREMARK_ARGS}" ]]; then
    COREMARK_ARGS="${ITERATIONS}"
fi
if [[ -z "${OUTDIR}" ]]; then
    OUTDIR="m5out/coremark-mytage-multi-$(date +%Y%m%d-%H%M%S)"
fi
if [[ -z "${BIMODAL_CTRS_PER_ROW}" ]]; then
    BIMODAL_CTRS_PER_ROW="${FETCH_WIDTH}"
fi

if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "error: gem5 binary not found or not executable: ${GEM5_BIN}" >&2
    echo "hint: build it with: scons -Q -j12 build/RISCV/gem5.opt" >&2
    exit 1
fi

if [[ ! -x "${COREMARK_BIN}" ]]; then
    echo "error: CoreMark binary not found or not executable: ${COREMARK_BIN}" >&2
    echo "hint: run: BUILD_COREMARK=1 bash work/se_mode_tests.sh" >&2
    exit 1
fi

mkdir -p "${OUTDIR}"

SE_IO_ARGS=(
    --output="${COREMARK_STDOUT}"
    --errout="${COREMARK_STDERR}"
)
if [[ -n "${COREMARK_STDIN}" ]]; then
    SE_IO_ARGS+=(--input="${COREMARK_STDIN}")
fi

SE_STOP_ARGS=()
if [[ -n "${MAXINSTS}" ]]; then
    SE_STOP_ARGS+=(-I "${MAXINSTS}")
fi

CPU_PARAM_ARGS=(
    --param "system.cpu[0].fetchWidth = ${FETCH_WIDTH}"
    --param "system.cpu[0].decodeWidth = ${DECODE_WIDTH}"
    --param "system.cpu[0].issueWidth = ${ISSUE_WIDTH}"
    --param "system.cpu[0].commitWidth = ${COMMIT_WIDTH}"
)

TAGE_PARAM_ARGS=(
    --param "system.cpu[0].branchPred.bimodalDepth = ${BIMODAL_DEPTH}"
    --param "system.cpu[0].branchPred.ghistoryLength = ${GHISTORY_LENGTH}"
    --param "system.cpu[0].branchPred.pchistoryLength = ${PCHISTORY_LENGTH}"
    --param "system.cpu[0].branchPred.compCount = ${COMP_COUNT}"
    --param "system.cpu[0].branchPred.entryPerSet = ${ENTRY_PER_SET}"
    --param "system.cpu[0].branchPred.fetchLineBytes = ${FETCH_LINE_BYTES}"
    --param "system.cpu[0].branchPred.bimodalCtrsPerRow = ${BIMODAL_CTRS_PER_ROW}"
    --param "system.cpu[0].branchPred.bimodalOffsetShift = ${BIMODAL_OFFSET_SHIFT}"
    --param "system.cpu[0].branchPred.bimodalUseAlignedAddr = $(py_bool "${BIMODAL_USE_ALIGNED_ADDR}")"
    --param "system.cpu[0].branchPred.tagUseAlignedAddr = $(py_bool "${TAG_USE_ALIGNED_ADDR}")"
    --param "system.cpu[0].branchPred.pcHashStartForIdx = ${PC_HASH_START_FOR_IDX}"
    --param "system.cpu[0].branchPred.pcHashWidthForIdx = ${PC_HASH_WIDTH_FOR_IDX}"
    --param "system.cpu[0].branchPred.pcHashStartForTag = ${PC_HASH_START_FOR_TAG}"
    --param "system.cpu[0].branchPred.pcHashWidthForTag = ${PC_HASH_WIDTH_FOR_TAG}"
    --param "system.cpu[0].branchPred.allocateRandomPlacement = $(py_bool "${ALLOCATE_RANDOM_PLACEMENT}")"
    --param "system.cpu[0].branchPred.allocateLongerThanProvider = $(py_bool "${ALLOCATE_LONGER_THAN_PROVIDER}")"
    --param "system.cpu[0].branchPred.periodicReset = $(py_bool "${PERIODIC_RESET}")"
    --param "system.cpu[0].branchPred.periodicResetBranchPeriod = ${PERIODIC_RESET_BRANCH_PERIOD}"
    --param "system.cpu[0].branchPred.useLfsr = $(py_bool "${USE_LFSR}")"
    --param "system.cpu[0].branchPred.lfsrWidth = ${LFSR_WIDTH}"
    --param "system.cpu[0].branchPred.lfsrSeed = ${LFSR_SEED}"
    --param "system.cpu[0].branchPred.lfsrMispredictionUpdate = $(py_bool "${LFSR_MISPREDICTION_UPDATE}")"
    --param "system.cpu[0].branchPred.historyHashType = '${HISTORY_HASH_TYPE}'"
    --param "system.cpu[0].branchPred.replacementMode = '${REPLACEMENT_MODE}'"
    --param "system.cpu[0].branchPred.useAltOnNA = $(py_bool "${USE_ALT_ON_NA}")"
    --param "system.cpu[0].branchPred.numUseAltOnNa = ${NUM_USE_ALT_ON_NA}"
    --param "system.cpu[0].branchPred.useAltOnNaBits = ${USE_ALT_ON_NA_BITS}"
    --param "system.cpu[0].branchPred.useAltOnNaHashStart = ${USE_ALT_ON_NA_HASH_START}"
    --param "system.cpu[0].branchPred.useAltOnNaHashWidth = ${USE_ALT_ON_NA_HASH_WIDTH}"
    --param "system.cpu[0].branchPred.tableDepth = ${TABLE_DEPTH}"
    --param "system.cpu[0].branchPred.tableUsefulWidth = ${TABLE_USEFUL_WIDTH}"
    --param "system.cpu[0].branchPred.tableCtrWidth = ${TABLE_CTR_WIDTH}"
    --param "system.cpu[0].branchPred.tableTagWidth = ${TABLE_TAG_WIDTH}"
    --param "system.cpu[0].branchPred.tableHistoryWidth = ${TABLE_HISTORY_WIDTH}"
    --param "system.cpu[0].branchPred.tablePcHistoryStart = ${TABLE_PC_HISTORY_START}"
    --param "system.cpu[0].branchPred.tablePcHistoryWidth = ${TABLE_PC_HISTORY_WIDTH}"
)

CMD=(
    "${GEM5_BIN}"
    --outdir="${OUTDIR}"
    "${EXTRA_GEM5_ARGS[@]}"
    "${SE_CONFIG}"
    --cpu-type="${CPU_TYPE}"
    --bp-type="${BP_TYPE}"
    --indirect-bp-type="${INDIRECT_BP_TYPE}"
    --num-cpus=1
    --caches
    --cmd="${COREMARK_BIN}"
    --options="${COREMARK_ARGS}"
    "${SE_IO_ARGS[@]}"
    "${SE_STOP_ARGS[@]}"
    "${CPU_PARAM_ARGS[@]}"
    "${TAGE_PARAM_ARGS[@]}"
    "${EXTRA_PARAM_ARGS[@]}"
    "${EXTRA_SE_ARGS[@]}"
)

printf '%q ' "${CMD[@]}" > "${OUTDIR}/command.txt"
printf '\n' >> "${OUTDIR}/command.txt"

echo "[gem5] outdir: ${OUTDIR}"
echo "[gem5] stats: ${OUTDIR}/stats.txt"
echo "[gem5] stdout: ${OUTDIR}/${GEM5_STDOUT}"
echo "[gem5] stderr: ${OUTDIR}/${GEM5_STDERR}"
echo "[coremark] stdout: ${OUTDIR}/${COREMARK_STDOUT}"
echo "[coremark] stderr: ${OUTDIR}/${COREMARK_STDERR}"
echo "[coremark] args: ${COREMARK_ARGS}"
echo "[mytage] entryPerSet=${ENTRY_PER_SET} bimodalCtrsPerRow=${BIMODAL_CTRS_PER_ROW} fetchWidth=${FETCH_WIDTH}"

set +e
"${CMD[@]}" >"${OUTDIR}/${GEM5_STDOUT}" 2>"${OUTDIR}/${GEM5_STDERR}"
status=$?
set -e

echo "[gem5] exit status: ${status}"
if [[ -s "${OUTDIR}/stats.txt" ]]; then
    echo "[gem5] stats written."
elif [[ -f "${OUTDIR}/stats.txt" ]]; then
    echo "[gem5] warning: stats.txt exists but is empty." >&2
else
    echo "[gem5] warning: stats.txt was not produced." >&2
fi

exit "${status}"
