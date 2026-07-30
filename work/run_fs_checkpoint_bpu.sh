#!/usr/bin/env bash
set -euo pipefail

# Restore a RISC-V full-system checkpoint with AtomicSimpleCPU, then switch to
# RiscvO3CPU with the experimental decoupled BPU/FTQ frontend enabled.
#
# Usage:
#   CHECKPOINT=3 ./work/run_fs_checkpoint_bpu.sh
#   CHECKPOINT_DIR=m5out_rasCheckpoint CHECKPOINT=1 ./work/run_fs_checkpoint_bpu.sh
#
# Notes:
# - Because --restore-with-cpu=AtomicSimpleCPU differs from --cpu-type, gem5
#   creates the detailed O3 CPU as system.switch_cpus[0].  The BPU params must
#   therefore be applied to system.switch_cpus[0], not system.cpu[0].
# - Keep BPU_DEBUG=0 for full-system runs unless you intentionally want a large
#   DecoupledBPU trace file.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

GEM5_BOOT_DIR=../gem5-boot-tests-image-files

GEM5_BIN="${GEM5_BIN:-build/RISCV/gem5.opt}"
OUTDIR="${OUTDIR:-m5out_t3}"
CONFIG="${CONFIG:-configs/example/riscv/fs_linux_detailed_o3.py}"
KERNEL="${KERNEL:-${GEM5_BOOT_DIR}/bootloader-vmlinux-5.10}"
DISK_IMAGE="${DISK_IMAGE:-${GEM5_BOOT_DIR}/ubuntu-riscv.raw.img}"
CHECKPOINT_DIR="${CHECKPOINT_DIR:?set CHECKPOINT_DIR to the CHECKPOINT_DIR, e.g. CHECKPOINT_DIR=ccc}"
CHECKPOINT="${CHECKPOINT:?set CHECKPOINT to the checkpoint number, e.g. CHECKPOINT=3}"
MEM_SIZE="${MEM_SIZE:-20GB}"

BPU_ENABLE="${BPU_ENABLE:-True}"
BPU_USE_TAGE="${BPU_USE_TAGE:-True}"
BPU_USE_RAS="${BPU_USE_RAS:-True}"
BPU_USE_ITTAGE="${BPU_USE_ITTAGE:-True}"
BPU_BURST_TICKS="${BPU_BURST_TICKS:-24}"
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

# Full-system serial console uses gem5's listener sockets.  gem5's default
# listener-mode=auto disables all listeners when stdin is not a real terminal
# (for example when launched from Codex, a batch shell, or a wrapper script).
# Force it on here so m5term can always attach to the restored Linux console.
LISTENER_MODE="${LISTENER_MODE:-on}"
TERMINAL_PORT="${TERMINAL_PORT:-3456}"
REMOTE_GDB_PORT="${REMOTE_GDB_PORT:-0}"
ALLOW_REMOTE_CONNECTIONS="${ALLOW_REMOTE_CONNECTIONS:-0}"

# Optional stop conditions for bring-up/debug runs.  Leave them empty for an
# open-ended interactive FS session.
MAXINSTS="${MAXINSTS:-}"
REL_MAX_TICK="${REL_MAX_TICK:-}"
ABS_MAX_TICK="${ABS_MAX_TICK:-}"

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

GEM5_LISTENER_ARGS=(
  --listener-mode="${LISTENER_MODE}"
  --remote-gdb-port="${REMOTE_GDB_PORT}"
)
if [[ "${ALLOW_REMOTE_CONNECTIONS,,}" == "1" ||
      "${ALLOW_REMOTE_CONNECTIONS,,}" == "true" ||
      "${ALLOW_REMOTE_CONNECTIONS,,}" == "yes" ||
      "${ALLOW_REMOTE_CONNECTIONS,,}" == "on" ]]; then
  GEM5_LISTENER_ARGS+=(--allow-remote-connections)
fi

SIM_STOP_ARGS=()
if [[ -n "${MAXINSTS}" ]]; then
  SIM_STOP_ARGS+=(--maxinsts "${MAXINSTS}")
fi
if [[ -n "${REL_MAX_TICK}" ]]; then
  SIM_STOP_ARGS+=(--rel-max-tick "${REL_MAX_TICK}")
fi
if [[ -n "${ABS_MAX_TICK}" ]]; then
  SIM_STOP_ARGS+=(--abs-max-tick "${ABS_MAX_TICK}")
fi

echo "[gem5] outdir: ${OUTDIR}"
echo "[gem5] CHECKPOINT_DIR: ${CHECKPOINT_DIR}"
echo "[gem5] checkpoint: ${CHECKPOINT}"
echo "[gem5] listener mode: ${LISTENER_MODE}"
echo "[gem5] terminal: ./util/term/m5term localhost ${TERMINAL_PORT}"
echo "[gem5] remote gdb port: ${REMOTE_GDB_PORT}"
echo "[gem5] mem size: ${MEM_SIZE}"
echo "[gem5] stop: MAXINSTS=${MAXINSTS:-<none>} REL_MAX_TICK=${REL_MAX_TICK:-<none>} ABS_MAX_TICK=${ABS_MAX_TICK:-<none>}"
echo "[bpu] decoupled: ${BPU_ENABLE}"
echo "[bpu] modules: TAGE=${BPU_USE_TAGE} RAS=${BPU_USE_RAS} ITTAGE=${BPU_USE_ITTAGE}"

exec "${GEM5_BIN}" -d "${OUTDIR}" \
  "${GEM5_LISTENER_ARGS[@]}" \
  "${GEM5_DEBUG_ARGS[@]}" \
  "${CONFIG}" \
  --kernel "${KERNEL}" \
  --disk-image "${DISK_IMAGE}" \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --terminal-port "${TERMINAL_PORT}" \
  --cpu-type=RiscvO3CPU \
  --restore-with-cpu=AtomicSimpleCPU \
  --checkpoint-dir="${CHECKPOINT_DIR}" \
  -r "${CHECKPOINT}" \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-type DDR4_2400_8x8 \
  --mem-size "${MEM_SIZE}" \
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
  --bp-type TAGE \
  "${SIM_STOP_ARGS[@]}" \
  -P "system.switch_cpus[0].decoupledBPU = ${BPU_ENABLE}" \
  -P "system.switch_cpus[0].decoupledBPUUseTAGE = ${BPU_USE_TAGE}" \
  -P "system.switch_cpus[0].decoupledBPUUseRAS = ${BPU_USE_RAS}" \
  -P "system.switch_cpus[0].decoupledBPUUseITTAGE = ${BPU_USE_ITTAGE}" \
  -P "system.switch_cpus[0].decoupledBPUBurstTicks = ${BPU_BURST_TICKS}" \
  -P "system.switch_cpus[0].decoupledBPUFTQDepth = ${BPU_FTQ_DEPTH}" \
  -P "system.switch_cpus[0].decoupledBPUUBTBEntries = ${BPU_UBTB_ENTRIES}" \
  -P "system.switch_cpus[0].decoupledBPUBTBWays = ${BPU_BTB_WAYS}" \
  -P "system.switch_cpus[0].decoupledBPUBTBSets = ${BPU_BTB_SETS}" \
  -P "system.switch_cpus[0].decoupledBPUBanks = ${BPU_BANKS}" \
  -P "system.switch_cpus[0].decoupledBPUTTWays = ${BPU_TT_WAYS}" \
  -P "system.switch_cpus[0].decoupledBPUTTSets = ${BPU_TT_SETS}"
