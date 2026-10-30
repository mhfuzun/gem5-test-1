Evet, Ubuntu 24.04 LTS (RISC-V 64-bit) ile sıfırdan yeni bir gem5 Full-System ortamı kuralım. Bu kez Linux kernel, OpenSBI ve Ubuntu disk imajını ayrı ayrı hazırlayalım. Önce QEMU'da, ardından gem5'te test edelim.

Önemli bir şey buldum: gem5'in resmi kaynaklarında Ubuntu 24.04 RISC-V desteği ve bunun için Linux 6.8.12 tabanlı bir yapılandırma bulunuyor. gem5 v25.0 dokümantasyonunda da Ubuntu 24.04 ile Linux 6.8.12 eşleşmesi belirtilmiş.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://www.gem5.org\&sz=32)

gem5

+1



## Hedef yapılandırmamız

| Bileşen         | Seçim                    |
| --------------- | ------------------------ |
| Mimari          | RISC-V RV64GC            |
| İşletim sistemi | Ubuntu 24.04 LTS (Noble) |
| Linux kernel    | 6.8.12                   |
| Firmware        | OpenSBI, `FW_JUMP`       |
| Disk formatı    | RAW, ext4                |
| Konsol          | `ttyS0`                  |
| Kullanıcı       | `spectest`               |
| Giriş           | Otomatik                 |
| Simülatör       | QEMU + gem5 FS           |

Ubuntu 24.04 ayrıca glibc 2.39 kullandığından, daha önce SPEC CPU derlemelerinde karşılaştığın `GLIBC_2.38 not found` problemi açısından da daha uygun.

Kurulumda mevcut Ubuntu 22.04 imajını değiştirmeyeceğiz.

## 1. Host üzerinde gerekli araçların kurulması

Linux Mint veya Ubuntu x86-64 üzerinde:

```
sudo apt update

sudo apt install -y \
    gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    qemu-system-misc \
    qemu-user-static \
    binfmt-support \
    debootstrap \
    ubuntu-keyring \
    build-essential \
    flex bison bc \
    libssl-dev libelf-dev \
    libncurses-dev \
    device-tree-compiler \
    parted e2fsprogs
```

Çalışma dizinimizi oluşturalım:

```
export WORK="$HOME/riscv-ubuntu24"

mkdir -p "$WORK"
cd "$WORK"
```

Mevcut Git klonlarını kullanacağız. Örneğin `~/linux` ve `~/opensbi` altındaysa:

```
export LINUX_SRC="$HOME/linux"
export OPENSBI_SRC="$HOME/opensbi"
export KBUILD="$WORK/linux-build"
```

Kaynaklar başka bir dizindeyse yalnızca bu değişkenleri değiştirmen yeterli.

## 2. Linux 6.8.12 derlemesi

Önce sürümü sabitleyelim. Mevcut klonda yaptığın değişiklikler varsa checkout öncesinde korumalısın.

```
cd "$LINUX_SRC"

git fetch origin tag v6.8.12
git checkout v6.8.12
```

Kernel yapılandırması:

```
make ARCH=riscv \
    CROSS_COMPILE=riscv64-linux-gnu- \
    O="$KBUILD" \
    defconfig
```

İlk aşamada initramfs kullanmayacağımız için root dosya sistemine erişmekte gerekli sürücüleri kernel içine alalım.

```
scripts/config --file "$KBUILD/.config" \
    -e DEVTMPFS \
    -e DEVTMPFS_MOUNT \
    -e EXT4_FS \
    -e VIRTIO \
    -e VIRTIO_MMIO \
    -e VIRTIO_BLK \
    -e VIRTIO_PCI \
    -e SERIAL_8250 \
    -e SERIAL_8250_CONSOLE \
    -e MSDOS_PARTITION \
    -d LOCALVERSION_AUTO
```

Bağımlılıkları yeniden çözümleyelim:

```
make ARCH=riscv \
    CROSS_COMPILE=riscv64-linux-gnu- \
    O="$KBUILD" \
    olddefconfig
```

Önemli ayarları kontrol et:

```
grep -E '^CONFIG_(EXT4_FS|VIRTIO_BLK|VIRTIO_MMIO|SERIAL_8250_CONSOLE|DEVTMPFS_MOUNT)=' \
    "$KBUILD/.config"
```

Bu beş ayarın `=y` olmasını istiyoruz. Ardından derleme:

```
make ARCH=riscv \
    CROSS_COMPILE=riscv64-linux-gnu- \
    O="$KBUILD" \
    -j"$(nproc)" \
    vmlinux Image modules
```

Çıktılar:

```
riscv-ubuntu24/
└── linux-build/
    ├── vmlinux
    └── arch/riscv/boot/Image
```

`vmlinux` dosyasını gem5, `Image` dosyasını QEMU için kullanacağız.

## 3. OpenSBI derlemesi

gem5'in resmî OpenSBI örneği `FW_JUMP_ADDR=0x80200000` ve `FW_JUMP_FDT_ADDR=0x87e00000` kullanıyor.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



