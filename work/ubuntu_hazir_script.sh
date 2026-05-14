#!/usr/bin/env bash
set -Eeuo pipefail

###############################################################################
# make-gem5-ubuntu-riscv.sh
#
# gem5 full-system RISC-V için minimal Ubuntu Jammy disk image üretir.
#
# Mantık:
#   - IMG     -> kaynak/template image, mevcutsa ASLA değiştirilmez
#   - RAW_IMG -> IMG'den kopyalanan ve üzerinde işlem yapılan hazır image
#
# Özellikler:
#   - /dev/vda1 -> /      RO immutable rootfs
#   - /dev/vda2 -> /data  RW system data: /var, /home
#   - /dev/vda3 -> /work  RW benchmark alanı
#   - root şifresi: root
#   - myuser şifresi: myuser123
#   - ttyS0 serial autologin: myuser
#   - boot sırasında /work/run.sh varsa otomatik çalıştırır
#   - m5 varsa resetstats/dumpstats/exit çağırır
#
# Kullanım:
#   chmod +x make-gem5-ubuntu-riscv.sh
#   ./make-gem5-ubuntu-riscv.sh
#
# Opsiyonel:
#   IMG=ubuntu-riscv.img RAW_IMG=ubuntu-riscv.raw.img SIZE_MB=16384 TOOL_DIR=tools ./make-gem5-ubuntu-riscv.sh
#   M5_HOST_BIN=/path/to/gem5/util/m5/build/riscv/out/m5 ./make-gem5-ubuntu-riscv.sh
###############################################################################

IMG="${IMG:-ubuntu-riscv.img}"
RAW_IMG="${RAW_IMG:-ubuntu-riscv.raw.img}"
SIZE_MB="${SIZE_MB:-16384}"
TOOL_DIR="${TOOL_DIR:-tools}"
MNT="${MNT:-/mnt/gem5-ubuntu-riscv}"
UBUNTU_CODENAME="${UBUNTU_CODENAME:-jammy}"
UBUNTU_MIRROR="${UBUNTU_MIRROR:-http://ports.ubuntu.com/}"
M5_HOST_BIN="${M5_HOST_BIN:-}"
GEM5_ROOT="${GEM5_ROOT:-}"

LOOP=""
PART1_END_MIB=""
PART2_END_MIB=""

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

log() {
  echo "[+] $*"
}

warn() {
  echo "[!] $*" >&2
}

die() {
  echo "[ERROR] $*" >&2
  exit 1
}

on_error() {
  local line="$1"
  warn "Hata oluştu. Satır: ${line}"
  warn "Cleanup deneniyor..."
}

cleanup() {
  set +e

  if [[ -n "${MNT:-}" ]]; then
    if mountpoint -q "${MNT}/dev/pts"; then
      ${SUDO} umount "${MNT}/dev/pts"
    fi

    if mountpoint -q "${MNT}/dev"; then
      ${SUDO} umount "${MNT}/dev"
    fi

    if mountpoint -q "${MNT}/proc"; then
      ${SUDO} umount "${MNT}/proc"
    fi

    if mountpoint -q "${MNT}/sys"; then
      ${SUDO} umount "${MNT}/sys"
    fi

    if mountpoint -q "${MNT}/work"; then
      ${SUDO} umount "${MNT}/work"
    fi

    if mountpoint -q "${MNT}/data"; then
      ${SUDO} umount "${MNT}/data"
    fi

    if mountpoint -q "${MNT}"; then
      ${SUDO} umount "${MNT}"
    fi
  fi

  if [[ -n "${LOOP:-}" ]] && losetup "${LOOP}" >/dev/null 2>&1; then
    ${SUDO} losetup -d "${LOOP}"
  fi
}

trap 'on_error $LINENO' ERR
trap cleanup EXIT

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Komut bulunamadı: $1"
}

parse_size_mib() {
  local raw="$1"
  local normalized="${raw//[[:space:]]/}"
  normalized="${normalized,,}"

  if [[ "${normalized}" =~ ^([0-9]+)$ ]]; then
    printf '%s\n' "${BASH_REMATCH[1]}"
    return 0
  fi

  if [[ "${normalized}" =~ ^([0-9]+)(m|mb|mib)$ ]]; then
    printf '%s\n' "${BASH_REMATCH[1]}"
    return 0
  fi

  if [[ "${normalized}" =~ ^([0-9]+)(g|gb|gib)$ ]]; then
    printf '%s\n' "$(( ${BASH_REMATCH[1]} * 1024 ))"
    return 0
  fi

  die "SIZE_MB desteklenmiyor: ${raw}. Ornek: 2048, 2048M, 2G, 2GB"
}

