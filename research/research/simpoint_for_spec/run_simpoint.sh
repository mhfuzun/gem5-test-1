#!/usr/bin/env bash
# Host-side automation: boot checkpoint -> benchmark-start checkpoint ->
# BBV profiling -> SimPoint clustering -> windows summary.
set -euo pipefail

# Required env variables:
: "${DISK_IMAGE:?Set DISK_IMAGE to the absolute path of the RISC-V disk image}"
: "${BOOT_CPT:?Set BOOT_CPT to the booted checkpoint folder (or its parent)}"

GEM5_MODE=${GEM5_MODE:-fast}
GEM5_BIN=${GEM5_BIN:-./build/RISCV/gem5.$GEM5_MODE}
CONFIG=${CONFIG:-./fs_simpoint.py}
SIMPOINT_BIN=${SIMPOINT_BIN:-simpoint}
OUT=${OUT:-./simpoint_results/spec_run_01}
INTERVAL=${INTERVAL:-10000000}
MAX_K=${MAX_K:-10}
WARMUP=${WARMUP:-10000000}
KERNEL=${KERNEL:-riscv-linux-5.15.180}
BOOTLOADER=${BOOTLOADER:-riscv-bootloader-opensbi-1.3.1}
CPU_CLOCK=${CPU_CLOCK:-2GHz}
SYS_CLOCK=${SYS_CLOCK:-1GHz}
MEM_SIZE=${MEM_SIZE:-16GiB}
L1I_SIZE=${L1I_SIZE:-64KiB}
L1D_SIZE=${L1D_SIZE:-64KiB}
L2_SIZE=${L2_SIZE:-2MiB}
KERNEL_CMDLINE=${KERNEL_CMDLINE:-console=ttyS0 root=/dev/vda1 ro init=/sbin/init}

for number in INTERVAL MAX_K WARMUP; do
  value=${!number}
  if [[ ! "$value" =~ ^[0-9]+$ ]]; then
    echo "ERROR: $number must be a nonnegative integer" >&2
    exit 2
  fi
done
if (( INTERVAL <= 0 || MAX_K <= 0 )); then
  echo "ERROR: INTERVAL and MAX_K must be positive" >&2
  exit 2
fi

GEM5_BIN=$(realpath -e "$GEM5_BIN")
CONFIG=$(realpath -e "$CONFIG")
DISK_IMAGE=$(realpath -e "$DISK_IMAGE")
BOOT_CPT=$(realpath -e "$BOOT_CPT")

[[ -x "$GEM5_BIN" ]] || {
    echo "ERROR: gem5 binary not found: $GEM5_BIN"
    exit 1
}

echo "Using gem5: $GEM5_BIN"

command -v "$SIMPOINT_BIN" >/dev/null || {
  echo "ERROR: SimPoint executable not found: $SIMPOINT_BIN" >&2
  exit 2
}

mkdir -p "$OUT"
OUT=$(realpath -e "$OUT")
BENCH_CPT="$OUT/benchmark_start"
BBV_OUT="$OUT/profile_gem5"
ANALYSIS="$OUT/analysis"
if [[ -e "$BENCH_CPT" || -e "$BBV_OUT" || -e "$ANALYSIS" ]]; then
  echo "ERROR: output already contains a previous run. Choose a fresh OUT: $OUT" >&2
  exit 2
fi
mkdir -p "$OUT/prepare_gem5" "$BBV_OUT" "$ANALYSIS"

shared_args=(
  --disk-image "$DISK_IMAGE"
  --kernel "$KERNEL"
  --bootloader "$BOOTLOADER"
  --command-line "$KERNEL_CMDLINE"
  --cpu-type atomic
  --cpu-clock "$CPU_CLOCK"
  --sys-clock "$SYS_CLOCK"
  --mem-size "$MEM_SIZE"
  --l1i_size "$L1I_SIZE"
  --l1d_size "$L1D_SIZE"
  --l2_size "$L2_SIZE"
)

