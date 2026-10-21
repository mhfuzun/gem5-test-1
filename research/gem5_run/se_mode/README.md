# RISC-V SE + Atomic + SimPoint + O3

Bu dizin, tek iş parçacıklı RISC-V SE workload'ları için üç aşamalı ve
manifest kontrollü bir SimPoint akışı sağlar:

1. `01_profile.py`: Atomic BBV profili ve SimPoint clustering.
2. `02_checkpoint.py`: bütün seçili noktalar için tek Atomic koşuda,
   SimPoint başlangıcından warmup kadar önce checkpoint.
3. `03_run_o3.py`: checkpoint restore, O3 warmup, stats reset ve committed
   instruction tabanlı ölçüm.

`common.py`, `spec_workload.py` ve gem5 tarafından çalıştırılan
`gem5_config.py` ortak altyapıdır. `summarize.py` tamamlanmış SimPoint
koşularından ağırlıklı CPI ve tahminî IPC üretir.

## Ön koşullar

- RISC-V destekli `gem5.fast` veya `gem5.opt`.
- SimPoint 3.2 executable'ı.
- Tek iş parçacıklı bir RISC-V executable ve temiz SPEC run dizini.
- Dinamik bağlı RISC-V programlarda, guest dynamic linker ve kütüphaneleri
  içeren bir host sysroot.

gem5 derlenecekse 12 çekirdek kullanın:

```bash
scons build/RISCV/gem5.opt -j 12
```

## 1. Workload manifesti

SPEC run dizini tek başına girdi değildir. Önce `speccmds.cmd` içindeki her
komut için ayrı manifest oluşturulur:

```bash
python3 research/gem5_run/se_mode/spec_workload.py from-speccmds \
  --run-dir research/scripts/spec-runs/train/531.deepsjeng_r/run_base_train_mytest.0000 \
  --benchmark 531.deepsjeng_r \
  --output-dir research/gem5_run/se_mode/workloads
```

Dinamik binary için sysroot verin. `--sysroot`, hem ELF interpreter kökünü
ayarlar hem de guest `/` yollarını bu dizine yönlendirir:

```bash
python3 research/gem5_run/se_mode/spec_workload.py from-speccmds \
  --run-dir /path/to/531.deepsjeng_r/run_base_train_config.0000 \
  --benchmark 531.deepsjeng_r \
  --sysroot /opt/riscv/sysroot \
  --output-dir research/gem5_run/se_mode/workloads
```

`502.gcc_r` gibi birden fazla komut içeren run dizinlerinde birden fazla JSON
üretilir. Komutlar birleştirilmez. Tek birini seçmek için `--command-index N`
kullanılabilir. Varsayılan environment yalnızca `LC_ALL=C`,
`OMP_NUM_THREADS=1` ve `OMP_THREAD_LIMIT=1` içerir; gerekli ek değişkenler
`--env NAME=VALUE` ile açıkça eklenmelidir.

Manifest şu sözleşmeyi taşır:

```json
{
  "schema_version": 1,
  "workload_id": "531.deepsjeng_r.cmd000.deepsjeng_r",
  "source_run_directory": "/absolute/pristine/run/directory",
  "binary": "deepsjeng_r_base.mytest",
  "guest_binary": "/work/deepsjeng_r_base.mytest",
  "arguments": ["train.txt"],
  "stdin": null,
  "stdout": "train.out",
  "stderr": "train.err",
  "environment": {"LC_ALL": "C"},
  "guest_working_directory": "/work",
  "interpreter_root": "/path/to/riscv/sysroot",
  "path_redirects": [{"guest": "/", "host": "/path/to/riscv/sysroot"}]
}
```

Profil ve checkpoint aşamaları `source_run_directory` içeriğini kendi
`workdir/` dizinine kopyalar. Her checkpoint anında o noktadaki değiştirilebilir
dosyaların ayrıca `filesystem_snapshots/<checkpoint_id>/` kopyası alınır. Step
C kendi `workdir/` dizinini ilgili snapshot'tan oluşturur. Simüle edilen süreç
daima `/work` görür; gem5 path redirection bunu ilgili aşamanın host kopyasına
eşler. Böylece aşamalar değiştirilebilir dosyaları paylaşmaz ve restore,
checkpoint öncesinde üretilmiş ara dosyaları görür. Kaynak ağacın SHA-256 özeti
manifest sözleşmesine girer; sonradan değişirse sonraki aşama durur.

