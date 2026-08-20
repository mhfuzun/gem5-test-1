#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <gem5-src-dir> <isa> <output-dir>" >&2
    exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_root="$(realpath "$1")"
isa="$2"
mkdir -p "$3"
out_root="$(realpath "$3")"
binary="${src_root}/build/${isa}/gem5.opt"

if [[ ! -x "${binary}" ]]; then
    echo "gem5 binary not found: ${binary}" >&2
    exit 1
fi

bundle_root="${out_root}"

mkdir -p "${bundle_root}"
find "${bundle_root}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p \
    "${bundle_root}/bin" \
    "${bundle_root}/lib" \
    "${bundle_root}/python/lib" \
    "${bundle_root}/python/lib64" \
    "${bundle_root}/gem5/build/${isa}/python"

cp -Lv "${binary}" "${bundle_root}/bin/gem5.opt"

copy_runtime_file() {
    local path="$1"
    if [[ -z "${path}" || ! -e "${path}" ]]; then
        return 0
    fi
    cp -Lv "${path}" "${bundle_root}/lib/"
}

copy_needed_libs() {
    local elf="$1"
    while IFS= read -r lib; do
        copy_runtime_file "${lib}"
    done < <(
        ldd "${elf}" 2>/dev/null | awk '
            $2 == "=>" && $3 ~ /^\// { print $3 }
            $1 ~ /^\// { print $1 }
        ' | sort -u
    )
}

interpreter="$(readelf -l "${binary}" | awk '/Requesting program interpreter/ {gsub(/[\[\]]/, "", $NF); print $NF}')"
copy_runtime_file "${interpreter}"
copy_needed_libs "${binary}"

copy_python_tree() {
    local src_dir="$1"
    local dst_dir="$2"
    if [[ -d "${src_dir}" ]]; then
        mkdir -p "$(dirname "${dst_dir}")"
        cp -a "${src_dir}" "${dst_dir}"
    fi
}

py_stdlib="$(python3 -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')"
py_platstdlib="$(python3 -c 'import sysconfig; print(sysconfig.get_path("platstdlib"))')"
py_version="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"

copy_python_tree "${py_stdlib}" "${bundle_root}/python/lib/python${py_version}"
if [[ "${py_platstdlib}" != "${py_stdlib}" ]]; then
    copy_python_tree "${py_platstdlib}" "${bundle_root}/python/lib64/python${py_version}"
fi

python_shared="$(ldd "${binary}" | awk '/libpython3/ && $3 ~ /^\// { print $3; exit }')"
copy_runtime_file "${python_shared}"
if [[ -n "${python_shared}" ]]; then
    copy_needed_libs "${python_shared}"
fi

copy_repo_tree() {
    local rel="$1"
    if [[ -e "${src_root}/${rel}" ]]; then
        mkdir -p "$(dirname "${bundle_root}/gem5/${rel}")"
        cp -a "${src_root}/${rel}" "${bundle_root}/gem5/${rel}"
    fi
}

copy_repo_tree configs
copy_repo_tree src/python
copy_repo_tree ext/ply
copy_repo_tree ext/Kconfiglib

rsync -a --prune-empty-dirs \
    --include='*/' \
    --include='*.py' \
    --include='*.pyc' \
    --include='*.pyo' \
    --include='*.so' \
    --exclude='*' \
    "${src_root}/build/${isa}/python/" \
    "${bundle_root}/gem5/build/${isa}/python/"

while IFS= read -r module_so; do
    copy_needed_libs "${module_so}"
done < <(find "${bundle_root}/python" -type f -name '*.so')

run_gem5_template="/usr/local/bin/run-gem5.sh"
if [[ ! -f "${run_gem5_template}" ]]; then
    run_gem5_template="${script_dir}/run-gem5.sh"
fi

cp -a "${run_gem5_template}" "${bundle_root}/run.sh"
chmod +x "${bundle_root}/run.sh"

cat > "${bundle_root}/BUILD_INFO.txt" <<EOF
Container userspace:
$(cat /etc/os-release)

Compiler:
$(g++ --version | head -n 1)

Python:
$(python3 --version)

Binary:
${binary}
EOF

ldd "${binary}" > "${bundle_root}/BINARY_LDD.txt" || true
