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
