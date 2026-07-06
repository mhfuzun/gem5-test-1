#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
isa="${GEM5_ISA:-RISCV}"
binary="${root_dir}/bin/gem5.opt"
loader="${root_dir}/lib/ld-linux-x86-64.so.2"

if [[ ! -x "${binary}" ]]; then
    echo "Missing gem5 binary: ${binary}" >&2
    exit 1
fi

export LD_LIBRARY_PATH="${root_dir}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PYTHONHOME="${root_dir}/python"
export PYTHONPATH="${root_dir}/gem5/build/${isa}/python:${root_dir}/gem5/src/python:${root_dir}/gem5/ext/ply:${root_dir}/gem5/ext/Kconfiglib${PYTHONPATH:+:${PYTHONPATH}}"

cd "${root_dir}/gem5"

if [[ -x "${loader}" ]]; then
    exec "${loader}" --library-path "${root_dir}/lib" "${binary}" "$@"
fi

exec "${binary}" "$@"
