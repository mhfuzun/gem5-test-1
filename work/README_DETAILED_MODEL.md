
## Gem5 Derleme
```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

## Yürütme
```bash
build/RISCV/gem5.opt \
  configs/example/riscv/fs_linux_detailed_o3.py \
  --kernel ./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image ./boot-tests/ubuntu-riscv.raw.img \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-type DDR4_2400_8x8 \
  --mem-size 10GB \
  --mem-channels 2 \
  --l1i_size 64KiB \
  --l1d_size 64KiB \
  --l2_size 2MiB \
  --fetch-width 8 \
  --decode-width 8 \
  --rename-width 8 \
  --dispatch-width 8 \
  --issue-width 8 \
  --commit-width 8 \
  --num-rob-entries 256 \
  --num-iq-entries 128 \
  --lq-entries 64 \
  --sq-entries 64 \
  --bp-type MyTAGE
```

## Atomic ile checkpoint alma
```bash
build/RISCV/gem5.opt -d m5out_t3 \
  configs/example/riscv/fs_linux_detailed_o3.py \
  --kernel ./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image ./boot-tests/ubuntu-riscv.raw.img \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --cpu-type=AtomicSimpleCPU \
  --checkpoint-at-end \
  --checkpoint-dir=m5out_t3 \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-type DDR4_2400_8x8 \
  --mem-size 20GB \
  --mem-channels 2 \
  --l1i_size 64KiB \
  --l1d_size 64KiB \
  --l2_size 2MiB
```

Bu komutta hem normal gem5 ciktilari hem de checkpoint ayni klasore gider:
`m5out_t2/`

## Boot Linux'u dinleme (test edildi)
```bash
# derle
cd ./util/term
make
make install

# çalıştır
./util/term/m5term localhost 3456
```

## checkpoint
```bash
CHECKPOINT=1

build/RISCV/gem5.opt -d m5out_t3 \
  configs/example/riscv/fs_linux_detailed_o3.py \
  --kernel ./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image ./boot-tests/ubuntu-riscv.raw.img \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --cpu-type=RiscvO3CPU \
  --restore-with-cpu=AtomicSimpleCPU \
  --checkpoint-dir=m5out_t3 \
  -r $CHECKPOINT \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-type DDR4_2400_8x8 \
  --mem-size 20GB \
  --mem-channels 2 \
  --l1i_size 64KiB \
  --l1d_size 64KiB \
  --l2_size 2MiB \
  --fetch-width 8 \
  --decode-width 8 \
  --rename-width 8 \
  --dispatch-width 8 \
  --issue-width 8 \
  --commit-width 8 \
  --num-rob-entries 256 \
  --num-iq-entries 128 \
  --lq-entries 64 \
  --sq-entries 64 \
  --bp-type MyTAGE
```

`--checkpoint-dir` restore edilecek checkpoint klasorunu gosterir. Yeni ciktilari
ayri bir klasore yazmak icin `-d <outdir>` kullan.

`-r $CHECKPOINT` burada checkpoint klasorunun adini degil, sira numarasini
ifade eder. Ornegin `m5out_t2` altinda tek bir `cpt.*` klasoru varsa
`CHECKPOINT=1` kullan.

Checkpoint alma ve restore komutlarinda su parametrelerin ayni kalmasi gerekir:
`--kernel`, `--disk-image`, `--mem-size`, `--mem-type`, `--mem-channels`.
Bu alanlar degisirse restore sirasinda page table / fiziksel adres uyumsuzlugu
gorup `outside of physical memory` benzeri fetch uyarilari alabilirsin.

## Checkpoint Sonrasi Aktif MyTAGE Konfigurasyonu

README'deki restore komutu sadece `--bp-type MyTAGE` geciyor. Bu nedenle
checkpoint sonrasi detayli O3 cekirdekte olusan `MyTAGE` backend'i su an
`BranchPredictor.py` icindeki default degerlerle aciliyor.

Onemli nokta: restore komutunda `--cpu-type=RiscvO3CPU` ve
`--restore-with-cpu=AtomicSimpleCPU` kullanildigi icin restore aninda aktif
detayli cekirdek `system.switch_cpus[0]` altinda olusur. Yani `MyTAGE`
parametrelerini restore senaryosunda `system.switch_cpus[0].branchPred.*`
uzerinden vermek gerekir. Direkt O3 boot senaryosunda ise hedef
`system.cpu[0].branchPred.*` olur.

Su anki restore komutunda aktif `MyTAGE` backend defaultlari:

```text
General:
  bimodalDepth                : 1024
  ghistoryLength              : 128
  pchistoryLength             : 32
  compCount                   : 4
  pcHashStartForIdx           : 2
  pcHashWidthForIdx           : 10
  pcHashStartForTag           : 10
  pcHashWidthForTag           : 12
  allocateRandomPlacement     : true
  allocateLongerThanProvider  : true
  periodicReset               : false
  periodicResetBranchPeriod   : 262144
  useLfsr                     : false    -> backend random_type = SIMRAND
  lfsrWidth                   : 16
  lfsrSeed                    : 44257
  lfsrMispredictionUpdate     : true

Tables (4):
  [Table 0] depth=1024 useful=2 ctr=3 tag=8 hist=5   pchist_start=0 pchist_width=0
  [Table 1] depth=1024 useful=2 ctr=3 tag=8 hist=15  pchist_start=0 pchist_width=0
  [Table 2] depth=1024 useful=2 ctr=3 tag=9 hist=44  pchist_start=0 pchist_width=0
  [Table 3] depth=1024 useful=2 ctr=3 tag=9 hist=130 pchist_start=0 pchist_width=0