## 2. Step A — BBV ve clustering

```bash
python3 research/gem5_run/se_mode/01_profile.py \
  --gem5 build/RISCV/gem5.fast \
  --workload research/gem5_run/se_mode/workloads/531.deepsjeng_r.cmd000.deepsjeng_r.json \
  --simpoint-bin /path/to/SimPoint/bin/simpoint \
  --interval 200M \
  --max-k 10 \
  --seed 1
```

Normal kullanımda workload tamamen bitmelidir. `--max-insts` yalnızca kısa
test içindir ve `--allow-partial-profile` olmadan reddedilir. Kısmi profil
manifestte temsilî olmayan profil olarak işaretlenir; akademik sonuçta
kullanılmamalıdır.

Başlıca çıktılar:

- `simpoint.bb.gz`
- `simpoints.txt`
- `weights.txt`
- `profile_manifest.json`
- `profile.log` ve `clustering.log`

## 3. Step B — checkpoint seti

```bash
python3 research/gem5_run/se_mode/02_checkpoint.py \
  --gem5 build/RISCV/gem5.fast \
  --profile-manifest results/531.deepsjeng_r.cmd000.deepsjeng_r/profile/profile_manifest.json \
  --warmup 50M
```

Her nokta için:

```text
S = interval_index * BBV_interval
C = max(0, S - requested_warmup)
actual_warmup = S - C
```

hesaplanır. Noktalar instruction sırasına göre programı yeniden başlatmadan
tek Atomic koşuda yazılır. Aynı `C` değerine düşen erken SimPoint'ler ayrı
checkpoint kimlikleriyle aynı durumdan kaydedilir. Her checkpoint dizininde
`checkpoint_manifest.json`, üst dizinde `checkpoint_set_manifest.json`
bulunur. Checkpoint ile aynı anda alınan çalışma dizini snapshot'ı da manifest
sözleşmesine dahildir. Snapshot'lar bağımsız fiziksel kopyalardır; büyük SPEC
run dizinlerinde checkpoint belleğine ek olarak belirgin disk alanı tüketir.

50M/100M warmup duyarlılık deneyi için iki ayrı checkpoint seti üretin;
Step C warmup override kabul etmez. Böylece restore edilen noktayla warmup
değeri ayrı ayrı değiştirilemez.

## 4. Step C — O3 ölçümü

```bash
python3 research/gem5_run/se_mode/03_run_o3.py \
  --gem5 build/RISCV/gem5.opt \
  --checkpoint-manifest /path/to/checkpoint_manifest.json \
  --l1i-size 32KiB \
  --l1d-size 32KiB \
  --l2-size 256KiB \
  --branch-predictor TAGE_SC_L_64KB \
  --cpu-param fetchWidth=8 \
  --cpu-param decodeWidth=8 \
  --bp-param speculativeHistUpdate=true
```

Ölçüm uzunluğu verilmezse BBV interval kullanılır. Farklı bir pencere açıkça
`--measurement 100M` ile seçilebilir ve manifestte
`measurement_matches_bbv_interval=false` olarak görünür.

Repo içindeki özel frontend için Python modülü de kullanılabilir:

```python
def configure(board, processor, cache_hierarchy, config):
    core = processor.get_cores()[0].core
    # Derlenmiş SimObject parametrelerini burada ayarla.
```

Modülü `--configuration-module /absolute/configure_frontend.py` ile verin.
Dosya SHA-256 özeti konfigürasyon kimliğine dahil edilir.

Çıktılar arasında `stats.txt`, `config.json`, `run_manifest.json`,
`runtime_status.json`, workload `workdir/` ve `run.log` vardır. Program warmup
veya measurement bitmeden çıkarsa koşu `incomplete` olur ve komut başarısız
döner.

### Geniş O3 commit ve instruction sınırı

