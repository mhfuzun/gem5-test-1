#!/usr/bin/env bash
set -euo pipefail

# CoreMark has its own CORE_DEBUG macro.  This is separate from the gem5
# BPU_DEBUG switch in work/run_coremark_bpu_debug.sh:
#   COREMARK_DEBUG=1 => CoreMark forces iterations to 1 inside core_main.c.
#   BPU_DEBUG=1      => gem5 writes DecoupledBPU trace lines.
COREMARK_ITERATIONS="${COREMARK_ITERATIONS:-1}"
COREMARK_DEBUG="${COREMARK_DEBUG:-0}"
COREMARK_OPT="${COREMARK_OPT:--O2}"
MAKE_JOBS="${MAKE_JOBS:-12}"

echo "[coremark] ITERATIONS=${COREMARK_ITERATIONS}"
echo "[coremark] CORE_DEBUG=${COREMARK_DEBUG}"
echo "[coremark] OPT=${COREMARK_OPT}"

make -j"${MAKE_JOBS}" -C coremark \
  clean compile \
  PORT_DIR=posix \
  CC=riscv64-linux-gnu-gcc \
  ITERATIONS="${COREMARK_ITERATIONS}" \
  XCFLAGS="${COREMARK_OPT} -static -march=rv64gc -mabi=lp64d -DCORE_DEBUG=${COREMARK_DEBUG}"
