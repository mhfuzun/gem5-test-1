sudo mount -o loop riscv-disk.img ./tmp/riscv-rootfs
sudo cp testbench ./tmp/riscv-rootfs/test/testbench
sudo chmod +x ./tmp/riscv-rootfs/test/testbench
riscv64-linux-gnu-readelf -l testbench | grep interpreter
sync
sudo umount ./tmp/riscv-rootfs
echo done!