gem5'in `MAX_INSTS` olayı committed instruction sayacıyla kurulur. Geniş O3,
olayın yükseldiği çevrimde birkaç ek instruction retire edebilir. Bu nedenle
örneğin 200M istek, `commitWidth - 1` mertebesinde küçük bir overshoot ile
sonuçlanabilir. Sistem gerçek değerleri
`observed_*_instructions`, `*_overshoot_instructions` ve
`measurement_exact` alanlarında kaydeder; `stats.txt` de gerçek ölçüm
penceresini içerir. Cycle/tick tabanlı sonlandırma kullanılmaz.

## 5. Ağırlıklı sonuç

Bütün checkpoint'ler aynı O3 konfigürasyonuyla tamamlandıktan sonra:

```bash
python3 research/gem5_run/se_mode/summarize.py \
  --runs-root results/531.deepsjeng_r.cmd000.deepsjeng_r/o3_runs
```

Araç eksik/tekrarlı SimPoint'i, farklı profil veya karışık konfigürasyonu
reddeder ve şunları hesaplar:

```text
CPI_weighted = sum(weight_i * CPI_i)
IPC_estimated = 1 / CPI_weighted
```

Çıktı `weighted_summary.json` yalnızca profillenen workload için bir SimPoint
tahminidir; resmî SPEC skoru değildir.

## Sonuç dizini

```text
results/<workload_id>/
  profile/
    workdir/
    simpoint.bb.gz
    simpoints.txt
    weights.txt
    profile_manifest.json
  checkpoints/warmup_<N>/
    workdir/
    filesystem_snapshots/<checkpoint_id>/
    checkpoint_set_manifest.json
    checkpoints/<checkpoint_id>/
      m5.cpt
      checkpoint_manifest.json
  o3_runs/<checkpoint_id>/<configuration_id>/
    workdir/
    stats.txt
    config.json
    run_manifest.json
```

## Kısa kabul testi

Repo içindeki statik RISC-V hello binary ile uçtan uca smoke testi yapılabilir:

```bash
python3 research/gem5_run/se_mode/01_profile.py \
  --gem5 build/RISCV/gem5.fast \
  --workload research/gem5_run/se_mode/examples/hello_smoke.json \
  --simpoint-bin /path/to/simpoint --results-root /tmp/se-mode-smoke \
  --interval 100 --max-k 2 --force

python3 research/gem5_run/se_mode/02_checkpoint.py \
  --gem5 build/RISCV/gem5.fast \
  --profile-manifest /tmp/se-mode-smoke/riscv-hello-smoke/profile/profile_manifest.json \
  --warmup 50 --force
```

Son komutta üretilen checkpoint manifestlerinden birini `03_run_o3.py` ile
100 instruction ölçerek Atomic → RISC-V O3 restore kabul testi yapılır.

Python sözleşme testleri:

```bash
python3 -m unittest discover -s research/gem5_run/se_mode/tests -v
```

## Bilinen sınırlar

- Akış tek core/tek thread içindir. İlk doğrulamalar `_r` SPECint train
  workload'larıyla yapılmalıdır.
- gem5 SE checkpoint'leri pipe, socket ve emulated device FD'lerini güvenli
  restore etmez. Argümanla açılan sıradan dosyaları kullanan SPEC komutları
  hedeflenmiştir.
- Bu gem5 revizyonu SE checkpoint restore sırasında standart stdin FD
  offset'ini geri yüklemiyor. Bu yüzden `stdin` alanı dolu workload'lar Step
  B'de açık hata ile reddedilir; aksi halde restore stdin'i baştan okuyarak
  instruction stream'i sessizce kaydırır. Dosya adını argüman olarak alan
  `531.deepsjeng_r`/`541.leela_r` gibi ilk workload'lar bu sınıra takılmaz.
- Checkpoint belleği, ISA, workload fingerprint'i ve gem5 commit'i doğrulanır.
  Restore sırasında bellek boyutu değiştirilemez; cache hiyerarşisi ve CPU
  modeli değişebilir.
- Kaynak SPEC run dizini pristine tutulmalıdır. Yeniden koşuda sonuç dizinini
  silmek için yalnızca hedef aşamaya açıkça `--force` verilir.
