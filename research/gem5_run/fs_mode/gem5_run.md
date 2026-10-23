
## derleme
```
scons build/RISCV/gem5.fast -j$(nproc)
scons build/RISCV/gem5.opt -j$(nproc)
```

## Checkpoint
```
IMAGE_FILES=$(realpath ../gem5-boot-tests-image-files)
build/RISCV/gem5.opt \
  -d m5out_runAtomic \
  ./research/gem5_run/config/research-config.py \
  --kernel $IMAGE_FILES/bootloader-vmlinux-5.10 \
  --disk-image $IMAGE_FILES/spec-research/ubuntu-riscv-spec.raw.img  \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --checkpoint-dir=m5out_bootCheckpoint \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-size 20GiB \
  --l1i_size 64KiB \
  --l1d_size 64KiB \
  --l2_size 2MiB

IMAGE_FILES=$(realpath ../gem5-kernels)
build/RISCV/gem5.opt \
  -d m5out_runAtomic2 \
  ./research/gem5_run/config/research-config_v2.py \
  --kernel $IMAGE_FILES/linux-build/vmlinux \
  --bootloader $IMAGE_FILES/opensbi/build/platform/generic/firmware/fw_jump.elf \
  --disk-image $IMAGE_FILES/ubuntu-24.04-riscv64.raw.img \
  --checkpoint-dir m5out_bootCheckpoint2 \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-size 16GiB
```

## Checkpoint'ten restore
```
build/RISCV/gem5.opt \
    -d m5out_specTest_gcc \
    ./research/gem5_run/config/research-config_v2.py \
    --kernel "$IMAGE_FILES/linux-build/vmlinux" \
    --bootloader "$IMAGE_FILES/opensbi/build/platform/generic/firmware/fw_jump.elf" \
    --disk-image "$IMAGE_FILES/ubuntu-24.04-riscv64.raw.img" \
    --cpu-type o3 \
    --r 1 \
    --restore-checkpoint ./m5out_runAtomic-ubuntu24-withSpec \
    --cpu-clock 2GHz \
    --sys-clock 1GHz \
    --mem-size 16GiB
```

## Boot Linux'u dinleme (test edildi)

```
# derle
cd ./util/term
make
make install

# çalıştır
./util/term/m5term localhost 3456
```

## çalışan araçtan stats alma
gem5 kapanmıyor, stats olduğu yerde güncelleniyor.
```
pgrep -af 'gem5.opt'
kill -USR1 <....>
grep -E '^(simInsts|hostSeconds|hostInstRate) ' \
    /path/to/m5out/stats.txt | tail -n 3
```

## qemu ile inst sayısı öğrenme
!! önemli not içinde system call ve interrupt mevzuları dahil değildir.
```
QEMU="$HOME/github_repos/qemu-count/build/qemu-riscv64"
PLUGIN="$HOME/github_repos/qemu-count/build/tests/tcg/plugins/libinsn.so"

"$QEMU" \
    -L /usr/riscv64-linux-gnu \
    -plugin "$PLUGIN,inline=true" \
    -d plugin \
    -D qemu.plugin.log \
    ./cpugcc_r_base.mytest \
    200.c -O3 -finline-limit=50000 \
    -o 200.opts-O3_-finline-limit_50000.s \
    > qemu.stdout 2> qemu.stderr

awk '/total insns:/ {
    n=$NF
    printf "Instructions: %.0f\n", n
    printf "gem5 time: %.2f hours\n", n/2400000/3600
}' qemu.plugin.log
```