```
cd "$OPENSBI_SRC"

git fetch origin tag v1.3.1
git checkout v1.3.1
```

Derleyelim:

```
make PLATFORM=generic \
    FW_JUMP=y \
    FW_JUMP_ADDR=0x80200000 \
    FW_JUMP_FDT_ADDR=0x87e00000 \
    CROSS_COMPILE=riscv64-linux-gnu- \
    -j"$(nproc)"
```

Çıktı:

```
opensbi/build/platform/generic/firmware/fw_jump.elf
```

Böylece gem5'ta önyükleme akışı şu olacak:

OpenSBI — M-mode

fw_jump.elf

Linux 6.8.12 — S-mode

vmlinux

Ubuntu 24.04 — Userspace

systemd → serial-getty → Bash

## 4. Ubuntu 24.04 RAW disk imajının oluşturulması

Bu kez `debootstrap` ile temiz bir sistem kuracağız. RISC-V Noble paketleri Ubuntu Ports deposunda bulunuyor.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://ports.ubuntu.com\&sz=32)

ports.ubuntu.com



### 4.1. Disk oluşturma

32 GB kapasiteli, sparse RAW disk oluşturalım:

```
cd "$WORK"

export IMG="$WORK/ubuntu-24.04-riscv64.raw.img"
export MNT="$WORK/rootfs"

mkdir -p "$MNT"

truncate -s 32G "$IMG"

parted -s "$IMG" mklabel msdos
parted -s "$IMG" mkpart primary ext4 1MiB 100%
```

Loop device bağla:

```
export LOOP=$(sudo losetup --find --show --partscan "$IMG")

echo "$LOOP"
```

Örneğin `/dev/loop0` dönebilir.

```
sudo udevadm settle

sudo mkfs.ext4 -F "${LOOP}p1"

sudo mount "${LOOP}p1" "$MNT"
```

Böylece QEMU/gem5 içerisinde root partition `/dev/vda1` olacak.

### 4.2. Ubuntu temel sisteminin kurulması

```
sudo debootstrap \
    --arch=riscv64 \
    --foreign \
    --variant=minbase \
    noble \
    "$MNT" \
    https://ports.ubuntu.com/ubuntu-ports/
```

QEMU kullanıcı modu emülasyonunu hazırlayalım:

```
sudo cp /usr/bin/qemu-riscv64-static "$MNT/usr/bin/"

sudo mount -t proc proc "$MNT/proc"

sudo mount --rbind /sys "$MNT/sys"
sudo mount --make-rslave "$MNT/sys"

sudo mount --rbind /dev "$MNT/dev"
sudo mount --make-rslave "$MNT/dev"
```

Binfmt kaydını kontrol edelim:

```
cat /proc/sys/fs/binfmt_misc/qemu-riscv64
```

Kayıt etkin değilse:

```
sudo update-binfmts --enable qemu-riscv64
```

İkinci aşamayı yürütelim:

```
sudo chroot "$MNT" /debootstrap/debootstrap --second-stage
```

Paket kaynaklarını yapılandıralım:

```
sudo tee "$MNT/etc/apt/sources.list" > /dev/null <<'EOF'
deb https://ports.ubuntu.com/ubuntu-ports noble main universe
deb https://ports.ubuntu.com/ubuntu-ports noble-updates main universe
deb https://ports.ubuntu.com/ubuntu-ports noble-security main universe
EOF
```

DNS:

```
echo "nameserver 1.1.1.1" | sudo tee "$MNT/etc/resolv.conf"
```

### 4.3. Chroot ve gerekli paketler

```
sudo chroot "$MNT" /bin/bash
```

İçeride:

```
apt update

DEBIAN_FRONTEND=noninteractive apt install -y \
    systemd-sysv \
    udev dbus \
    sudo \
    bash \
    login \
    util-linux \
    kmod \
    procps \
    iproute2 \
    locales \
    ca-certificates \
    file \
    vim \
    libstdc++6 \
    libgfortran5 \
    libgomp1 \
    libatomic1 \
    zlib1g
```

Böylece SPEC için temel çalışma zamanı kütüphanelerini de eklemiş oluyoruz. Bütün SPEC benchmark'larının bağımlılıklarını daha sonra tamamlayabiliriz.

`systemd-sysv` kurulduktan sonra `/sbin/init` üzerinden systemd açılabilecek.

## 5. Kullanıcı ve otomatik giriş

Hâlâ chroot içerisindeyiz.

### Kullanıcı oluşturma

```
useradd -m -s /bin/bash -G sudo spectest

echo "spectest:gem5" | chpasswd

echo "spectest ALL=(ALL) NOPASSWD:ALL" \
    > /etc/sudoers.d/90-spectest

chmod 0440 /etc/sudoers.d/90-spectest
```

Bu sefer kullanıcı home dizinini ilk test için `/home/spectest` bırakıyoruz. Eski `/data/home/spectest` yapılandırmasını taşımayacağız.

### ttyS0 otomatik giriş

```
mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
```

```
cat > /etc/systemd/system/serial-getty@ttyS0.service.d/override.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin spectest --keep-baud 115200,38400,9600 %I $TERM
EOF
```

