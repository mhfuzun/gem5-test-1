sudo mount -o loop riscv_disk.img ./tmp/riscv-rootfs
sudo cp linux-init-rutine.sh ./tmp/riscv-rootfs/test/linux-init-rutine.sh
sudo cp testbench ./tmp/riscv-rootfs/test/testbench
sudo cp coremark ./tmp/riscv-rootfs/test/coremark
sudo chmod +x ./tmp/riscv-rootfs/test/linux-init-rutine.sh
sudo chmod +x ./tmp/riscv-rootfs/test/testbench
sudo chmod +x ./tmp/riscv-rootfs/test/coremark
riscv64-linux-gnu-readelf -l testbench | grep interpreter
riscv64-linux-gnu-readelf -l coremark | grep coremark
sync
sudo umount ./tmp/riscv-rootfs
echo done!
