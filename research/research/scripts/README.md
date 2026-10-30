# SPEC CPU2017 / gem5: three-stage workflow

The scripts are designed for research simulation using **one SPECrate copy** per benchmark and independent gem5 `-d` output directories. Only the run directories (inputs + compiled RISC-V executables + SPEC command files) go into the guest; no full SPEC installation is required inside it.

## 1. Prepare (host with complete SPEC installation)

```bash
./01_prepare_spec.sh --spec-dir /opt/spec2017 --config mytest \
  --size ref --dest ./spec-runs/ref 519.lbm_r 502.gcc_r
```

The script builds the selected benchmarks, runs SPEC `runsetup` (without running the benchmarks), invokes `specinvoke -nn`, and creates `run_benchmark.sh` in each copied run directory. It removes setup-host absolute `cd` commands and machine-specific environment variables. Additional environment requirements from your SPEC config (e.g., LD_LIBRARY_PATH for custom shared libraries) must be reconfigured for the guest.

Mount your Ubuntu root filesystem (for example at `/mnt/ubuntu`) and copy:

```bash
sudo mkdir -p /mnt/ubuntu/data/spec2017
sudo rsync -a spec-runs/ref/ /mnt/ubuntu/data/spec2017/
sudo install -m 755 02_run_guest.sh /mnt/ubuntu/usr/local/bin/run-spec-one
```

IMPORTANT: `spec-runs/ref` must remain intact on the host to reconstruct and validate outputs. The full SPEC installation and reference files must also remain available at their original host paths; `compare.cmd` often contains absolute paths.

### spec derlemesi için docker kurulumu
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

### Docker çalıştırma
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

### Docker ortamını test etme
```
cat /etc/os-release
```

glibc için 2.35 bekliyoruz.
```
ldd --version | head -1

riscv64-linux-gnu-gcc --version
riscv64-linux-gnu-g++ --version
riscv64-linux-gnu-gfortran --version
```

 $SPEC değeri /spec olmalı.

```
cd /spec
source ./shrc

echo "$SPEC"
command -v runcpu
command -v specinvoke
```

### Docker'ı başlatma
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

```
runcpu \
    --config=spec_risc-v_cfg.cfg \
    --define build_ncpus=$(nproc) \
    --action=build \
    --rebuild \
    --tune=base \
    --copies=1 \
    502.gcc_r

runcpu \
    --config=spec_risc-v_cfg.cfg \
    --define build_ncpus=$(nproc) \
    --action=build \
    --rebuild \
    --tune=base \
    --copies=1 \
    all
```

### 01_prepare_spec.sh script'ini çalıştır
```
cd /work
```

```
./01_prepare_spec.sh \
    --spec-dir /spec \
    --config spec_risc-v_cfg.cfg \
    --size test \
    --dest /work/spec-runs/test \
    502.gcc_r
```

## 2. Run (inside gem5 guest)

```bash
run-spec-one --run-dir /data/spec2017/519.lbm_r/run_base_refrate_mytest.0000 \
  --out-dir /data/results --test 519.lbm_r --id o3-1
```

The exact run folder can be looked up in the host-generated `spec-runs/ref/manifest.tsv`.

Call `m5 resetstats` immediately before starting benchmark commands and `m5 dumpstats` immediately after. The guest script snapshots the list of files in the run directory before and after execution, archives new/modified files plus run logs/metadata, then transfers archive and checksum with `m5 writefile`. The base disk is covered by gem5's COW layer; the source disk image does not receive modifications. Compression and transfer happen **after** `m5 dumpstats`.

For a local/QEMU functional check without m5, add `--no-m5`. `m5` must be installed on the guest PATH otherwise.

Choose unique gem5 out dirs, e.g.

```
m5_test_grup_1/
  m5_out_test1/
    stats.txt
    spec_519.lbm_r_o3-1.tar.gz
    spec_519.lbm_r_o3-1.tar.gz.sha256
  m5_out_test2/
    stats.txt
    spec_502.gcc_r_o3-1.tar.gz
    spec_502.gcc_r_o3-1.tar.gz.sha256
```

The guest out-dir `/data/results` is unrelated to the gem5 host `-d` directory. The `m5 writefile` destination is the **host gem5 output directory** for the invoking simulator process.

## 3. Verify (host with complete SPEC installation)

```bash
./03_verify_host.sh --group ./m5_test_grup_1 \
  --prepared ./spec-runs/ref --spec-dir /opt/spec2017
```

Results:

```
m5_test_grup_1/verified/
  expanded/m5_out_test1/spec_519.lbm_r_o3-1/  # unpacked archive
  reports/m5_out_test1/spec_519.lbm_r_o3-1/   # compare.log, miscompare files
  summary.tsv
```

It clones original prepared run contents into a temporary staging run directory, overlays the guest's changed files, applies deleted paths, and invokes `specinvoke -f compare.cmd` in that **reconstituted directory**. If desired, `--keep-work` preserves the reconstructed run directories, otherwise they are removed after verification.

The helper checks the command return code, `specinvoke exit: rc=N`, and `*.mis` files. It is not a substitute for the official SPEC reportable workflow. Some SPEC benchmarks execute a **benchmark-specific validation executable** in `compare.cmd`; if it was built for RISC-V, an x86-64 host cannot run it natively. Such tests need suitable QEMU user-mode/binfmt support or an appropriately built host-native validator. Standard `specdiff` comparisons use the host SPEC tools normally.

### Notes

- Do **not** run QEMU and gem5 simultaneously with write access to the same base image.
- All the guest benchmark binaries must match the guest's glibc (Ubuntu 22.04 normally has glibc 2.35).
- For `ref`, SPEC's run directory is often named `run_base_refrate_...0000` (not literally `run_base_ref_...`).
- `specinvoke -nn` can contain multiple commands and stdin/stdout redirections; this workflow supports **one copy** only. Target-specific configs that set custom environment variables may need a small adjustment in generated `run_benchmark.sh`.
- The run-directory snapshot compares file names, sizes, timestamps and types; extremely unusual software that modifies bytes while deliberately preserving both timestamps and size may escape capture.
- The benchmark stats window includes the small Bash process/launch overhead; fine-grained measurement needs instrumentation in the benchmark executable itself.
- `--no-m5` is for functional tests only. The guest script does not call `m5 exit`, and your current config.py therefore does not create a new checkpoint at the end of each test.

### Spec Testleri
  500.perlbench_r 502.gcc_r 505.mcf_r 520.omnetpp_r 523.xalancbmk_r 525.x264_r 531.deepsjeng_r 541.leela_r 548.exchange2_r 557.xz_r 999.specrand_ir \
  503.bwaves_r 507.cactuBSSN_r 508.namd_r 510.parest_r 511.povray_r 519.lbm_r 521.wrf_r 526.blender_r 527.cam4_r 538.imagick_r 544.nab_r 549.fotonik3d_r 554.roms_r 997.specrand_fr \
  600.perlbench_s 602.gcc_s 605.mcf_s 620.omnetpp_s 623.xalancbmk_s 625.x264_s 631.deepsjeng_s 641.leela_s 648.exchange2_s 657.xz_s 998.specrand_is \
  603.bwaves_s 607.cactuBSSN_s 619.lbm_s 621.wrf_s 627.cam4_s 628.pop2_s 638.imagick_s 644.nab_s 649.fotonik3d_s 654.roms_s 996.specrand_fs
