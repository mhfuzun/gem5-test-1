#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

usage() {
    cat <<'USAGE'
Boot the RISC-V Ubuntu disk with AtomicSimpleCPU and take a checkpoint.
By default this also prepares the mounted image's /work/run.sh so restore
continues with CoreMark under O3.

Examples:
  bash work/run_riscv_ubuntu_atomic_checkpoint.sh
  bash work/run_riscv_ubuntu_atomic_checkpoint.sh --iterations 200

Options:
  --kernel PATH          Bootloader+vmlinux image. Default: auto-detect bootloader-vmlinux-5.10
  --disk-image PATH      Ubuntu disk image. Default: auto-detect ubuntu-riscv-2gb.raw.img
  --image-files-dir DIR  Host dir containing kernel/disk images
  --mount-work-dir DIR   Mounted guest /work dir. Default: /mnt/gem5-ubuntu-riscv/work
  --no-prepare-image     Do not rewrite mounted /work/run.sh
  --iterations N         ITER value baked into post-checkpoint /work/run.sh. Default: 1
  --outdir DIR           gem5 output/checkpoint parent dir
  --gem5-bin PATH        gem5 binary. Default: build/RISCV/gem5.opt
  --mem-size SIZE        Memory size. Default: 2GB
  --root-device DEV      Linux root device. Default: /dev/vda1
  --command-line STR     Full Linux command line override
  --rel-max-tick N       Optional relative max tick
  --fs-arg ARG           Extra argument passed after fs_linux.py
  --gem5-arg ARG         Extra argument passed before fs_linux.py
USAGE
}

first_existing() {
    for path in "$@"; do
        if [[ -n "${path}" && -e "${path}" ]]; then
            printf '%s\n' "${path}"
            return 0
        fi
    done
    return 1
}

auto_kernel() {
    first_existing \
        "${KERNEL:-}" \
        "${IMAGE_FILES_DIR}/bootloader-vmlinux-5.10" \
        "work/bootloader-vmlinux-5.10" \
        "bootloader-vmlinux-5.10" \
        "work/riscv-bootloader-vmlinux-5.10" \
        "riscv-bootloader-vmlinux-5.10" || true
}

auto_disk() {
    first_existing \
        "${DISK_IMAGE:-}" \
        "${IMAGE_FILES_DIR}/ubuntu-riscv-2gb.raw.img" \
        "${IMAGE_FILES_DIR}/ubuntu-riscv.raw.img" \
        "${IMAGE_FILES_DIR}/ubuntu-riscv-min.raw.img" \
        "work/ubuntu-riscv-2gb.raw.img" \
        "ubuntu-riscv-2gb.raw.img" \
        "work/ubuntu-riscv.raw.img" \
        "ubuntu-riscv.raw.img" || true
}

GEM5_BIN="${GEM5_BIN:-build/RISCV/gem5.opt}"
FS_CONFIG="${FS_CONFIG:-configs/example/riscv/fs_linux.py}"
IMAGE_FILES_DIR="${IMAGE_FILES_DIR:-/home/tarator/github_repos/gem5-boot-tests-image-files}"
KERNEL="${KERNEL:-}"
DISK_IMAGE="${DISK_IMAGE:-}"
OUTDIR="${OUTDIR:-m5out/riscv-ubuntu-atomic-checkpoint}"

CPU_TYPE="${CPU_TYPE:-RiscvAtomicSimpleCPU}"
MEM_SIZE="${MEM_SIZE:-2GB}"
ROOT_DEVICE="${ROOT_DEVICE:-/dev/vda1}"
COMMAND_LINE="${COMMAND_LINE:-}"
SYS_CLOCK="${SYS_CLOCK:-1GHz}"
CPU_CLOCK="${CPU_CLOCK:-3GHz}"
CACHELINE_SIZE="${CACHELINE_SIZE:-64}"
L1I_SIZE="${L1I_SIZE:-32KiB}"
L1D_SIZE="${L1D_SIZE:-64KiB}"
L2_SIZE="${L2_SIZE:-2MiB}"
USE_CACHES="${USE_CACHES:-1}"
USE_L2CACHE="${USE_L2CACHE:-1}"
REL_MAX_TICK="${REL_MAX_TICK:-}"
MAX_CHECKPOINTS="${MAX_CHECKPOINTS:-1}"

