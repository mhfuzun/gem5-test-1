# Gem5 notları
notlar.

## gem5 + Docker + VS Code (Cihazdan Bağımsız Geliştirme)

Bu doküman, gem5 geliştirme ortamının Docker kullanılarak
tüm cihazlarda **aynı şekilde** çalıştırılmasını açıklar.

Amaç:
- Host işletim sisteminden bağımsız derleme
- Tüm makinelerde aynı toolchain
- VS Code üzerinden ortak geliştirme
- Git ile senkronizasyon (push / pull)

---

## Gereksinimler (Her Cihazda)
```bash
sudo apt install -y \
  python3-full \
  python3-venv \
  python3-dev \
  scons \
  build-essential \
  git \
  m4
sudo apt install gcc-riscv64-linux-gnu

# Uyarılar için gerekli olanlar:
sudo apt install libgoogle-perftools-dev
sudo apt install libpng-dev
sudo apt install libhdf5-dev
sudo apt install protobuf-compiler libprotobuf-dev
sudo apt install libcapstone-dev
```
## Gereksinimler (Her Cihazda)

- Git
- Docker Desktop
- Visual Studio Code
- VS Code Extensions:
  - **Docker**
  - **Dev Containers (Remote - Containers)**

---

## Repo Yapısı (Önerilen)
```
gem5/
├── .devcontainer/
│ ├── devcontainer.json
│ └── Dockerfile
├── configs/
├── src/
├── README.md
---
```

## Dockerfile (Ubuntu + gem5 build ortamı)

`.devcontainer/Dockerfile`

```dockerfile
FROM ubuntu:22.04

RUN apt update && apt install -y \
    build-essential \
    scons \
    python3 \
    python3-pip \
    git \
    gcc-riscv64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
```

## devcontainer.json
.devcontainer/devcontainer.json
```JSON
{
  "name": "gem5-dev",
  "build": {
    "dockerfile": "Dockerfile"
  },
  "workspaceFolder": "/work",
  "extensions": [
    "ms-azuretools.vscode-docker"
  ]
}
```

## Kullanım Adımları (Yeni Bir Cihazda)
1. Repo’yu klonla
```bash
git clone <forked-gem5-repo>
cd gem5
```

2. VS Code ile aç
```bash
code .
```

3. Container içinde aç
```bash
VS Code otomatik olarak şunu sorar:
“Reopen in Container?”
→ Yes
```

## gem5 build
ilk defa için:
```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
scons build/RISCV/gem5.opt -j$(nproc)
```

vs-code açlışında:
```bash
source venv/bin/activate
```
boot-test klasörü:
https://www.gem5.org/documentation/gem5art/tutorials/boot-tutorial

mkdir boot-tests
cd boot-tests
git init
git remote add origin https://your-remote-add/boot-tests.git

virtualenv -p python3 venv
source venv/bin/activate

pip install gem5art-artifact gem5art-run gem5art-tasks

mkdir disk-image

Celuk (TOBB):
```bash
sudo apt install \
build-essential \
git \
m4 \
scons \
zlib1g \
zlib1g-dev \
libprotobuf-dev \
protobuf-compiler \
libprotoc-dev \
libgoogle-perftools-dev \
python3-dev \
python3-six \
python-is-python3 \
libboost-all-dev \
pkg-config
```

### Required Downloads

Download prebuilt ucanlinux riscv disk image from here:

dist.gem5.org/dist/v22-1/images/riscv/busybox/riscv-disk.img.gz

Download prebuilt bootloader from here:

https://github.com/UCanLinux/riscv64-sample/blob/master/bbl

#### or
Ubuntu:
Download riscv64 linux image from here:
https://old-releases.ubuntu.com/releases/22.04.3/ubuntu-22.04.3-preinstalled-server-riscv64+unmatched.img.xz

Extract the disk image:
xz -d -v ubuntu-22.04.3-preinstalled-server-riscv64+unmatched.img.xz

