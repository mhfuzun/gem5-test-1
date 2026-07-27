#!/usr/bin/env bash
set -euo pipefail

# Run CoreMark on the RISC-V O3 SE system with the experimental decoupled
# BPU/FTQ enabled.  The common workflow is:
#
#   1. Run once with a large DEBUG_START_TICK so CoreMark warms the predictor.
#   2. Inspect m5out/stats.txt and the tick range where the steady loop lives.
#   3. Re-run with DEBUG_START_TICK set near that range and follow
#      m5out/decoupled_bpu.trace.
#
# CoreMark does not emit gem5 m5ops/workbegin markers in this SE setup, so the
# debug switch is tick-based.  If later you add m5_work_begin/end calls to
# CoreMark, this script can be split into a checkpoint/warmup phase and a
# precise debug phase.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

GEM5_BIN="${GEM5_BIN:-build/RISCV/gem5.opt}"
SE_CONFIG="${SE_CONFIG:-configs/deprecated/example/se.py}"
COREMARK_DIR="${COREMARK_DIR:-work/benchs/coremark/coremark}"
COREMARK_BIN="${COREMARK_BIN:-${COREMARK_DIR}/coremark.exe}"
COREMARK_ARGS="${COREMARK_ARGS:-}"
OUTDIR="${OUTDIR:-m5out/coremark-bpu-debug}"

# Set BPU_DEBUG=0 for fast runs.  When disabled, gem5 is launched without
# --debug-flags/--debug-file, which is much faster than merely moving
# DEBUG_START_TICK far into the future.
BPU_DEBUG="${BPU_DEBUG:-1}"

# Keep this high for the first debug run to avoid drowning in cold-start trace.
# Example:
#   DEBUG_START_TICK=500000000 ./work/run_coremark_bpu_debug.sh
DEBUG_START_TICK="${DEBUG_START_TICK:-0}"

# Stop condition.  Increase this for a full CoreMark run, keep it small while
# validating the BPU trace format, or set MAXINSTS="" to let CoreMark exit
# normally.  A forced instruction-limit exit can happen before target libc
# flushes stdout, so normal exit is the best way to collect the final score.
MAXINSTS="${MAXINSTS-5000000}"

# Target program stdio.  In gem5 SE mode there is no UART/m5term connection;
# the simulated process uses these host-backed file descriptors instead.
# Relative paths are created under OUTDIR by gem5.  Use stdout/stderr to stream
# the target output directly to the terminal.
COREMARK_STDIN="${COREMARK_STDIN:-}"
COREMARK_STDOUT="${COREMARK_STDOUT:-coremark.stdout}"
COREMARK_STDERR="${COREMARK_STDERR:-coremark.stderr}"

# gem5 predictor selection.  These still configure the real gem5 BPredUnit.
# The experimental BPU/FTQ layer is enabled below through -P decoupledBPU=True.
CPU_TYPE="${CPU_TYPE:-RiscvO3CPU}"
BP_TYPE="${BP_TYPE:-TAGE}"
INDIRECT_BP_TYPE="${INDIRECT_BP_TYPE:-SimpleIndirectPredictor}"

# Decoupled frontend table knobs.  They map to BaseO3CPU.py params.
BPU_ENABLE="${BPU_ENABLE:-True}"
BPU_FTQ_DEPTH="${BPU_FTQ_DEPTH:-64}"
BPU_UBTB_ENTRIES="${BPU_UBTB_ENTRIES:-128}"
BPU_BTB_WAYS="${BPU_BTB_WAYS:-4}"
BPU_BTB_SETS="${BPU_BTB_SETS:-4096}"
BPU_BANKS="${BPU_BANKS:-4}"
BPU_TT_WAYS="${BPU_TT_WAYS:-2}"
BPU_TT_SETS="${BPU_TT_SETS:-1024}"

# Module gates for the experimental BPU-side accounting/path.
BPU_USE_TAGE="${BPU_USE_TAGE:-True}"
BPU_USE_RAS="${BPU_USE_RAS:-True}"
BPU_USE_ITTAGE="${BPU_USE_ITTAGE:-True}"

# Optional CoreMark rebuild.  The repository already has a built binary, but
# this is useful after changing ITERATIONS or compiler flags in coremark_build.sh.
if [[ "${BUILD_COREMARK:-0}" == "1" ]]; then
    echo "[coremark] rebuilding ${COREMARK_BIN}"
    echo "[coremark] COREMARK_ITERATIONS=${COREMARK_ITERATIONS:-1}"
    echo "[coremark] COREMARK_DEBUG=${COREMARK_DEBUG:-0}"
    (cd work/benchs/coremark && ./coremark_build.sh)
fi

if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "error: gem5 binary not found or not executable: ${GEM5_BIN}" >&2
    echo "hint: build it with: scons -Q build/RISCV/gem5.opt" >&2
    exit 1
fi

if [[ ! -x "${COREMARK_BIN}" ]]; then
    echo "error: CoreMark binary not found or not executable: ${COREMARK_BIN}" >&2
    echo "hint: BUILD_COREMARK=1 ./work/run_coremark_bpu_debug.sh" >&2
    exit 1
fi

COREMARK_BIN_INFO="$(stat -c '%s bytes, mtime=%y' "${COREMARK_BIN}")"

mkdir -p "${OUTDIR}"

echo "[gem5] output dir: ${OUTDIR}"
if [[ "${BPU_DEBUG,,}" == "0" ||
      "${BPU_DEBUG,,}" == "false" ||
      "${BPU_DEBUG,,}" == "off" ||
      "${BPU_DEBUG,,}" == "no" ]]; then
    GEM5_DEBUG_ARGS=()
    echo "[gem5] debug: disabled"
else
    GEM5_DEBUG_ARGS=(
        --debug-flags=DecoupledBPU
        --debug-start="${DEBUG_START_TICK}"
        --debug-file=decoupled_bpu.trace
    )
    echo "[gem5] debug starts at tick: ${DEBUG_START_TICK}"
    echo "[gem5] debug trace: ${OUTDIR}/decoupled_bpu.trace"
fi
echo "[gem5] stats: ${OUTDIR}/stats.txt"
echo "[coremark] binary: ${COREMARK_BIN} (${COREMARK_BIN_INFO})"
echo "[coremark] args: ${COREMARK_ARGS:-<none>}"
echo "[bpu] decoupled: ${BPU_ENABLE}"
echo "[coremark] stdout: ${COREMARK_STDOUT}"
echo "[coremark] stderr: ${COREMARK_STDERR}"

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

SE_CMD_ARGS=(
    --cmd="${COREMARK_BIN}"
)
if [[ -n "${COREMARK_ARGS}" ]]; then
    SE_CMD_ARGS+=(--options="${COREMARK_ARGS}")
fi

exec "${GEM5_BIN}" \
    --outdir="${OUTDIR}" \
    "${GEM5_DEBUG_ARGS[@]}" \
    "${SE_CONFIG}" \
    --cpu-type="${CPU_TYPE}" \
    --bp-type="${BP_TYPE}" \
    --indirect-bp-type="${INDIRECT_BP_TYPE}" \
    --caches \
    "${SE_CMD_ARGS[@]}" \
    "${SE_IO_ARGS[@]}" \
    "${SE_STOP_ARGS[@]}"