Hostname:

```
echo "tarator-pc" > /etc/hostname

cat > /etc/hosts <<'EOF'
127.0.0.1 localhost
127.0.1.1 tarator-pc
EOF
```

Fstab:

```
cat > /etc/fstab <<'EOF'
/dev/vda1 / ext4 defaults 0 1
proc /proc proc defaults 0 0
EOF
```

Çıkalım:

```
exit
```

Host üzerinde ilgili getty servisini etkinleştirelim:

```
sudo systemctl --root="$MNT" enable serial-getty@ttyS0.service
```

Bu adımı özellikle ekledim; önceki imajda çalıştırmadığını belirtmiştin.

### Kernel modüllerinin yüklenmesi

Chroot'tan çıktıktan sonra kernel derleme dizinindeki modülleri imaja kuralım:

```
sudo make -C "$LINUX_SRC" \
    ARCH=riscv \
    CROSS_COMPILE=riscv64-linux-gnu- \
    O="$KBUILD" \
    INSTALL_MOD_PATH="$MNT" \
    modules_install
```

Hazırlık tamamlandı. Diskten çıkabiliriz:

```
sudo umount -R "$MNT/dev"
sudo umount -R "$MNT/sys"
sudo umount "$MNT/proc"
sudo umount "$MNT"

sudo losetup -d "$LOOP"
```

## 6. Önce QEMU ile test

QEMU'nun kendi varsayılan OpenSBI firmware'ini kullanarak derlediğimiz kernel ve Ubuntu diskini başlatalım.

```
qemu-system-riscv64 \
    -machine virt \
    -m 4G \
    -smp 2 \
    -nographic \
    -bios default \
    -kernel "$KBUILD/arch/riscv/boot/Image" \
    -append "console=ttyS0 root=/dev/vda1 rw rootwait" \
    -drive file="$IMG",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -net none
```

Beklenen çıktı:

```
Ubuntu 24.04 LTS tarator-pc ttyS0

tarator-pc login: spectest (automatic login)

spectest@tarator-pc:~$
```

Shell açıldığında:

```
uname -a
cat /etc/os-release
ldd --version
echo "HELLO FROM UBUNTU 24"
```

Burada Linux 6.8.12, Ubuntu 24.04 ve glibc 2.39 görmeyi bekliyoruz.

## 7. gem5 ile test

Mevcut `fs_linux_detailed_o3.py` betiğini kullanabiliriz. Ancak burada önceki sistemden farklı olarak OpenSBI ve kernel'i iki ayrı dosya halinde vereceğiz.

Önce betiğin `--bootloader` parametresini desteklediğini kontrol et:

```
build/RISCV/gem5.opt \
    configs/example/riscv/fs_linux_detailed_o3.py \
    --help | grep -E 'bootloader|kernel|disk-image'
```

Destekliyorsa ilk test için:

```
build/RISCV/gem5.opt -d m5out_ubuntu24 \
    configs/example/riscv/fs_linux_detailed_o3.py \
    --kernel "$KBUILD/vmlinux" \
    --bootloader "$OPENSBI_SRC/build/platform/generic/firmware/fw_jump.elf" \
    --disk-image "$IMG" \
    --command-line="console=ttyS0 root=/dev/vda1 rw rootwait" \
    --cpu-type=AtomicSimpleCPU \
    --mem-size 4GB
```

Burada özel betiğinin kernel'i `0x80200000` adresine, DTB'yi `0x87e00000` adresine yerleştirdiğini doğrulamamız gerekiyor. Bu, oluşturduğumuz OpenSBI firmware'inin beklentisi. Betik bu yerleşimi kullanmıyorsa adresleri uyarlamalıyız.

Önemli: İlk boot testinde O3 veya SPEC benchmark çalıştırmayalım. Atomic CPU ile shell prompt'unun geldiğini ve komut işlediğini kanıtlayalım. Böylece olası bir gem5 CPU model problemiyle Ubuntu kurulumu problemini birbirinden ayırabiliriz.

## Sonuçta elde edeceğimiz dosyalar

```
riscv-ubuntu24/
├── linux-build/
│   ├── vmlinux
│   ├── .config
│   └── arch/riscv/boot/Image
│
├── ubuntu-24.04-riscv64.raw.img
│
└── rootfs/
```

OpenSBI de mevcut Git klonunda derlenmiş olacak.

Sonraki aşamada SPEC CPU2017 dosyalarını bu imaja aktarabiliriz. Ubuntu 24.04 RISC-V'nin glibc 2.39 paketine sahip olduğu doğrulanıyor.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://packages.ubuntu.com\&sz=32)

Details of package libc6 in noble



Bir noktayı özellikle vurgulayayım: Eğer yeni Ubuntu 24.04 imajı QEMU'da sorunsuz çalışır ama gem5'te yine `Last login:` satırından sonra durursa, artık sorunun Ubuntu kullanıcı yapılandırmasından ziyade gem5'in kernel/TTY/CPU etkileşiminde olduğu yönünde çok daha güçlü kanıtımız olacak.