Download bootloader image from here:
http://dist.gem5.org/dist/v22-1/kernels/riscv/static/bootloader-vmlinux-5.10
```bash
#UcanLinux
$ ./build/RISCV/gem5.opt ./configs/example/riscv/fs_linux.py \
--caches --l1i_size=16kB --l1d_size=16kB --l2cache --l2_size=256kB --mem-type=DDR4_2400_8x8 --mem-size=3GB --cpu-type=TimingSimpleCPU \
--kernel=./boot-tests/bbl \
--disk-image=./boot-tests/riscv_disk.img \
--command-line="console=ttyS0 root=/dev/vda ro"

# Ubuntu
./build/RISCV/gem5.opt \
./configs/example/riscv/fs_linux.py \
--caches --l1i_size=16kB --l1d_size=16kB \
--l2cache --l2_size=256kB \
--mem-type=DDR4_2400_8x8 \
--mem-size=3GB \
--cpu-type=TimingSimpleCPU \
--kernel=./boot-tests/bootloader-vmlinux-5.10 \
--disk-image=./boot-tests/ubuntu-22.04.3-preinstalled-server-riscv64+unmatched.img \
--command-line="console=ttyS0 root=/dev/vda1 ro"

# Ubuntu (minimal)
./build/RISCV/gem5.opt \
  ./configs/example/riscv/fs_linux.py \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=10GB \
  --cpu-type=AtomicSimpleCPU \
  --kernel=./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image=./boot-tests/ubuntu-riscv-min.raw.img \
  --command-line="console=ttyS0 root=/dev/vda ro"
```

[--cpu-type {AtomicSimpleCPU,BaseAtomicSimpleCPU,BaseMinorCPU,BaseNonCachingSimpleCPU,BaseO3CPU,BaseTimingSimpleCPU,DerivO3CPU,MinorCPU,NonCachingSimpleCPU,O3CPU,RiscvAtomicSimpleCPU,RiscvMinorCPU,RiscvNonCachingSimpleCPU,RiscvO3CPU,RiscvTimingSimpleCPU,TimingSimpleCPU}]

### risc-v linux içine program ekleme
```bash
cd ./boot-tests/
mkdir -p ./tmp/riscv-rootfs
sudo mount -o loop riscv_disk.img ./tmp/riscv-rootfs
sudo mkdir -p ./tmp/riscv-rootfs/test
riscv64-linux-gnu-gcc \
  -static \
  -O2 \
  -I../include \
  ../util/m5/src/abi/riscv/m5op.S \
  testbench.c \
  -o testbench
sudo cp testbench ./tmp/riscv-rootfs/test/testbench
sudo chmod +x ./tmp/riscv-rootfs/test/testbench
riscv64-linux-gnu-readelf -l testbench | grep interpreter
sync
sudo umount ./tmp/riscv-rootfs
```

Tekrar yükleme
```bash
sudo mount -o loop riscv-disk.img ./tmp/riscv-rootfs
sudo cp testbench ./tmp/riscv-rootfs/test/testbench
sudo chmod +x ./tmp/riscv-rootfs/test/testbench
riscv64-linux-gnu-readelf -l testbench | grep interpreter
sync
sudo umount ./tmp/riscv-rootfs
echo done!
```

```Python
kernel_cmd = [
    "console=ttyS0",
    "root=/dev/vda",
    "rw",
    "init=/test/testbench"
]
system.workload.command_line = " ".join(kernel_cmd)
```
#### Çalışma akışı (beklenen):
Linux boot eder
/test/testbench PID 1 olur
testbench çalışır
m5_exit() çağrılır
gem5 kapanır
m5out/stats.txt oluşur

#### Boot Linux'u dinleme (test edildi)
```bash
# derle
cd ./util/term
make
make install

# çalıştır
./util/term/m5term localhost 3456
```