calc_partition_layout() {
  local total_mib="$1"
  local available_mib
  local root_size_mib
  local data_size_mib
  local work_size_mib

  (( total_mib >= 2048 )) || die "SIZE_MB en az 2G/2048 olmali."

  if (( total_mib >= 6145 )); then
    PART1_END_MIB=4097
    PART2_END_MIB=6145
    return 0
  fi

  available_mib=$((total_mib - 1))
  root_size_mib=$((available_mib * 3 / 4))
  data_size_mib=$(((available_mib - root_size_mib) / 2))
  work_size_mib=$((available_mib - root_size_mib - data_size_mib))

  (( root_size_mib >= 1535 )) || die "Root partition cok kucuk kaldi: ${root_size_mib} MiB"
  (( data_size_mib >= 128 )) || die "Data partition cok kucuk kaldi: ${data_size_mib} MiB"
  (( work_size_mib >= 128 )) || die "Work partition cok kucuk kaldi: ${work_size_mib} MiB"

  PART1_END_MIB=$((1 + root_size_mib))
  PART2_END_MIB=$((PART1_END_MIB + data_size_mib))
}

wait_for_partition() {
  local part="$1"
  local i

  for i in $(seq 1 20); do
    if [[ -b "${part}" ]]; then
      return 0
    fi
    sleep 0.2
  done

  die "Partition görünmedi: ${part}"
}

copy_m5_if_available() {
  local dst="$1"

  if [[ -n "${M5_HOST_BIN}" ]]; then
    [[ -x "${M5_HOST_BIN}" ]] || die "M5_HOST_BIN var ama executable değil: ${M5_HOST_BIN}"
    ${SUDO} cp "${M5_HOST_BIN}" "${dst}/usr/local/bin/m5"
    ${SUDO} chmod +x "${dst}/usr/local/bin/m5"
    log "m5 kopyalandı: ${M5_HOST_BIN}"
    return 0
  fi

  if [[ -n "${GEM5_ROOT}" ]]; then
    local candidate="${GEM5_ROOT}/util/m5/build/riscv/out/m5"
    if [[ -x "${candidate}" ]]; then
      ${SUDO} cp "${candidate}" "${dst}/usr/local/bin/m5"
      ${SUDO} chmod +x "${dst}/usr/local/bin/m5"
      log "m5 kopyalandı: ${candidate}"
      return 0
    fi
  fi

  warn "m5 binary verilmedi. Image oluşacak ama m5 resetstats/dumpstats/exit kullanılamayabilir."
  warn "Örnek: M5_HOST_BIN=/path/to/gem5/util/m5/build/riscv/out/m5 ./make-gem5-ubuntu-riscv.sh"
}

log "Host araçları kontrol ediliyor..."

SIZE_MB="$(parse_size_mib "${SIZE_MB}")"
calc_partition_layout "${SIZE_MB}"

require_cmd dd
require_cmd parted
require_cmd losetup
require_cmd mkfs.ext4
require_cmd debootstrap
require_cmd qemu-riscv64-static
require_cmd cp

log "Gerekli host paketleri kuruluyor..."

${SUDO} apt-get update
${SUDO} apt-get install -y \
  debootstrap \
  qemu-user-static \
  binfmt-support \
  parted \
  e2fsprogs \
  dosfstools \
  ca-certificates

###############################################################################
# ÖNEMLİ KISIM
#
# IMG kaynak/template image olarak kabul edilir.
# Eğer yoksa sadece bir kere boş kaynak image oluşturulur.
# Bundan sonra IMG üzerinde hiçbir işlem yapılmaz.
###############################################################################

if [[ -e "${RAW_IMG}" ]]; then
  die "Çıkış image zaten var: ${RAW_IMG}. Önce sil veya RAW_IMG=başka_ad ver."
fi

if [[ ! -e "${IMG}" ]]; then
  log "Kaynak image yok. Boş kaynak image oluşturuluyor: ${IMG}, size=${SIZE_MB}MB"
  dd if=/dev/zero of="${IMG}" bs=1M count="${SIZE_MB}" status=progress
