#!/usr/bin/env bash
set -Eeuo pipefail

###############################################################################
# gem5-image-mount.sh
#
# Var olan gem5 Ubuntu RISC-V image dosyasını mount/unmount etmek için.
#
# Bu script:
#   - image oluşturmaz
#   - partition tablosu yazmaz
#   - mkfs yapmaz
#   - sadece mevcut image'ı loop device olarak bağlar
#
# Varsayılan partition yapısı:
#   /dev/vda1 -> /
#   /dev/vda2 -> /data
#   /dev/vda3 -> /work
#
# Kullanım:
#   chmod +x gem5-image-mount.sh
#
#   IMG=ubuntu-riscv.raw.img ./gem5-image-mount.sh mount
#   sudo cp -a ./dosyam /mnt/gem5-ubuntu-riscv/work/
#   ./gem5-image-mount.sh unmount
#
# Opsiyonel:
#   IMG=ubuntu-riscv.raw.img MNT=/mnt/my-gem5 ./gem5-image-mount.sh mount
###############################################################################

IMG="${IMG:-ubuntu-riscv.raw.img}"
MNT="${MNT:-/mnt/gem5-ubuntu-riscv}"
ROOT_MODE="${ROOT_MODE:-rw}"

STATE_FILE="${STATE_FILE:-/tmp/gem5-image-mount.state}"

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

usage() {
  cat <<EOF
Kullanım:

  IMG=ubuntu-riscv.raw.img $0 mount
  IMG=ubuntu-riscv.raw.img $0 unmount
  IMG=ubuntu-riscv.raw.img $0 status

Değişkenler:

  IMG       Image dosyası
            Varsayılan: ubuntu-riscv.raw.img

  MNT       Mount dizini
            Varsayılan: /mnt/gem5-ubuntu-riscv

  ROOT_MODE Root partition mount modu
            Varsayılan: rw
            Alternatif: ro

Örnek:

  IMG=ubuntu-riscv.raw.img $0 mount

  sudo cp -a ./coremark /mnt/gem5-ubuntu-riscv/work/
  sudo chown -R 1000:1000 /mnt/gem5-ubuntu-riscv/work/coremark

  IMG=ubuntu-riscv.raw.img $0 unmount

EOF
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Komut bulunamadı: $1"
}

wait_for_partition() {
  local part="$1"
  local i

  for i in $(seq 1 30); do
    if [[ -b "${part}" ]]; then
      return 0
    fi
    sleep 0.2
  done

  die "Partition görünmedi: ${part}"
}

safe_umount() {
  local target="$1"

  if mountpoint -q "${target}"; then
    log "Unmount: ${target}"
    ${SUDO} umount "${target}"
  else
    log "Zaten mount değil: ${target}"
  fi
}

mount_image() {
  require_cmd losetup
  require_cmd mount
  require_cmd findmnt

  [[ -f "${IMG}" ]] || die "Image bulunamadı: ${IMG}"

  if [[ -e "${STATE_FILE}" ]]; then
    die "State dosyası zaten var: ${STATE_FILE}. Önce unmount yap veya state dosyasını kontrol et."
  fi

  if mountpoint -q "${MNT}"; then
    die "Mount noktası zaten mount edilmiş: ${MNT}"
  fi

  log "Image bağlanıyor:"
  log "  IMG: ${IMG}"
  log "  MNT: ${MNT}"

  ${SUDO} mkdir -p "${MNT}"

  LOOP="$(${SUDO} losetup --find --show --partscan "${IMG}")"
  log "Loop device: ${LOOP}"

  wait_for_partition "${LOOP}p1"
  wait_for_partition "${LOOP}p2"
  wait_for_partition "${LOOP}p3"

  log "Root partition mount ediliyor: ${LOOP}p1 -> ${MNT} (${ROOT_MODE})"
  ${SUDO} mount -o "${ROOT_MODE}" "${LOOP}p1" "${MNT}"

  log "Alt mount dizinleri hazırlanıyor..."
  ${SUDO} mkdir -p "${MNT}/data"
  ${SUDO} mkdir -p "${MNT}/work"

  log "Data partition mount ediliyor: ${LOOP}p2 -> ${MNT}/data"
  ${SUDO} mount -o rw "${LOOP}p2" "${MNT}/data"

  log "Work partition mount ediliyor: ${LOOP}p3 -> ${MNT}/work"
  ${SUDO} mount -o rw "${LOOP}p3" "${MNT}/work"

  cat > "${STATE_FILE}" <<EOF
IMG="${IMG}"
MNT="${MNT}"
LOOP="${LOOP}"
EOF

  log "Mount tamamlandı."
  echo
  log "Kullanılabilir yollar:"
  echo "  Root : ${MNT}"
  echo "  Data : ${MNT}/data"
  echo "  Home : ${MNT}/data/home"
  echo "  Work : ${MNT}/work"
  echo
  log "Örnek dosya kopyalama:"
  echo "  sudo cp -a ./coremark ${MNT}/work/"
  echo "  sudo chown -R 1000:1000 ${MNT}/work/coremark"
  echo
  log "İşin bitince:"
  echo "  IMG=${IMG} $0 unmount"
}

unmount_image() {
  require_cmd losetup
  require_cmd umount

  if [[ ! -e "${STATE_FILE}" ]]; then
    die "State dosyası bulunamadı: ${STATE_FILE}. Image bu script ile mount edilmemiş olabilir."
  fi

  # shellcheck source=/dev/null
  source "${STATE_FILE}"

  [[ -n "${MNT:-}" ]] || die "State içinde MNT yok."
  [[ -n "${LOOP:-}" ]] || die "State içinde LOOP yok."

  log "Sync yapılıyor..."
  sync

  safe_umount "${MNT}/work"
  safe_umount "${MNT}/data"
  safe_umount "${MNT}"

  if losetup "${LOOP}" >/dev/null 2>&1; then
    log "Loop detach: ${LOOP}"
    ${SUDO} losetup -d "${LOOP}"
  else
    warn "Loop zaten bağlı değil: ${LOOP}"
  fi

  rm -f "${STATE_FILE}"

  log "Unmount tamamlandı. Değişiklikler image içine yazıldı."
}

status_image() {
  echo "IMG        : ${IMG}"
  echo "MNT        : ${MNT}"
  echo "STATE_FILE : ${STATE_FILE}"
  echo

  if [[ -e "${STATE_FILE}" ]]; then
    echo "State:"
    cat "${STATE_FILE}"
    echo
  else
    echo "State dosyası yok."
    echo
  fi

  echo "Mount durumu:"
  findmnt "${MNT}" 2>/dev/null || true
  findmnt "${MNT}/data" 2>/dev/null || true
  findmnt "${MNT}/work" 2>/dev/null || true
}

ACTION="${1:-}"

case "${ACTION}" in
  mount)
    mount_image
    ;;
  unmount|umount)
    unmount_image
    ;;
  status)
    status_image
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    die "Bilinmeyen komut: ${ACTION}"
    ;;
esac
