#!/usr/bin/env bash
set -euo pipefail

# Restore a RISC-V full-system checkpoint with AtomicSimpleCPU, then switch to
# RiscvO3CPU with the experimental decoupled BPU/FTQ frontend enabled.
#
# Usage:
#   CHECKPOINT=3 ./work/run_fs_checkpoint_bpu.sh
#
# Notes:
# - Because --restore-with-cpu=AtomicSimpleCPU differs from --cpu-type, gem5
#   creates the detailed O3 CPU as system.switch_cpus[0].  The BPU params must
#   therefore be applied to system.switch_cpus[0], not system.cpu[0].
# - Keep BPU_DEBUG=0 for full-system runs unless you intentionally want a large
#   DecoupledBPU trace file.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

GEM5_BIN="${GEM5_BIN:-build/RISCV/gem5.opt}"
OUTDIR="${OUTDIR:-m5out_t3}"
CONFIG="${CONFIG:-configs/example/riscv/fs_linux_detailed_o3.py}"
KERNEL="${KERNEL:-./boot-tests/bootloader-vmlinux-5.10}"
DISK_IMAGE="${DISK_IMAGE:-./boot-tests/ubuntu-riscv.raw.img}"
CHECKPOINT_DIR="${CHECKPOINT_DIR:?set CHECKPOINT_DIR to the CHECKPOINT_DIR, e.g. CHECKPOINT_DIR=ccc}"
CHECKPOINT="${CHECKPOINT:?set CHECKPOINT to the checkpoint number, e.g. CHECKPOINT=3}"

BPU_ENABLE="${BPU_ENABLE:-True}"
BPU_USE_TAGE="${BPU_USE_TAGE:-True}"
BPU_USE_RAS="${BPU_USE_RAS:-True}"
BPU_USE_ITTAGE="${BPU_USE_ITTAGE:-True}"
BPU_FTQ_DEPTH="${BPU_FTQ_DEPTH:-64}"
BPU_UBTB_ENTRIES="${BPU_UBTB_ENTRIES:-128}"
BPU_BTB_WAYS="${BPU_BTB_WAYS:-4}"
BPU_BTB_SETS="${BPU_BTB_SETS:-4096}"
BPU_BANKS="${BPU_BANKS:-4}"
BPU_TT_WAYS="${BPU_TT_WAYS:-2}"
BPU_TT_SETS="${BPU_TT_SETS:-1024}"

BPU_DEBUG="${BPU_DEBUG:-0}"
DEBUG_START_TICK="${DEBUG_START_TICK:-0}"
DEBUG_FILE="${DEBUG_FILE:-decoupled_bpu.trace}"

GEM5_DEBUG_ARGS=()
if [[ "${BPU_DEBUG,,}" != "0" &&
      "${BPU_DEBUG,,}" != "false" &&
      "${BPU_DEBUG,,}" != "off" &&
      "${BPU_DEBUG,,}" != "no" ]]; then
    GEM5_DEBUG_ARGS=(
        --debug-flags=DecoupledBPU
        --debug-start="${DEBUG_START_TICK}"
        --debug-file="${DEBUG_FILE}"
    )
fi

echo "[gem5] outdir: ${OUTDIR}"
echo "[gem5] CHECKPOINT_DIR: ${CHECKPOINT_DIR}"
echo "[gem5] checkpoint: ${CHECKPOINT}"
echo "[bpu] decoupled: ${BPU_ENABLE}"
echo "[bpu] modules: TAGE=${BPU_USE_TAGE} RAS=${BPU_USE_RAS} ITTAGE=${BPU_USE_ITTAGE}"

exec "${GEM5_BIN}" -d "${OUTDIR}" \
  "${GEM5_DEBUG_ARGS[@]}" \
  "${CONFIG}" \
  --kernel "${KERNEL}" \
  --disk-image "${DISK_IMAGE}" \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --cpu-type=RiscvO3CPU \
  --restore-with-cpu=AtomicSimpleCPU \
  --checkpoint-dir="${CHECKPOINT_DIR}" \
  -r "${CHECKPOINT}" \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-type DDR4_2400_8x8 \
  --mem-size 20GB \
  --mem-channels 2 \
  --l1i_size 64KiB \
  --l1d_size 64KiB \
  --l2_size 2MiB \
  --fetch-width 8 \
  --decode-width 8 \
  --rename-width 8 \
  --dispatch-width 8 \
  --issue-width 8 \
  --commit-width 8 \
  --num-rob-entries 256 \
  --num-iq-entries 128 \
  --lq-entries 64 \
  --sq-entries 64 \
  --bp-type MyTAGE \
  -P "system.switch_cpus[0].decoupledBPU = ${BPU_ENABLE}" \
  -P "system.switch_cpus[0].decoupledBPUUseTAGE = ${BPU_USE_TAGE}" \
  -P "system.switch_cpus[0].decoupledBPUUseRAS = ${BPU_USE_RAS}" \
  -P "system.switch_cpus[0].decoupledBPUUseITTAGE = ${BPU_USE_ITTAGE}" \
  -P "system.switch_cpus[0].decoupledBPUFTQDepth = ${BPU_FTQ_DEPTH}" \
  -P "system.switch_cpus[0].decoupledBPUUBTBEntries = ${BPU_UBTB_ENTRIES}" \
  -P "system.switch_cpus[0].decoupledBPUBTBWays = ${BPU_BTB_WAYS}" \
  -P "system.switch_cpus[0].decoupledBPUBTBSets = ${BPU_BTB_SETS}" \
  -P "system.switch_cpus[0].decoupledBPUBanks = ${BPU_BANKS}" \
  -P "system.switch_cpus[0].decoupledBPUTTWays = ${BPU_TT_WAYS}" \
  -P "system.switch_cpus[0].decoupledBPUTTSets = ${BPU_TT_SETS}"
