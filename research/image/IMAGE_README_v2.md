```
#!/usr/bin/env bash
set -euo pipefail

IMG="$PWD/ubuntu-riscv-spec.raw.img"
ROOT=/mnt/ubuntu

# Sparse raw image (64 GiB sanal kapasite)
truncate -s 64G "$IMG"

sudo parted "$IMG" --script \
    mklabel msdos \
    mkpart primary ext4 1MiB 8192MiB \
    mkpart primary ext4 8192MiB 100%

LOOP=$(sudo losetup --find --show --partscan "$IMG")

sudo mkfs.ext4 -L gem5root "${LOOP}p1"
sudo mkfs.ext4 -L gem5data "${LOOP}p2"

sudo mkdir -p "$ROOT"
sudo mount "${LOOP}p1" "$ROOT"

sudo mkdir -p "$ROOT/data"
sudo mount "${LOOP}p2" "$ROOT/data"

sudo debootstrap \
    --arch=riscv64 \
    --foreign \
    jammy \
    "$ROOT" \
    http://ports.ubuntu.com/ubuntu-ports/

sudo cp /usr/bin/qemu-riscv64-static \
    "$ROOT/usr/bin/"

sudo mount -t proc proc "$ROOT/proc"
sudo mount -t sysfs sysfs "$ROOT/sys"
sudo mount --rbind /dev "$ROOT/dev"
sudo mount --make-rslave "$ROOT/dev"

sudo chroot "$ROOT" \
    /debootstrap/debootstrap --second-stage

sudo chroot "$ROOT" /bin/bash
```

```
cat > /etc/fstab <<'EOF'
LABEL=gem5root /     ext4 rw,noatime 0 1
LABEL=gem5data /data ext4 rw,noatime 0 2

tmpfs /tmp tmpfs rw,nosuid,nodev,size=512M 0 0
EOF

# Ev dizinini artık /data içinde oluşturuyoruz
mkdir -p /data/home

# spectest kullanıcısını oluştur ve ev dizinini /data/home/spectest olarak belirle
useradd -m -d /data/home/spectest -s /bin/bash -G sudo spectest
echo "spectest:gem5" | chpasswd
echo "spectest ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-spectest
chmod 0440 /etc/sudoers.d/90-spectest

exit

sudo umount -R /mnt/ubuntu
sudo losetup -d "$LOOP"
```

## Qemu için kurulum
### İmajı host üzerinde bağlayalım:
```
IMG="$PWD/ubuntu-riscv.img"
ROOT=/mnt/ubuntu

LOOP=$(sudo losetup --find --show --partscan "$IMG")

sudo mkdir -p "$ROOT"
sudo mount "${LOOP}p1" "$ROOT"

# İkinci partition varsa:
sudo mkdir -p "$ROOT/data"
sudo mount "${LOOP}p2" "$ROOT/data"

sudo mount -t proc proc "$ROOT/proc"
sudo mount -t sysfs sysfs "$ROOT/sys"
sudo mount --rbind /dev "$ROOT/dev"
sudo mount --make-rslave "$ROOT/dev"

sudo cp -L /etc/resolv.conf "$ROOT/etc/resolv.conf"

sudo chroot "$ROOT" /bin/bash
```

### Chroot içerisinde:
```
apt-get update

apt-get install -y \
    linux-image-generic \
    initramfs-tools

## Ardından kernel sürümünü bul:
ls /lib/modules
ls /boot/vmlinuz-*
ls /boot/initrd.img-*

## Şimdi U-Boot için boot yapılandırmasını oluştur:
KVER=$(ls -1 /lib/modules | sort -V | tail -n1)

update-initramfs -u -k "$KVER"

mkdir -p /boot/extlinux

cat > /boot/extlinux/extlinux.conf <<EOF
default ubuntu
timeout 3

label ubuntu
    menu label Ubuntu RISC-V
    linux /boot/vmlinuz-$KVER
    initrd /boot/initrd.img-$KVER
    append root=/dev/vda1 rw rootwait console=ttyS0
EOF

## Kernel ve initrd dosyalarının gerçekten mevcut olduğunu kontrol et:
test -f "/boot/vmlinuz-$KVER"
test -f "/boot/initrd.img-$KVER"
```

### final:
```
exit

sudo umount -R /mnt/ubuntu
sudo losetup -d "$LOOP"
```

