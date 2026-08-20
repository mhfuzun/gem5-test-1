#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

ALIGN_VALUES="${ALIGN_VALUES:-2 4 8 16 32 64}"
BASE_OUTDIR="${BASE_OUTDIR:-./m5out/fetchBankAlign}"
RUN_SCRIPT="${RUN_SCRIPT:-./work/run_coremark_bpu_debug.sh}"
SWEEP_JOBS="${SWEEP_JOBS:-3}"

if ! [[ "${SWEEP_JOBS}" =~ ^[0-9]+$ ]] || (( SWEEP_JOBS < 1 )); then
    echo "error: SWEEP_JOBS must be a positive integer" >&2
    exit 1
fi

batch_pids=()
batch_aligns=()

run_one() {
    local align="$1"
    local outdir="${BASE_OUTDIR}-align${align}"
    local log="${outdir}/run.log"

    mkdir -p "${outdir}"
    echo "[sweep] start align=${align} outdir=${outdir} log=${log}" >&2
    OUTDIR="${outdir}" \
    FETCH_BANK_ALIGN_BYTES="${align}" \
    "${RUN_SCRIPT}" > "${log}" 2>&1
    echo "[sweep] done align=${align}" >&2
}

wait_batch() {
    local failed=0

    for i in "${!batch_pids[@]}"; do
        if ! wait "${batch_pids[$i]}"; then
            echo "[sweep] failed align=${batch_aligns[$i]} " \
                 "log=${BASE_OUTDIR}-align${batch_aligns[$i]}/run.log" >&2
            failed=1
        fi
    done

    batch_pids=()
    batch_aligns=()
    return "${failed}"
}

failed=0
for align in ${ALIGN_VALUES}; do
    run_one "${align}" &
    batch_pids+=("$!")
    batch_aligns+=("${align}")

    if (( ${#batch_pids[@]} >= SWEEP_JOBS )); then
        wait_batch || failed=1
    fi
done

wait_batch || failed=1

if (( failed )); then
    echo "error: one or more sweep runs failed" >&2
    exit 1
fi

echo "align_bytes,outdir,sim_insts,host_inst_rate,ipc,cache_lines,total_time_secs,iterations_per_sec"
for align in ${ALIGN_VALUES}; do
    outdir="${BASE_OUTDIR}-align${align}"
    stats="${outdir}/stats.txt"
    stdout="${outdir}/coremark.stdout"
    sim_insts="$(awk '$1 == "simInsts" {print $2; exit}' "${stats}")"
    host_inst_rate="$(awk '$1 == "hostInstRate" {print $2; exit}' "${stats}")"
    ipc="$(awk '$1 == "system.cpu.ipc" {print $2; exit}' "${stats}")"
    cache_lines="$(awk '$1 == "system.cpu.fetch.cacheLines" {print $2; exit}' "${stats}")"
    total_time="$(awk '$1 == "Total" && $2 == "time" {print $4; exit}' "${stdout}")"
    iterations_per_sec="$(awk '$1 == "Iterations/Sec" {print $3; exit}' "${stdout}")"
    echo "${align},${outdir},${sim_insts:-NA},${host_inst_rate:-NA},${ipc:-NA},${cache_lines:-NA},${total_time:-NA},${iterations_per_sec:-NA}"
done