#### ubuntu image oluşturma (test edildi)
```bash
# ilk defa için buradan başlanır.
# gerekli araçlar
sudo apt update
sudo apt install -y \
  debootstrap \
  qemu-user-static \
  binfmt-support \
  parted \
  e2fsprogs

# Disk image oluştur
dd if=/dev/zero of=ubuntu-riscv.img bs=1M count=8192
mkfs.ext4 ubuntu-riscv.img

# mount edilir
sudo mkdir -p /mnt/ubuntu-riscv
sudo mount ubuntu-riscv.img /mnt/ubuntu-riscv

# RISC-V minimal Ubuntu kur (Jammy önerilir)
sudo debootstrap \
  --arch=riscv64 \
  --foreign \
  jammy \
  /mnt/ubuntu-riscv \
  http://ports.ubuntu.com/

# QEMU RISC-V binary ekle (host’ta chroot için):
sudo cp /usr/bin/qemu-riscv64-static /mnt/ubuntu-riscv/usr/bin/

sudo chroot /mnt/ubuntu-riscv /debootstrap/debootstrap --second-stage

#########################################################
# daha öncesi, önceden yapılmış ise buradan başlanabilir.
sudo mount ubuntu-riscv.img /mnt/ubuntu-riscv
#########################################################
# ubuntuya bağlan
sudo chroot /mnt/ubuntu-riscv

# Gereksiz servisleri kaldır (hata verebilir, atla)
apt purge -y \
  snapd \
  cloud-init \
  systemd-timesyncd \
  systemd-resolved \
  rsyslog

apt autoremove -y

# SPEC / PARSEC için minimum paketler
apt install -y \
  build-essential \
  bash \
  coreutils \
  findutils \
  grep \
  sed \
  perl \
  python3 \
  make \
  libstdc++6 \
  libgcc-s1

# Login ayarla
passwd root

# fstab oluştur
cat > /etc/fstab <<EOF
/dev/vda  /  ext4  defaults  0 1
EOF

# Konsol ayarı (kritik)
systemctl disable getty@tty1.service
ln -sf /lib/systemd/system/serial-getty@.service /etc/systemd/system/getty.target.wants/serial-getty@ttyS0.service

# Serial console (en kritik)
systemctl enable serial-getty@ttyS0.service
systemctl disable getty@tty1.service

# Gereksiz servisleri devre dışı bırak
systemctl disable \
  apt-daily.service \
  apt-daily.timer \
  apt-daily-upgrade.service \
  apt-daily-upgrade.timer \
  systemd-journald.service \
  systemd-logind.service

# journald’i minimal moda al
cat > /etc/systemd/journald.conf <<EOF
[Journal]
Storage=volatile
RuntimeMaxUse=1M
EOF

# check point ve diğer gem5 terminal talimatları için
# kurulması gerekir.
apt install -y gem5-m5ops

# ek optimizasyonlar

# Disk’i kapat
exit
sudo umount /mnt/ubuntu-riscv

# Sparse olmayan kopya
cp --sparse=never ubuntu-riscv.img ubuntu-riscv.raw.img

# gem5 ile çalıştırma
./build/RISCV/gem5.opt \
  ./configs/example/riscv/fs_linux.py \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=10GB \
  --cpu-type=AtomicSimpleCPU \
  --kernel=./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image=./boot-tests/ubuntu-riscv-min.raw.img \
  --command-line="console=ttyS0 \
    root=/dev/vda \
    rootfstype=ext4 \
    rw \
    rootflags=errors=remount-ro \
    fsck.repair=yes \
    systemd.unit=multi-user.target \
    quiet"

# systemd olmadan, ucanlinux
./build/RISCV/gem5.opt \
  ./configs/example/riscv/fs_linux.py \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=10GB \
  --cpu-type=AtomicSimpleCPU \
  --kernel=./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image=./boot-tests/ubuntu-riscv-min.raw.img \
  --command-line="console=ttyS0 \
    root=/dev/vda ro \
    init=/bin/bash"

# systemd ile
./build/RISCV/gem5.opt \
  ./configs/example/riscv/fs_linux.py \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=10GB \
  --cpu-type=AtomicSimpleCPU \
  --kernel=./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image=./boot-tests/ubuntu-riscv-min.raw.img \
  --command-line="console=ttyS0 \
    root=/dev/vda ro \
    init=/sbin/init"
```

