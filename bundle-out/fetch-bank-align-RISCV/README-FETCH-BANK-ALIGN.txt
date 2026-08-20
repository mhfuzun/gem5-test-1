Fetch-bank align sweep bundle
=============================

This directory is self-contained for the CoreMark fetch-bank alignment sweep.
The target machine does not need scons, a RISC-V compiler, or gem5 build
dependencies.

Run one foreground sweep:

  ./run_fetch_bank_align_sweep.sh > sweep.csv 2> sweep.log

Leave it running after logout:

  nohup env SWEEP_JOBS=3 ALIGN_VALUES="2 4 8 16 32 64" \
    ./run_fetch_bank_align_sweep.sh > sweep.csv 2> sweep.log &
  echo $! > sweep.pid

Useful overrides:

  SWEEP_JOBS=1                         number of parallel gem5 runs
  ALIGN_VALUES="2 4 8 16 32 64"        alignment values to test
  MAXINSTS=5000000                     gem5 instruction limit
  BASE_OUTDIR=./m5out/fetchBankAlign   output prefix under gem5/
  BPU_DEBUG=1                          enable DecoupledBPU debug trace
  FETCH_BANK_PLOT=0                    skip SVG/CSV distribution output

Monitor:

  tail -f sweep.log
  tail -f gem5/m5out/fetchBankAlign-align2/run.log

Final per-run outputs are under:

  gem5/m5out/fetchBankAlign-align*/stats.txt
  gem5/m5out/fetchBankAlign-align*/coremark.stdout
  gem5/m5out/fetchBankAlign-align*/fetch_bank_dist.csv
  gem5/m5out/fetchBankAlign-align*/fetch_bank_dist.svg
