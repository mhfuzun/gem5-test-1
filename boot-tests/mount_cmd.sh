sudo mount -o loop riscv-disk.img ./tmp/riscv-rootfs
sudo cp testbench ./tmp/riscv-rootfs/test/testbench
sudo cp coremark ./tmp/riscv-rootfs/test/coremark
sudo chmod +x ./tmp/riscv-rootfs/test/testbench
riscv64-linux-gnu-readelf -l testbench | grep interpreter
riscv64-linux-gnu-readelf -l coremark | grep coremark
sync
sudo umount ./tmp/riscv-rootfs
echo done!