else
  log "Kaynak image bulundu, değiştirilmeyecek: ${IMG}"
fi

log "Kaynak image RAW_IMG olarak kopyalanıyor..."
log "Source : ${IMG}"
log "Output : ${RAW_IMG}"

cp --sparse=never "${IMG}" "${RAW_IMG}"

log "RAW_IMG boyutu kontrol ediliyor..."

RAW_SIZE_BYTES="$(stat -c%s "${RAW_IMG}")"
TARGET_SIZE_BYTES="$((SIZE_MB * 1024 * 1024))"

if (( RAW_SIZE_BYTES < TARGET_SIZE_BYTES )); then
  log "RAW_IMG küçük. Büyütülüyor:"
  log "  Eski boyut: $((RAW_SIZE_BYTES / 1024 / 1024)) MiB"
  log "  Yeni boyut: ${SIZE_MB} MiB"

  truncate -s "${TARGET_SIZE_BYTES}" "${RAW_IMG}"
else
  log "RAW_IMG zaten yeterli büyük:"
  log "  Mevcut boyut: $((RAW_SIZE_BYTES / 1024 / 1024)) MiB"
fi

###############################################################################
# Bundan sonraki tüm işlemler SADECE RAW_IMG üzerinde yapılır.
###############################################################################

log "Partition tablosu RAW_IMG üzerinde oluşturuluyor..."
log "  /dev/vda1: 1MiB -> ${PART1_END_MIB}MiB"
log "  /dev/vda2: ${PART1_END_MIB}MiB -> ${PART2_END_MIB}MiB"
log "  /dev/vda3: ${PART2_END_MIB}MiB -> 100%"

${SUDO} parted "${RAW_IMG}" --script \
  mklabel msdos \
  mkpart primary ext4 1MiB "${PART1_END_MIB}MiB" \
  mkpart primary ext4 "${PART1_END_MIB}MiB" "${PART2_END_MIB}MiB" \
  mkpart primary ext4 "${PART2_END_MIB}MiB" 100%

log "Loop device RAW_IMG için bağlanıyor..."

LOOP="$(${SUDO} losetup --find --show --partscan "${RAW_IMG}")"
log "Loop device: ${LOOP}"

wait_for_partition "${LOOP}p1"
wait_for_partition "${LOOP}p2"
wait_for_partition "${LOOP}p3"

log "Dosya sistemleri RAW_IMG partitionları üzerinde oluşturuluyor..."

${SUDO} mkfs.ext4 -F -O ^has_journal -E lazy_itable_init=0,lazy_journal_init=0 "${LOOP}p1"
${SUDO} mkfs.ext4 -F "${LOOP}p2"
${SUDO} mkfs.ext4 -F "${LOOP}p3"

log "Mount hiyerarşisi hazırlanıyor..."

${SUDO} mkdir -p "${MNT}"
${SUDO} mount "${LOOP}p1" "${MNT}"

${SUDO} mkdir -p "${MNT}/data"
${SUDO} mount "${LOOP}p2" "${MNT}/data"

${SUDO} mkdir -p "${MNT}/work"
${SUDO} mount "${LOOP}p3" "${MNT}/work"

log "Ubuntu ${UBUNTU_CODENAME} riscv64 minbase kuruluyor..."

${SUDO} debootstrap \
  --arch=riscv64 \
  --foreign \
  --variant=minbase \
  "${UBUNTU_CODENAME}" \
  "${MNT}" \
  "${UBUNTU_MIRROR}"

log "QEMU static ve DNS ayarları kopyalanıyor..."

${SUDO} cp /usr/bin/qemu-riscv64-static "${MNT}/usr/bin/"
${SUDO} cp -L /etc/resolv.conf "${MNT}/etc/resolv.conf"

log "Chroot mountları hazırlanıyor..."

${SUDO} mount -t proc proc "${MNT}/proc"
${SUDO} mount -t sysfs sys "${MNT}/sys"
${SUDO} mount --bind /dev "${MNT}/dev"
${SUDO} mount -t devpts devpts "${MNT}/dev/pts"

log "Debootstrap second-stage çalıştırılıyor..."

${SUDO} chroot "${MNT}" /debootstrap/debootstrap --second-stage

