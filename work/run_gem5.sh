#!/bin/sh

GEM5_BINARY="../build/RISCV/gem5.opt"
CONFIG_SCRIPT="run.py"

# Mount the disk image
mkdir -p mnt
sudo mount -o loop,offset=<offset> riscv-disk.img mnt

# Copy your binary
sudo cp ./benchs/hello_world/hello mnt/root/

# Unmount
sudo umount mnt

# Run gem5
${GEM5_BINARY} ${CONFIG_SCRIPT}
