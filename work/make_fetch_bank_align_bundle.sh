#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

ISA="${ISA:-RISCV}"
JOBS="${JOBS:-12}"
BUILD_GEM5="${BUILD_GEM5:-auto}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/bundle-out/fetch-bank-align-${ISA}}"
ARCHIVE="${ARCHIVE:-${OUT_DIR}.tar.gz}"
GEM5_BIN="${ROOT_DIR}/build/${ISA}/gem5.opt"
COREMARK_BIN="${ROOT_DIR}/work/benchs/coremark/coremark/coremark.exe"
COLLECT_RUNTIME="${ROOT_DIR}/util/dockerfiles/rhel7-compat-bundle/collect-runtime.sh"

if [[ "${BUILD_GEM5}" == "1" ||
      "${BUILD_GEM5,,}" == "true" ||
      ( "${BUILD_GEM5}" == "auto" && ! -x "${GEM5_BIN}" ) ]]; then
    echo "[bundle] building ${GEM5_BIN} with ${JOBS} jobs"
    scons -Q "build/${ISA}/gem5.opt" "-j${JOBS}"
fi

if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "error: gem5 binary not found or not executable: ${GEM5_BIN}" >&2
    echo "hint: BUILD_GEM5=1 ${BASH_SOURCE[0]}" >&2
    exit 1
fi

if [[ ! -x "${COREMARK_BIN}" ]]; then
    echo "error: CoreMark binary not found or not executable: ${COREMARK_BIN}" >&2
    echo "hint: build it first under work/benchs/coremark" >&2
    exit 1
fi

if [[ ! -f "${COLLECT_RUNTIME}" ]]; then
    echo "error: runtime collector not found: ${COLLECT_RUNTIME}" >&2
    exit 1
fi

echo "[bundle] collecting gem5 runtime into ${OUT_DIR}"
bash "${COLLECT_RUNTIME}" "${ROOT_DIR}" "${ISA}" "${OUT_DIR}"

bundle_gem5="${OUT_DIR}/gem5"
bundle_work="${bundle_gem5}/work"

mkdir -p \
    "${bundle_work}/benchs/coremark/coremark"

cp -a \
    "${ROOT_DIR}/work/run_fetch_bank_align_sweep.sh" \
    "${ROOT_DIR}/work/run_coremark_bpu_debug.sh" \
    "${ROOT_DIR}/work/plot_fetch_bank_dist.py" \
    "${bundle_work}/"

cp -a "${COREMARK_BIN}" "${bundle_work}/benchs/coremark/coremark/coremark.exe"

cat > "${OUT_DIR}/run_fetch_bank_align_sweep.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}/gem5"

export GEM5_BIN="${GEM5_BIN:-${ROOT_DIR}/run.sh}"
export RUN_SCRIPT="${RUN_SCRIPT:-./work/run_coremark_bpu_debug.sh}"

exec ./work/run_fetch_bank_align_sweep.sh "$@"
EOF
chmod +x "${OUT_DIR}/run_fetch_bank_align_sweep.sh"

cat > "${OUT_DIR}/README-FETCH-BANK-ALIGN.txt" <<'EOF'
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
EOF

mkdir -p "$(dirname "${ARCHIVE}")"
tar -C "$(dirname "${OUT_DIR}")" -czf "${ARCHIVE}" "$(basename "${OUT_DIR}")"

echo "[bundle] ready: ${OUT_DIR}"
echo "[bundle] archive: ${ARCHIVE}"
