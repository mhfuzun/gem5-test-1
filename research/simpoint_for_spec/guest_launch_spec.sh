#!/usr/bin/env bash
# INSIDE THE RISC-V GUEST (requires `m5` in PATH).
# Usage: bash guest_launch_spec.sh /absolute/spec/run/dir bash ./run_benchmark.sh
set -u
if (( $# < 2 )); then
  echo "Usage: $0 RUN_DIRECTORY BENCHMARK_COMMAND [args...]" >&2
  exit 2
fi
run_dir=$1
shift
cd "$run_dir" || exit 3
command -v m5 >/dev/null || { echo "m5 command is required" >&2; exit 4; }
echo "[guest] Ready to start SPEC in $(pwd), checkpointing just before execution"
m5 exit || exit 5  # Stage A host saves checkpoint and stops here.
# This command resumes in Stage B, after the checkpoint is restored.
echo "[guest] Starting benchmark: $*"
"$@"
status=$?
echo "[guest] SPEC command exit status: $status"
sync
m5 exit            # Stage B host stops profiling and clusters the BBV.
exit "$status"