## QEMU ile Ubuntu'yu başlat
```
IMG=/home/tarator/github_repos/gem5-boot-tests-image-files/spec-research/ubuntu-riscv-spec.raw.img
UBOOT=/home/tarator/github_repos/u-boot
qemu-system-riscv64 \
    -machine virt \
    -cpu rv64 \
    -smp 4 \
    -m 8G \
    -nographic \
    -bios default \
    -kernel "$UBOOT/build/u-boot" \
    -drive file="$IMG",format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::5555-:22 \
    -device virtio-net-device,netdev=net0
```


## Image'ı tekrar açma
```
IMG="$PWD/ubuntu-riscv.img"

LOOP=$(sudo losetup --find --show --partscan "$IMG")

sudo mkdir -p /mnt/ubuntu
sudo mount "${LOOP}p1" /mnt/ubuntu

sudo mkdir -p /mnt/ubuntu/data
sudo mount "${LOOP}p2" /mnt/ubuntu/data

sudo cp /usr/bin/qemu-riscv64-static \
    /mnt/ubuntu/usr/bin/

sudo chroot /mnt/ubuntu /bin/bash

exit

sudo umount -R /mnt/ubuntu
sudo losetup -d $LOOP
```

## Spec klasörünü kopayalama
```
# önce spec derlenir ve tek klasörde hazırlanır testler.
IMG="$PWD/ubuntu-riscv.img"

LOOP=$(sudo losetup --find --show --partscan "$IMG")

sudo mkdir -p /mnt/ubuntu/data
sudo mount "${LOOP}p2" /mnt/ubuntu/data

sudo mkdir -p /mnt/ubuntu/data/spec2017

sudo rsync -a \
    spec-runs/ \
    /mnt/ubuntu/data/spec2017/

sync

sudo umount /mnt/ubuntu/data
sudo losetup -d "$LOOP"
```

## spec gerekli yazılımlar
```
sudo apt update

sudo apt install -y --no-install-recommends \
    libc6 \
    libgcc-s1 \
    libstdc++6 \
    libgfortran5 \
    libgomp1 \
    libatomic1 \
    libgmp10 \
    libmpfr6 \
    libmpc3 \
    zlib1g \
    libbz2-1.0 \
    liblzma5 \
    libzstd1 \
    libxml2
```

## spec derlemesi için docker kurulumu
```
sudo apt install -y docker.io

mkdir -p ~/spec-jammy-build
cd ~/spec-jammy-build

# Dockerfile:
nano Dockerfile
```

### docker içeriği
```
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      gcc-riscv64-linux-gnu \
      g++-riscv64-linux-gnu \
      gfortran-riscv64-linux-gnu \
      libc6-dev-riscv64-cross \
      make perl python3 \
      file binutils ca-certificates rsync \
      && rm -rf /var/lib/apt/lists/*

ARG HOST_UID=1000
ARG HOST_GID=1000

RUN groupadd -g ${HOST_GID} specgroup && \
    useradd -m -u ${HOST_UID} -g ${HOST_GID} \
      -s /bin/bash specuser

USER specuser
ENV HOME=/home/specuser

WORKDIR /spec

```

```
sudo docker build \
    --build-arg HOST_UID="$(id -u)" \
    --build-arg HOST_GID="$(id -g)" \
    -t spec-riscv-jammy \
    .
```

## Docker çalıştırma
pwd script klasörü olmalı,
spec host ile spec klasörü verilmeli,
ardından runcpu ile çalıştırma yapmak mümkün.
```
SPEC_HOST="/SPEC/KURULUM/DIZININ"
sudo docker run --rm -it \
    --mount type=bind,source="$(realpath "$SPEC_HOST")",target=/spec \
    --mount type=bind,source="$(pwd)",target=/work \
    --workdir /work \
    spec-riscv-jammy \
    /bin/bash
```

## m5 aracını image içine ekleme
```
M5_BIN=$(realpath util/m5/build/riscv/out/m5)

IMG=$(realpath ../gem5-kernels/ubuntu-24.04-riscv64.raw.img)

LOOP=$(sudo losetup --find --show --partscan "$IMG")
lsblk -f "$LOOP"

sudo mkdir -p /mnt/gem5-root
sudo mount "${LOOP}p1" /mnt/gem5-root

sudo install -Dm755 "$M5_BIN" \
    /mnt/gem5-root/usr/local/bin/m5

sync
sudo umount /mnt/gem5-root
sudo losetup -d "$LOOP"
```