##### Optimizasyon detayları (test edildi)
```bash
# Çalışması GEREKMEYEN her şeyi kapat
systemctl disable \
  apt-daily.service \
  apt-daily.timer \
  apt-daily-upgrade.service \
  apt-daily-upgrade.timer \
  systemd-journald.service \
  systemd-logind.service \
  systemd-networkd.service \
  systemd-resolved.service \
  systemd-udevd.service \
  systemd-tmpfiles-setup.service \
  systemd-tmpfiles-clean.service \
  rsyslog.service

# journald’i tamamen kapat (çok büyük kazanç)
systemctl mask systemd-journald.service

# Tek konsol bırak
systemctl disable getty@tty1.service
systemctl enable serial-getty@ttyS0.service

# fstab’ı ultra minimal yap
cat > /etc/fstab <<EOF
/dev/vda  /  ext4  ro,noatime,nodiratime  0 1
EOF

# Çalışmayacak her şeyi sil
apt purge -y \
  bash-completion \
  man-db \
  manpages \
  info \
  vim \
  nano \
  less \
  perl-doc \
  python3-doc \
  locales \
  ubuntu-standard

apt install -y psmisc lsof procps

# Locale’leri kapat
rm -rf /usr/share/locale/*
rm -rf /usr/lib/locale/*

systemctl set-default multi-user.target
systemctl disable systemd-timesyncd.service
systemctl disable systemd-resolved.service
systemctl mask systemd-journald.service
systemctl disable systemd-udevd.service
systemctl disable systemd-udevd-control.socket
systemctl disable systemd-udevd-kernel.socket
systemctl disable systemd-random-seed.service
systemctl disable modprobe@drm.service
systemctl disable modprobe@fuse.service
systemctl disable modprobe@configfs.service
systemctl disable console-setup.service
systemctl disable keyboard-setup.service
systemctl set-default multi-user.target
systemctl disable systemd-timesyncd.service
systemctl disable systemd-resolved.service

# oto login talimatları (tavsiye edilmiyor!)
mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d

cat > /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear ttyS0 115200,38400,9600 vt102
EOF

systemctl daemon-reexec
systemctl restart serial-getty@ttyS0
```

#### Checkpoint kullanımı
##### checkpoint alma (test edildi):
checkpoint alan araç:
terminalden çağrılacak
bu yüzden derlenip sonradan mount ile yüklenmesi gerekiyor.

```c
#include <stdint.h>
#include <stdio.h>

#include "gem5/m5ops.h"

int main() {
    // Her şey boot olduktan sonra terminalden çalıştır
    printf("Checkpoint alınacak...\n");
    m5_checkpoint(0, 0);  // hemen checkpoint
    printf("Checkpoint alındı.\n");
    return 0;
}
```

derleme için:
```bash
riscv64-linux-gnu-gcc \
  -static \
  -O2 \
  -I../include \
  ../util/m5/src/abi/riscv/m5op.S \
  checkpointer.c \
  -o checkpointer
```

mount ile ekleme
```bash
sudo mount ubuntu-riscv.img /mnt/ubuntu-riscv
sudo cp ../github_repos/gem5-test-1/boot-tests/checkpointer /mnt/ubuntu-riscv/home/checkpointer
sudo chmod +x /mnt/ubuntu-riscv/home/checkpointer
sudo umount /mnt/ubuntu-riscv
cp --sparse=never ubuntu-riscv.img ubuntu-riscv.raw.img
```

##### checkpoint'ten başlatma:
N: kaçıncı checkpoint olduğu. checkpointler m5out içinde tutulur.
"cpt.5454545" gibi gözükse de kaçıncı sırada olduğuna bakmak gerekiyor.
Eğer en başta "cpt.%" varsa bunu saymamalısın.

gem5 checkpoint'ten başlama komutu.
```bash
./build/RISCV/gem5.opt \
  ./configs/example/riscv/fs_linux.py \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=10GB \
  --restore-with-cpu=O3CPU \
  --checkpoint-dir=m5out \
  -r <N> \
  --kernel=./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image=./boot-tests/ubuntu-riscv-min.raw.img \
  --command-line="console=ttyS0 \
    root=/dev/vda ro \
    init=/sbin/init"
```

## commit işlemleri
```bash
git add .     # değişiklikleri güncelle
git status    # kontrol et
pre-commit run --all-files
git commit -m "<msg>" # message formatına uyulmalı
```

## Parsec ile ilgili komutlar
```bash
# repo
git clone https://github.com/cirosantilli/parsec-benchmark

# img içine kopyala
cp parsec /mnt/ubuntu/data/parsec

sudo chroot /mnt/ubuntu

sudo apt update
sudo apt install -y build-essential \
  gcc \
  g++ \
  make \
  m4 \
  perl \
  python3 \
  autoconf \
  automake \
  libtool \
  lib1g-dev

./configure
source env.sh

# build
./bin/parsecmgmt -a build -p blackscholes -c gcc

# run
source env.sh
./bin/parsecmgmt -a run -p blackscholes -i simsmall
./bin/parsecmgmt -a run -p blackscholes -i test
```
