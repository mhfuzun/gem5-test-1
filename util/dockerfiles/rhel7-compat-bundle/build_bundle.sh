#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../.." && pwd)"

image_tag="${IMAGE_TAG:-gem5-rhel7-compat-bundle}"
isa="${ISA:-RISCV}"
jobs="${JOBS:-$(nproc)}"
out_dir="${OUT_DIR:-${repo_root}/bundle-out/${isa}}"
docker_config_args=()
temp_docker_config=""

mkdir -p "${out_dir}"

if [[ -f "${HOME}/.docker/config.json" ]] &&
   grep -q '"credsStore"[[:space:]]*:[[:space:]]*"desktop"' "${HOME}/.docker/config.json" &&
   ! command -v docker-credential-desktop >/dev/null 2>&1; then
    temp_docker_config="$(mktemp -d)"
    cat > "${temp_docker_config}/config.json" <<'EOF'
{
  "auths": {}
}
EOF
    docker_config_args=(env DOCKER_CONFIG="${temp_docker_config}")
    trap 'rm -rf "${temp_docker_config}"' EXIT
    echo "Using a temporary Docker config because docker-credential-desktop is not installed."
fi

"${docker_config_args[@]}" docker build -t "${image_tag}" "${script_dir}"

"${docker_config_args[@]}" docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e ISA="${isa}" \
    -e JOBS="${jobs}" \
    -e HOME=/tmp \
    -v "${repo_root}:/src/gem5" \
    -v "${out_dir}:/out" \
    "${image_tag}" \
    bash -lc "
        rm -rf /tmp/gem5-work &&
        mkdir -p /tmp/gem5-work &&
        rsync -a --delete --exclude .git/ --exclude build/ --exclude bundle-out/ /src/gem5/ /tmp/gem5-work/ &&
        cd /tmp/gem5-work &&
        scons build/${isa}/gem5.opt -j${jobs} --without-tcmalloc PYTHON_CONFIG=python3-config &&
        collect-runtime.sh /tmp/gem5-work ${isa} /out &&
        cd /out &&
        zip -qr gem5-${isa}-rhel7-compat-bundle.zip .
    "

echo "Bundle ready under: ${out_dir}"