log "m5 binary kontrol/kopyalama..."

copy_m5_if_available "${MNT}"

log "Chroot içinde sistem ayarları yapılıyor..."

${SUDO} chroot "${MNT}" /bin/bash -eux <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive

echo "[chroot] apt update"

apt-get update

echo "[chroot] /var ve /home RW alana taşınıyor"

mkdir -p /data/var
if [ -d /var ] && [ ! -L /var ]; then
  cp -a /var/. /data/var/
  rm -rf /var
fi
ln -sfn /data/var /var

mkdir -p /data/home
if [ -d /home ] && [ ! -L /home ]; then
  cp -a /home/. /data/home/ || true
  rm -rf /home
fi
ln -sfn /data/home /home

mkdir -p /work

echo "[chroot] fstab yazılıyor"

cat > /etc/fstab <<'EOF'
# Immutable root
/dev/vda1  /        ext4  ro,noatime,errors=panic  0  1

# System RW
/dev/vda2  /data    ext4  rw,noatime               0  2

# Benchmark RW
/dev/vda3  /work    ext4  rw,noatime               0  2

# tmpfs
tmpfs  /tmp      tmpfs rw,nosuid,nodev,size=256M   0 0
tmpfs  /run      tmpfs rw,nosuid,nodev,size=128M   0 0
tmpfs  /var/log  tmpfs rw,nosuid,nodev,size=64M    0 0
tmpfs  /var/tmp  tmpfs rw,nosuid,nodev,size=256M   0 0
EOF

echo "[chroot] paketler kuruluyor"

apt-get update

apt-get install -y --no-install-recommends \
  systemd-sysv \
  sudo \
  bash \
  coreutils \
  findutils \
  grep \
  sed \
  gawk \
  perl \
  python3 \
  make \
  gcc \
  g++ \
  gfortran \
  libc6-dev \
  libstdc++6 \
  libgcc-s1 \
  libgomp1 \
  libatomic1 \
  binutils \
  file \
  tar \
  gzip \
  bzip2 \
  xz-utils \
  unzip \
  ca-certificates \
  procps \
  time \
  util-linux \
  hostname \
  passwd \
  login \
  adduser \
  initramfs-tools

echo "[chroot] hostname, hosts, machine-id"

echo "gem5-riscv" > /etc/hostname

cat > /etc/hosts <<'EOF'
127.0.0.1 localhost
127.0.1.1 gem5-riscv
EOF

systemd-machine-id-setup || true

echo "[chroot] kullanıcılar ve şifreler"

groupadd -g 1000 myusers 2>/dev/null || true

if ! id myuser >/dev/null 2>&1; then
  useradd \
    --uid 1000 \
    --gid 1000 \
    --home /home/myuser \
    --create-home \
    --shell /bin/bash \
    myuser
fi

echo "root:root" | chpasswd
echo "myuser:myuser123" | chpasswd

usermod -aG sudo myuser || true

cat > /etc/sudoers.d/90-myuser-nopasswd <<'EOF'
myuser ALL=(ALL) NOPASSWD:ALL
EOF
chmod 0440 /etc/sudoers.d/90-myuser-nopasswd

mkdir -p /home/myuser
chown -R myuser:myusers /home/myuser
chown -R myuser:myusers /work

echo "[chroot] bash profil"

cat > /home/myuser/.bashrc <<'EOF'
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
cd /work 2>/dev/null || cd ~
EOF

chown myuser:myusers /home/myuser/.bashrc

echo "[chroot] journald RAM-only"

mkdir -p /etc/systemd

cat > /etc/systemd/journald.conf <<'EOF'
[Journal]
Storage=volatile
RuntimeMaxUse=8M
EOF

echo "[chroot] serial autologin"

systemctl set-default multi-user.target || true

systemctl disable getty@tty1.service 2>/dev/null || true
systemctl enable serial-getty@ttyS0.service

mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d

cat > /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin myuser --noclear %I $TERM
EOF

echo "[chroot] gereksiz servisler kapatılıyor"

systemctl disable \
  apt-daily.service \
  apt-daily.timer \
  apt-daily-upgrade.service \
  apt-daily-upgrade.timer \
  systemd-timesyncd.service \
  systemd-networkd.service \
  systemd-networkd.socket \
  systemd-resolved.service \
  2>/dev/null || true

