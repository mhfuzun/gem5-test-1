# Gem5 notları
notlar.

# gem5 + Docker + VS Code (Cihazdan Bağımsız Geliştirme)

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

```bash
$ ./build/RISCV/gem5.opt ./configs/example/riscv/fs_linux.py --caches --l1i_size=16kB --l1d_size=16kB --l2cache --l2_size=256kB --mem-type=DDR4_2400_8x8 --mem-size=1GB --cpu-type=TimingSimpleCPU --kernel=./boot-tests/bbl --disk-image=./boot-tests/riscv-disk.img
```

[--cpu-type {AtomicSimpleCPU,BaseAtomicSimpleCPU,BaseMinorCPU,BaseNonCachingSimpleCPU,BaseO3CPU,BaseTimingSimpleCPU,DerivO3CPU,MinorCPU,NonCachingSimpleCPU,O3CPU,RiscvAtomicSimpleCPU,RiscvMinorCPU,RiscvNonCachingSimpleCPU,RiscvO3CPU,RiscvTimingSimpleCPU,TimingSimpleCPU}]

### risc-v linux içine program ekleme
```bash
cd ./boot-tests/
mkdir -p ./tmp/riscv-rootfs
sudo mount -o loop riscv-disk.img ./tmp/riscv-rootfs
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

# commit işlemleri
```bash
git add .     # değişiklikleri güncelle
git status    # kontrol et
pre-commit run --all-files
git commit -m "<msg>" # message formatına uyulmalı
```
