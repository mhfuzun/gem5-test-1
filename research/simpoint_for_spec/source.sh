# GEM5, BOOTFILES, SimPoint

export GEM5_MODE=fast
export GEM5_BIN=$GEM5/build/RISCV/gem5.$GEM5_MODE
export CONFIG="$PWD/fs_simpoint.py"

export KERNEL=$BOOTFILES/linux-build/vmlinux
export BOOTLOADER=$BOOTFILES/opensbi/build/platform/generic/firmware/fw_jump.elf
export DISK_IMAGE=$BOOTFILES/ubuntu-24.04-riscv64.raw.img

export BOOT_CPT=$GEM5/m5out_runAtomic-ubuntu24-withSpec
export SIMPOINT_BIN=$SimPoint/bin/simpoint
export OUT="$PWD/simpoint_results/xz_r_ref_fast"
export INTERVAL=10000000
export MAX_K=10
export WARMUP=10000000
bash ./run_simpoint.sh