ITERATIONS="${ITERATIONS:-1}"
MOUNT_WORK_DIR="${MOUNT_WORK_DIR:-/mnt/gem5-ubuntu-riscv/work}"
PREPARE_IMAGE="${PREPARE_IMAGE:-1}"
GUEST_CHECKPOINTER_CMD="${GUEST_CHECKPOINTER_CMD:-./checkpointer}"
GUEST_M5OPS_CMD="${GUEST_M5OPS_CMD:-./m5ops}"
GUEST_COREMARK_CMD="${GUEST_COREMARK_CMD:-./coremark_nIter}"

GEM5_STDOUT="${GEM5_STDOUT:-gem5.stdout}"
GEM5_STDERR="${GEM5_STDERR:-gem5.stderr}"

EXTRA_GEM5_ARGS=()
EXTRA_FS_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --kernel)
            KERNEL="$2"
            shift 2
            ;;
        --disk-image)
            DISK_IMAGE="$2"
            shift 2
            ;;
        --image-files-dir)
            IMAGE_FILES_DIR="$2"
            shift 2
            ;;
        --mount-work-dir)
            MOUNT_WORK_DIR="$2"
            shift 2
            ;;
        --no-prepare-image)
            PREPARE_IMAGE=0
            shift
            ;;
        --iterations)
            ITERATIONS="$2"
            shift 2
            ;;
        --outdir)
            OUTDIR="$2"
            shift 2
            ;;
        --gem5-bin)
            GEM5_BIN="$2"
            shift 2
            ;;
        --mem-size)
            MEM_SIZE="$2"
            shift 2
            ;;
        --root-device)
            ROOT_DEVICE="$2"
            shift 2
            ;;
        --command-line)
            COMMAND_LINE="$2"
            shift 2
            ;;
        --rel-max-tick)
            REL_MAX_TICK="$2"
            shift 2
            ;;
        --fs-arg)
            EXTRA_FS_ARGS+=("$2")
            shift 2
            ;;
        --gem5-arg)
            EXTRA_GEM5_ARGS+=("$2")
            shift 2
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

KERNEL="$(auto_kernel)"
DISK_IMAGE="$(auto_disk)"

if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "error: gem5 binary not found or not executable: ${GEM5_BIN}" >&2
    echo "hint: build it with: scons -Q -j12 build/RISCV/gem5.opt" >&2
    exit 1
fi
if [[ -z "${KERNEL}" || ! -f "${KERNEL}" ]]; then
    echo "error: bootloader-vmlinux-5.10 not found." >&2
    echo "hint: pass --kernel /path/to/bootloader-vmlinux-5.10" >&2
    exit 1
fi
if [[ -z "${DISK_IMAGE}" || ! -f "${DISK_IMAGE}" ]]; then
    echo "error: Ubuntu disk image not found." >&2
    echo "hint: pass --disk-image /path/to/ubuntu-riscv-2gb.raw.img" >&2
    exit 1
fi

mkdir -p "${OUTDIR}"

if [[ "${PREPARE_IMAGE}" == "1" || "${PREPARE_IMAGE,,}" == "true" ]]; then
    if [[ ! -d "${MOUNT_WORK_DIR}" || ! -w "${MOUNT_WORK_DIR}" ]]; then
        echo "error: mounted guest /work dir is not writable: ${MOUNT_WORK_DIR}" >&2
        echo "hint: mount the image at /mnt/gem5-ubuntu-riscv or pass --mount-work-dir DIR." >&2
        exit 1
    fi
    if [[ -e "${MOUNT_WORK_DIR}/run.sh" && ! -e "${MOUNT_WORK_DIR}/run.sh.before-mytage-checkpoint" ]]; then
        cp -p "${MOUNT_WORK_DIR}/run.sh" \
            "${MOUNT_WORK_DIR}/run.sh.before-mytage-checkpoint"
    fi
    cat > "${MOUNT_WORK_DIR}/run.sh" <<RUNSH
