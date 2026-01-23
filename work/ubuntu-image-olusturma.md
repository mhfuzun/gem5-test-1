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
  e2fsprogs \
  dosfstools

# Disk image oluştur
IMG=ubuntu-riscv.img
SIZE_MB=8192

dd if=/dev/zero of=$IMG bs=1M count=$SIZE_MB status=progress

###################################################
# p1 → immutable rootfs
# p2 → system RW (checkpoint ile sabit kalacak)
# p3 → user RW (umount / reset edilebilir)
###################################################

sudo parted $IMG --script \
  mklabel msdos \
  mkpart primary ext4 1MiB 2049MiB \
  mkpart primary ext4 2049MiB 100%

LOOP=$(sudo losetup --find --show --partscan $IMG)
echo "Loop device: $LOOP"

#
# Kontrol için:
#  sudo fdisk /dev/loop7
#  fdisk aracı içinde p'ye bas.
#

sudo mkfs.ext4 -O ^has_journal -E lazy_itable_init=0,lazy_journal_init=0 ${LOOP}p1
sudo mkfs.ext4 ${LOOP}p2

# Mount Hiyerarşisi
sudo mkdir -p /mnt/ubuntu
sudo mount ${LOOP}p1 /mnt/ubuntu
sudo mkdir -p /mnt/ubuntu/data
sudo mount ${LOOP}p2 /mnt/ubuntu/data

# RISC-V minimal Ubuntu kur (Jammy önerilir)
sudo debootstrap \
  --arch=riscv64 \
  --foreign \
  jammy \
  /mnt/ubuntu \
  http://ports.ubuntu.com/

# kalan mount işlemleri
sudo mount -t proc proc /mnt/ubuntu/proc
sudo mount -t sysfs sys /mnt/ubuntu/sys
sudo mount --bind /dev /mnt/ubuntu/dev

# chroot işlemleri
sudo cp /usr/bin/qemu-riscv64-static /mnt/ubuntu/usr/bin/
sudo chroot /mnt/ubuntu /debootstrap/debootstrap --second-stage

#####
sudo chroot /mnt/ubuntu
#####

# fstab (RO Root + RW Data)
cat > /etc/fstab <<EOF
# Immutable root
/dev/vda1  /        ext4  ro,noatime,errors=panic   0  1

# System RW (checkpoint-stable)
/dev/vda2  /data   ext4  rw,noatime                0  2

# tmpfs
tmpfs  /tmp      tmpfs rw,nosuid,nodev,size=64M   0 0
tmpfs  /run      tmpfs rw,nosuid,nodev,size=32M   0 0
tmpfs  /var/log  tmpfs rw,nosuid,nodev,size=32M   0 0
EOF

# User RW alanı – home
mkdir -p /data/home
ln -s /data/home /home

groupadd -g 1000 myusers
useradd \
  --uid 1000 \
  --gid 1000 \
  --home /home/myuser \
  --create-home \
  --shell /bin/bash \
  myuser

echo "myuser:myuser123" | chpasswd
chown -R myuser:myusers /home/myuser

# Journald RAM-only
cat > /etc/systemd/journald.conf <<EOF
[Journal]
Storage=volatile
RuntimeMaxUse=8M
EOF

systemctl disable \
  apt-daily.service \
  apt-daily.timer \
  apt-daily-upgrade.service \
  apt-daily-upgrade.timer \
  systemd-logind.service \
  systemd-timesyncd.service \
  rsyslog.service

# Serial Console (gem5 için kritik)
systemctl disable getty@tty1.service
systemctl enable serial-getty@ttyS0.service

# Gerekli Paketler (SPEC / PARSEC / gem5)
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
  libgcc-s1 \
  gem5-m5ops

# /var → RW Alan
mkdir -p /data/var
rm -rf /var
ln -s /data/var /var

# Root Şifre
passwd root

#####
exit
#####

# (Opsiyonel ama Güçlü) fsck Devre Dışı
# RO root + checkpoint için:
tune2fs -c 0 -i 0 ${LOOP}p1

#######################
# programları kopyala #
#######################

# Kapatma & Temizlik
sudo umount -R /mnt/ubuntu
sudo losetup -d $LOOP

# Sparse Olmayan Final Image
cp --sparse=never ubuntu-riscv.img ubuntu-riscv.raw.img
```

Bir sonraki çalıştırmada
```bash
sudo mount ubuntu-riscv.img /mnt/ubuntu
sudo chroot /mnt/ubuntu

apt install -y psmisc lsof procps
apt install -y sudo

# user oluşturma
mkdir -p /data/home
rm -rf /home
ln -s /data/home /home
useradd \
  --create-home \
  --home-dir /home/bench \
  --shell /bin/bash \
  --uid 1000 \
  --gid 1000 \
  bench

# grup oluşturma
groupadd -g 1000 bench

# Şifreyi boot öncesi ayarlama
echo "bench:bench123" | chpasswd

# Home dizini izinleri
chown -R bench:bench /data/home/bench
chmod 700 /data/home/bench

# sudo işleri
usermod -aG sudo bench

```

kernel parameter
```bash
console=ttyS0 root=/dev/vda1 ro
```

uzun hali ile:
```bash
./build/RISCV/gem5.opt \
  ./configs/example/riscv/fs_linux.py \
  --caches --l1i_size=16kB --l1d_size=16kB \
  --l2cache --l2_size=256kB \
  --mem-type=DDR4_2400_8x8 \
  --mem-size=10GB \
  --cpu-type=AtomicSimpleCPU \
  --kernel=./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image=./boot-tests/ubuntu-riscv-min.raw.img \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init"
```

### daha sonra açıldığında
```bash
LOOP=$(sudo losetup --find --show --partscan $IMG)
echo "Loop device: $LOOP"

# Mount Hiyerarşisi
sudo mkdir -p /mnt/ubuntu
sudo mount ${LOOP}p1 /mnt/ubuntu
sudo mkdir -p /mnt/ubuntu/data
sudo mount ${LOOP}p2 /mnt/ubuntu/data

sudo chroot /mnt/ubuntu

##########
# işler...
##########

exit

# Kapatma & Temizlik
sudo umount -R /mnt/ubuntu
sudo losetup -d $LOOP

# Sparse Olmayan Final Image
cp --sparse=never ubuntu-riscv.img ubuntu-riscv.raw.img
```
