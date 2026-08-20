#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}/gem5"

export GEM5_BIN="${GEM5_BIN:-${ROOT_DIR}/run.sh}"
export RUN_SCRIPT="${RUN_SCRIPT:-./work/run_coremark_bpu_debug.sh}"

exec ./work/run_fetch_bank_align_sweep.sh "$@"