```

`fs_linux_detailed_o3.py` tarafindan ayarlanan ortak predictor frontend
defaultlari da sunlar:

```text
branchPred.instShiftAmt            = 2
branchPred.speculativeHistUpdate   = true
branchPred.requiresBTBHit          = false
branchPred.takenOnlyHistory        = false
branchPred.btb.numEntries          = 4096
branchPred.btb.tagBits             = 16
branchPred.btb.associativity       = 1
branchPred.ras.numEntries          = 16
branchPred.indirectBranchPred      = SimpleIndirectPredictor
```

Kisacasi, senin daha once logladigin `comp_count = 12` ve 12 adet tagged table
iceren konfig su an README'deki restore komutunda aktif degil. Onu ayrica
`--param` ile gecmen gerekiyor.

## MyTAGE'i Tum Detaylariyla Configure Etme

`fs_linux_detailed_o3.py` `MyTAGE` icin ozel CLI flag tanimlamiyor; detayli
backend ayarlari gem5'in genel `--param` mekanizmasi ile veriliyor.

12 tagged TAGE component + 1 bimodal istiyorsan, restore komutuna su parametre
bloklarini ekleyebilirsin:

```bash
CHECKPOINT=1

build/RISCV/gem5.opt -d m5out_t3_restore_mytage12 \
  configs/example/riscv/fs_linux_detailed_o3.py \
  --kernel ./boot-tests/bootloader-vmlinux-5.10 \
  --disk-image ./boot-tests/ubuntu-riscv.raw.img \
  --command-line="console=ttyS0 root=/dev/vda1 ro init=/sbin/init" \
  --cpu-type=RiscvO3CPU \
  --restore-with-cpu=AtomicSimpleCPU \
  --checkpoint-dir=m5out_t3 \
  -r $CHECKPOINT \
  --cpu-clock 2GHz \
  --sys-clock 1GHz \
  --mem-type DDR4_2400_8x8 \
  --mem-size 20GB \
  --mem-channels 2 \
  --l1i_size 64KiB \
  --l1d_size 64KiB \
  --l2_size 2MiB \
  --fetch-width 8 \
  --decode-width 8 \
  --rename-width 8 \
  --dispatch-width 8 \
  --issue-width 8 \
  --commit-width 8 \
  --num-rob-entries 256 \
  --num-iq-entries 128 \
  --lq-entries 64 \
  --sq-entries 64 \
  --bp-type MyTAGE \
  --bp-inst-shift-amt 2 \
  --param 'system.switch_cpus[0].branchPred.bimodalDepth = 4096' \
  --param 'system.switch_cpus[0].branchPred.ghistoryLength = 2000' \
  --param 'system.switch_cpus[0].branchPred.pchistoryLength = 32' \
  --param 'system.switch_cpus[0].branchPred.compCount = 12' \
  --param 'system.switch_cpus[0].branchPred.pcHashStartForIdx = 6' \
  --param 'system.switch_cpus[0].branchPred.pcHashWidthForIdx = 12' \
  --param 'system.switch_cpus[0].branchPred.pcHashStartForTag = 2' \
  --param 'system.switch_cpus[0].branchPred.pcHashWidthForTag = 12' \
  --param 'system.switch_cpus[0].branchPred.allocateRandomPlacement = True' \
  --param 'system.switch_cpus[0].branchPred.allocateLongerThanProvider = True' \
  --param 'system.switch_cpus[0].branchPred.periodicReset = False' \
  --param 'system.switch_cpus[0].branchPred.periodicResetBranchPeriod = 256000' \
  --param 'system.switch_cpus[0].branchPred.useLfsr = True' \
  --param 'system.switch_cpus[0].branchPred.lfsrWidth = 8' \
  --param 'system.switch_cpus[0].branchPred.lfsrSeed = 44257' \
  --param 'system.switch_cpus[0].branchPred.lfsrMispredictionUpdate = True' \
  --param 'system.switch_cpus[0].branchPred.tableDepth = [2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048,2048]' \
  --param 'system.switch_cpus[0].branchPred.tableUsefulWidth = [2,2,2,2,2,2,2,2,2,2,2,2]' \
  --param 'system.switch_cpus[0].branchPred.tableCtrWidth = [3,3,3,3,3,3,3,3,3,3,3,3]' \
  --param 'system.switch_cpus[0].branchPred.tableTagWidth = [7,7,8,9,9,10,11,11,12,13,14,15]' \
  --param 'system.switch_cpus[0].branchPred.tableHistoryWidth = [5,9,15,25,42,70,116,193,321,535,891,1484]' \
  --param 'system.switch_cpus[0].branchPred.tablePcHistoryStart = [0,0,0,0,0,0,0,0,0,0,0,0]' \
  --param 'system.switch_cpus[0].branchPred.tablePcHistoryWidth = [3,3,3,5,5,5,8,8,8,16,16,16]'
```

Bu blok tam olarak senin daha once logladigin su yapıyı kurar:

```text
1 bimodal + 12 tagged TAGE component
```

Eger ayni konfigi checkpoint'siz, dogrudan O3 boot akisinda kullanacaksan
yalnizca `system.switch_cpus[0]` yerine `system.cpu[0]` yazman yeterli.

## Boyut Notu

Sabit `depth = 2048` kullandigin icin tagged table'larin mantiksal boyutu
esit degil; `tag_width` arttikca tablo buyuyor. Kabaca tagged table mantiksal
bit butcesi:

```text
table_bits ~= depth * (useful_width + ctr_width + tag_width)
```

Bu nedenle yukaridaki ornekte table boyutlari yaklasik olarak 3 KiB ile 5 KiB
arasinda degisir. Tam olarak "her tagged component 4 KiB olsun" istiyorsan
`tag_width` ile birlikte `depth` de tablo bazinda yeniden ayarlanmalidir.