systemctl mask \
  apt-daily.service \
  apt-daily.timer \
  apt-daily-upgrade.service \
  apt-daily-upgrade.timer \
  systemd-timesyncd.service \
  2>/dev/null || true

echo "[chroot] gem5 workload runner"

cat > /usr/local/sbin/gem5-runner.sh <<'EOF'
#!/bin/bash
set +e

exec >/dev/ttyS0 2>&1

echo
echo "============================================================"
echo "[gem5-runner] boot completed"
echo "[gem5-runner] user: $(id || true)"
echo "[gem5-runner] uname: $(uname -a || true)"
echo "============================================================"
echo

M5="$(command -v m5 || true)"

if [ -n "$M5" ]; then
  echo "[gem5-runner] m5 found: $M5"
else
  echo "[gem5-runner] WARNING: m5 not found"
fi

if [ -x /work/run.sh ]; then
  echo "[gem5-runner] starting /work/run.sh"

  if [ -n "$M5" ]; then
    echo "[gem5-runner] m5 resetstats"
    m5 resetstats || true
  fi

  su - myuser -c "cd /work && ./run.sh"
  RC=$?

  echo "[gem5-runner] workload rc=$RC"

  if [ -n "$M5" ]; then
    echo "[gem5-runner] m5 dumpstats"
    m5 dumpstats || true

    echo "[gem5-runner] m5 exit"
    m5 exit || true
  fi

  echo "[gem5-runner] poweroff"
  poweroff -f || true
else
  echo "[gem5-runner] /work/run.sh yok."
  echo "[gem5-runner] serial autologin shell kullanabilirsin."
fi
EOF

chmod +x /usr/local/sbin/gem5-runner.sh

cat > /etc/systemd/system/gem5-workload.service <<'EOF'
[Unit]
Description=Run gem5 benchmark workload
After=local-fs.target
Wants=local-fs.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/gem5-runner.sh
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF

# Terminal modunda otomatik workload başlatma kapalı
# systemctl enable gem5-workload.service
systemctl disable gem5-workload.service 2>/dev/null || true

echo "[chroot] örnek /work/run.sh oluşturuluyor"

cat > /work/run.sh <<'EOF'
#!/bin/bash
set -e

echo "[run.sh] placeholder workload"
echo "[run.sh] Buraya CoreMark/SPEC komutlarını koy."
echo "[run.sh] Örnek:"
echo "  cd /work/coremark && ./coremark.exe 0x0 0x0 0x66 0 7 1 2000"

sleep 1
EOF

chmod 644 /work/run.sh
chown myuser:myusers /work/run.sh

echo "[chroot] apt clean"

apt-get clean
rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

echo "[chroot] tamam"

CHROOT

log "gerekli araçlar kopyalaniyor..."
${SUDO} cp "${TOOL_DIR}"/* "${MNT}/work"/
${SUDO} chmod +x "${MNT}/work"/*

log "ext4 fsck periyodu kapatılıyor..."

${SUDO} tune2fs -c 0 -i 0 "${LOOP}p1"
${SUDO} tune2fs -c 0 -i 0 "${LOOP}p2"
${SUDO} tune2fs -c 0 -i 0 "${LOOP}p3"

safe_umount() {
  local target="$1"

  if mountpoint -q "$target"; then
    log "Unmount: $target"
    ${SUDO} umount "$target"
  else
    log "Zaten mount değil: $target"
  fi
}

log "Sync ve unmount..."

sync

safe_umount "${MNT}/dev/pts"
safe_umount "${MNT}/dev"
safe_umount "${MNT}/proc"
safe_umount "${MNT}/sys"
safe_umount "${MNT}/work"
safe_umount "${MNT}/data"
safe_umount "${MNT}"

if [[ -n "${LOOP:-}" ]] && losetup "${LOOP}" >/dev/null 2>&1; then
  log "Loop detach: ${LOOP}"
  ${SUDO} losetup -d "${LOOP}"
fi

LOOP=""

log "Bitti."
log "Kaynak image değişmedi: ${IMG}"
log "Hazir image: ${RAW_IMG}"
log ""
log "gem5 kernel cmdline önerisi:"
log "console=ttyS0 root=/dev/vda1 ro rootwait init=/sbin/init systemd.unit=multi-user.target"
