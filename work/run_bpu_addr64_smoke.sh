#!/usr/bin/env bash
set -euo pipefail

# Small regression wrapper for the experimental decoupled BPU.
# It is intentionally short: the goal is to catch address-width regressions,
# hangs in the demand-pumped BPU path, and missing BPU stats before a long run.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RUN_SE="${RUN_SE:-1}"
RUN_FS="${RUN_FS:-1}"

SE_OUTDIR="${SE_OUTDIR:-m5out/bpu-addr64-se-smoke}"
FS_OUTDIR="${FS_OUTDIR:-m5out/bpu-addr64-fs-smoke}"

MAXINSTS="${MAXINSTS:-10000}"
REL_MAX_TICK="${REL_MAX_TICK:-100000}"
BPU_BURST_TICKS="${BPU_BURST_TICKS:-24}"

CHECKPOINT_DIR="${CHECKPOINT_DIR:-m5out_rasCheckpoint}"
CHECKPOINT="${CHECKPOINT:-1}"

stats_regex="hostSeconds|simInsts|decoupledBpuTicks|decoupledBpuBursts|decoupledBpuSpeculativeNodesMax|decoupledBpuRASDepthMax|decoupledBpuTageCheckpointsMax|decoupledBpuITTAGECheckpointsMax|decoupledBpuTageHits|decoupledBpuTageMisses|decoupledBpuITTAGEHits|decoupledBpuITTAGEMisses|decoupledBpuRASHits|decoupledBpuRASMisses|decoupledBpuBackendRedirects|decoupledBpuFetchPredecodeRedirects"

show_stats() {
    local stats_file="$1"

    if [[ -f "$stats_file" ]]; then
        rg -n "$stats_regex" "$stats_file" || true
    else
        echo "[bpu-smoke] missing stats file: $stats_file"
        return 1
    fi
}

if [[ "$RUN_SE" == "1" ]]; then
    echo "[bpu-smoke] running SE CoreMark smoke: outdir=$SE_OUTDIR"
    timeout -k 5s 60s env \
        OUTDIR="$SE_OUTDIR" \
        BPU_ENABLE=True \
        BPU_DEBUG=0 \
        BUILD_COREMARK=0 \
        MAXINSTS="$MAXINSTS" \
        COREMARK_ARGS="${COREMARK_ARGS:-1}" \
        BPU_BURST_TICKS="$BPU_BURST_TICKS" \
        BPU_USE_TAGE="${BPU_USE_TAGE:-True}" \
        BPU_USE_RAS="${BPU_USE_RAS:-True}" \
        BPU_USE_ITTAGE="${BPU_USE_ITTAGE:-True}" \
        ./work/run_coremark_bpu_debug.sh
    show_stats "$SE_OUTDIR/stats.txt"
fi

if [[ "$RUN_FS" == "1" ]]; then
    if [[ ! -d "$CHECKPOINT_DIR" ]]; then
        echo "[bpu-smoke] skipping FS smoke; checkpoint dir not found: $CHECKPOINT_DIR"
        exit 0
    fi

    echo "[bpu-smoke] running FS checkpoint smoke: outdir=$FS_OUTDIR"
    timeout -k 5s 90s env \
        CHECKPOINT_DIR="$CHECKPOINT_DIR" \
        CHECKPOINT="$CHECKPOINT" \
        OUTDIR="$FS_OUTDIR" \
        BPU_ENABLE=True \
        BPU_DEBUG=0 \
        LISTENER_MODE=off \
        REL_MAX_TICK="$REL_MAX_TICK" \
        BPU_BURST_TICKS="$BPU_BURST_TICKS" \
        BPU_USE_TAGE="${FS_BPU_USE_TAGE:-False}" \
        BPU_USE_RAS="${FS_BPU_USE_RAS:-False}" \
        BPU_USE_ITTAGE="${FS_BPU_USE_ITTAGE:-False}" \
        ./work/run_fs_checkpoint_bpu.sh
    show_stats "$FS_OUTDIR/stats.txt"
fi