echo "===================================================="
echo "STAGE A: Boot checkpoint -> benchmark-start checkpoint"
echo "When the guest terminal is ready, run e.g.:"
echo "  bash /data/guest_launch_spec.sh /data/spec-runs/gcc bash ./run_gcc.sh"
echo "The guest launcher calls 'm5 exit' BEFORE the actual SPEC command."
echo "gem5 will resume it automatically from that point in Stage B."
echo "===================================================="
preparation_start=$(date +%s%N)
"$GEM5_BIN" -d "$OUT/prepare_gem5" "$CONFIG" \
  "${shared_args[@]}" \
  --restore-checkpoint "$BOOT_CPT" \
  --exit-action checkpoint \
  --checkpoint-dir "$BENCH_CPT" 2>&1 | tee "$OUT/prepare.log"
preparation_secs=$(python3 -c 'import sys; print(f"{int(sys.argv[1]) / 1e9:.3f}")' "$(( $(date +%s%N) - preparation_start ))")
if [[ ! -f "$BENCH_CPT/m5.cpt" ]]; then
  echo "ERROR: no checkpoint saved by Stage A" >&2
  exit 1
fi

echo "===================================================="
echo "STAGE B: Benchmark-start checkpoint -> BBV profile"
echo "===================================================="
profile_start=$(date +%s%N)
"$GEM5_BIN" -d "$BBV_OUT" "$CONFIG" \
  "${shared_args[@]}" \
  --restore-checkpoint "$BENCH_CPT" \
  --simpoint-profile --simpoint-interval "$INTERVAL" \
  --exit-action stop 2>&1 | tee "$OUT/profile.log"
profile_secs=$(python3 -c 'import sys; print(f"{int(sys.argv[1]) / 1e9:.3f}")' "$(( $(date +%s%N) - profile_start ))")

BBV="$BBV_OUT/simpoint.bb.gz"
if [[ ! -s "$BBV" ]]; then
  echo "ERROR: BBV output is missing or empty: $BBV" >&2
  exit 1
fi
gzip -t "$BBV"
# Count complete intervals, excluding the incomplete tail.
interval_count=$(gzip -cd "$BBV" | awk '/^T/{n++} END{print n+0}')
if (( interval_count < 2 )); then
  echo "ERROR: only $interval_count complete BBV intervals; run a longer benchmark or lower INTERVAL" >&2
  exit 1
fi
if (( MAX_K < interval_count )); then
  actual_k=$MAX_K
else
  actual_k=$interval_count
fi

echo "===================================================="
echo "STAGE C: SimPoint clustering ($interval_count intervals, maxK=$actual_k)"
echo "===================================================="
analysis_start=$(date +%s%N)
(
  cd "$ANALYSIS"
  "$SIMPOINT_BIN" \
    -loadFVFile "$BBV" \
    -inputVectorsGzipped \
    -maxK "$actual_k" \
    -saveSimpoints simpoints.txt \
    -saveSimpointWeights weights.txt
) 2>&1 | tee "$OUT/simpoint.log"
analysis_secs=$(python3 -c 'import sys; print(f"{int(sys.argv[1]) / 1e9:.3f}")' "$(( $(date +%s%N) - analysis_start ))")

test -s "$ANALYSIS/simpoints.txt"
test -s "$ANALYSIS/weights.txt"
python3 "$(dirname "$(realpath -e "${BASH_SOURCE[0]}")")/summarize_simpoints.py" \
  --analysis "$ANALYSIS" \
  --stats "$BBV_OUT/stats.txt" \
  --interval "$INTERVAL" \
  --warmup "$WARMUP" \
  --bbv-intervals "$interval_count" \
  --preparation-seconds "$preparation_secs" \
  --profile-seconds "$profile_secs" \
  --cluster-seconds "$analysis_secs"

echo "==========================================="
echo "DONE"
echo "BBV: $BBV"
echo "Windows: $ANALYSIS/windows.tsv"
echo "Metadata: $ANALYSIS/metadata.json"
echo "Output: $OUT"