#!/bin/bash
set -e

cd /work
echo "[run.sh] taking checkpoint before CoreMark ROI"
${GUEST_CHECKPOINTER_CMD}

echo "[run.sh] restored after checkpoint; starting CoreMark"
echo "[run.sh] ITER=${ITERATIONS}"
${GUEST_M5OPS_CMD} reset
${GUEST_COREMARK_CMD} ${ITERATIONS}
${GUEST_M5OPS_CMD} dump
sync
poweroff -f || true
RUNSH
    chmod +x "${MOUNT_WORK_DIR}/run.sh"
fi

if [[ -z "${COMMAND_LINE}" ]]; then
    COMMAND_LINE="console=ttyS0 root=${ROOT_DEVICE} rw rootwait init=/sbin/init systemd.unit=multi-user.target"
fi

FS_ARGS=(
    "${FS_CONFIG}"
    --kernel="${KERNEL}"
    --disk-image="${DISK_IMAGE}"
    --cpu-type="${CPU_TYPE}"
    --num-cpus=1
    --mem-size="${MEM_SIZE}"
    --sys-clock="${SYS_CLOCK}"
    --cpu-clock="${CPU_CLOCK}"
    --cacheline_size="${CACHELINE_SIZE}"
    --l1i_size="${L1I_SIZE}"
    --l1d_size="${L1D_SIZE}"
    --l2_size="${L2_SIZE}"
    --root-device="${ROOT_DEVICE}"
    --command-line="${COMMAND_LINE}"
)

if [[ "${USE_CACHES}" == "1" || "${USE_CACHES,,}" == "true" ]]; then
    FS_ARGS+=(--caches)
fi
if [[ "${USE_L2CACHE}" == "1" || "${USE_L2CACHE,,}" == "true" ]]; then
    FS_ARGS+=(--l2cache)
fi
if [[ -n "${REL_MAX_TICK}" ]]; then
    FS_ARGS+=(--rel-max-tick="${REL_MAX_TICK}")
fi
if [[ -n "${MAX_CHECKPOINTS}" ]]; then
    FS_ARGS+=(--max-checkpoints="${MAX_CHECKPOINTS}")
fi
FS_ARGS+=("${EXTRA_FS_ARGS[@]}")

CMD=(
    "${GEM5_BIN}"
    --outdir="${OUTDIR}"
    "${EXTRA_GEM5_ARGS[@]}"
    "${FS_ARGS[@]}"
)

printf '%q ' "${CMD[@]}" > "${OUTDIR}/command.txt"
printf '\n' >> "${OUTDIR}/command.txt"

echo "[gem5] outdir/checkpoint parent: ${OUTDIR}"
echo "[gem5] stdout: ${OUTDIR}/${GEM5_STDOUT}"
echo "[gem5] stderr: ${OUTDIR}/${GEM5_STDERR}"
echo "[guest] mounted /work: ${MOUNT_WORK_DIR}"
echo "[guest] /work/run.sh: checkpoint, then m5ops reset, coremark_nIter ${ITERATIONS}, m5ops dump"
echo "[guest] console: ${OUTDIR}/system.pc.com_1.device"
echo "[kernel] ${KERNEL}"
echo "[disk] ${DISK_IMAGE}"

set +e
"${CMD[@]}" >"${OUTDIR}/${GEM5_STDOUT}" 2>"${OUTDIR}/${GEM5_STDERR}"
status=$?
set -e

echo "[gem5] exit status: ${status}"
if compgen -G "${OUTDIR}/cpt.*" >/dev/null; then
    echo "[gem5] checkpoints:"
    find "${OUTDIR}" -maxdepth 1 -type d -name 'cpt.*' -printf '  %f\n' | sort -V
else
    echo "[gem5] warning: no cpt.* directory was produced." >&2
fi

exit "${status}"
