#!/bin/sh

GEM5_ROOT=$(cd "$(dirname "$0")/.." && pwd)

$GEM5_ROOT/build/RISCV/gem5.opt \
  $GEM5_ROOT/configs/example/gem5_library/riscv-fs.py \
  --script coremark.rcS \
  --cpu O3CPU \
  --caches \
  --l2cache \
  --mem-size 2GB
