#!/bin/bash
# download_disk.sh

echo "Downloading RISC-V disk image for gem5..."
wget http://dist.gem5.org/dist/develop/images/riscv/busybox/riscv-disk.img.gz

echo "Extracting..."
gunzip riscv-disk.img.gz

echo "Done! Disk image ready."
ls -lh riscv-disk.img
