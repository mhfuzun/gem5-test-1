## Kurulum

```
sudo apt update
sudo apt install -y \
  debootstrap \
  qemu-user-static \
  binfmt-support \
  parted \
  e2fsprogs \
  dosfstools

# ilk defa için buradan başlanır.
IMG=ubuntu-riscv.img
SIZE_MB=5120

# Diski oluştur
dd if=/dev/zero of=$IMG bs=1M count=$SIZE_MB status=progress

# 3 Partition oluştur
# p1: 1MB   - 2048MB (RootFS - RO)
# p3: 4096MB - 100%   (/data - RW)
sudo parted $IMG --script \
  mklabel msdos \
  mkpart primary ext4 1MiB 2048MiB \
  mkpart primary ext4 2048MiB 100%

LOOP=$(sudo losetup --find --show --partscan $IMG)
echo "Loop device: $LOOP"

# Dosya sistemlerini oluştur
sudo mkfs.ext4 -O ^has_journal -E lazy_itable_init=0,lazy_journal_init=0 ${LOOP}p1
sudo mkfs.ext4 ${LOOP}p3

sudo mkdir -p /mnt/ubuntu
# 1. Root'u bağla
sudo mount ${LOOP}p1 /mnt/ubuntu

# 3. /data dizinini oluştur ve bağla
sudo mkdir -p /mnt/ubuntu/data
sudo mount ${LOOP}p3 /mnt/ubuntu/data

# Ubuntu debootstrap kurulumu (Önceki mesajındaki gibi aynen yapılır)
# ... debootstrap komutları ...
# ... proc, sys, dev bind mount işlemleri ...
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

# ----- fstab Ayarı -----
# vda1: Root (Read-Only)
# vda3: /data (Read-Write)
cat > /etc/fstab <<EOF
/dev/vda1  /       ext4  ro,noatime,errors=panic   0  1
/dev/vda3  /data   ext4  rw,noatime                0  2

tmpfs  /tmp      tmpfs rw,nosuid,nodev,size=64M   0 0
tmpfs  /run      tmpfs rw,nosuid,nodev,size=32M   0 0
tmpfs  /var/log  tmpfs rw,nosuid,nodev,size=32M   0 0
tmpfs  /var/tmp  tmpfs rw,nosuid,nodev,size=64M   0 0
EOF

# ----- Kullanıcı Ekleme -----
apt-get update
apt-get install -y sudo nano bash-completion

# Ev dizinini artık /data içinde oluşturuyoruz
mkdir -p /data/home

# gem5user kullanıcısını oluştur ve ev dizinini /data/home/gem5user olarak belirle
useradd -m -d /data/home/gem5user -s /bin/bash -G sudo gem5user
echo "gem5user:gem5" | chpasswd
echo "gem5user ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-gem5user
chmod 0440 /etc/sudoers.d/90-gem5user

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

# /var → RW Alan
mkdir -p /data/var
rm -rf /var
ln -s /data/var /var

# Root Şifre
passwd root

apt-get update
apt-get install -y \
  psmisc lsof strace file htop less nano \
  tar gzip bzip2 xz-utils unzip \
  make libgomp1 build-essential \
  iproute2 iputils-ping curl wget

#####
exit
#####

## Gerekli programları kopyala

sudo umount -R /mnt/ubuntu
sudo losetup -d $LOOP

# Sparse Olmayan Final Image
cp --sparse=never $IMG ubuntu-riscv.raw.img

# 1. Bekleyen tüm I/O işlemlerini diske yaz (Çok önemli!)
sync

# 2. Data partition'ını unmount et
sudo umount /data

# 3. Gem5'e checkpoint alması için m5 komutu gönder (m5 binary'si sisteme kuruluysa)
m5 checkpoint

```

## Güncelleme
```
IMG=ubuntu-riscv-min.raw.img

LOOP=$(sudo losetup --find --show --partscan $IMG)
echo "Loop device: $LOOP"

sudo mkdir -p /mnt/ubuntu
# 1. Root'u bağla
sudo mount ${LOOP}p1 /mnt/ubuntu

# 2. /space dizinini oluştur ve bağla
sudo mkdir -p /mnt/ubuntu/space
sudo mount ${LOOP}p2 /mnt/ubuntu/space

# 3. /data dizinini oluştur ve bağla
sudo mkdir -p /mnt/ubuntu/data
sudo mount ${LOOP}p3 /mnt/ubuntu/data

#####
sudo chroot /mnt/ubuntu
#####

# ....

#####
exit
#####

sudo umount -R /mnt/ubuntu
sudo losetup -d $LOOP
```
