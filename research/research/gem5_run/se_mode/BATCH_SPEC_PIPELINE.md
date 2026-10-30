# Paralel SPEC SimPoint pipeline raporu

`run_spec_batch.py`, doğrudan verilen SPEC benchmark listesindeki bütün bağımsız
`speccmds.cmd` komutları için şu zinciri yürütür:

```text
workload manifesti -> Step A (BBV + clustering) -> Step B (checkpoint seti)
```

Bir workload içinde Step B daima kendi Step A çıktısını bekler. Farklı
workload zincirleri `--jobs` sınırına kadar paralel yürür. Dolayısıyla
`--jobs 4`, aynı anda en fazla dört gem5 sürecine izin verir. SimPoint
clustering sırasında ilgili worker meşgul kalacağı için her anda tam dört
gem5 süreci bulunması garanti edilmez.

## Bundle uyumluluğu

Taşınabilir bundle içindeki `bin/gem5.fast`, host sistemde doğrudan
çalıştırılmamalıdır. Gerekli ELF loader, paylaşımlı kütüphaneler ve Python
ortamı bundle içindeki `run-gem5 fast` tarafından kurulur. Batch scriptine
yine bundle veya `gem5.fast` yolu verilir; launcher otomatik seçilir.

Kabul edilen biçimler:

```text
--bundle ./bundle-out
--bundle ./bundle-out/gem5-RISCV-<rev>-rhel-8.10
--bundle ./bundle-out/gem5-RISCV-<rev>-rhel-8.10/bin/gem5.fast
```

`bundle-out` altında birden fazla `gem5.fast` paketi bulunursa belirsiz seçim
yapılmaz; çıkarılmış bundle dizini açıkça verilmelidir. Sadece `.tar.gz`
dosyasının bulunması yeterli değildir, paket önce çıkarılmalıdır.

Dinamik RISC-V SPEC binary'leri için kullanılan bundle, ELF interpreter
aralığını doğru ayıran güncel gem5 kaynağından yeniden üretilmiş olmalıdır.
Aksi halde eski binary `0x4000000000000000` adresinde VMA çakışmasıyla
durabilir.

### 23 Ekim 2026 doğrulama sonucu

Çalışma ağacındaki mevcut
`gem5-RISCV-7e8c411d8df1-rhel-8.10` bundle'ı launcher üzerinden başarıyla
başlatıldı; ancak bu artifact ELF-loader düzeltmesinden önce üretildiği için
dinamik `502.gcc_r` smoke testinde yukarıdaki VMA paniğini verdi. Aynı batch
akışı güncel yerel `gem5.fast` ile 1M instruction Step A, clustering ve iki
checkpoint içeren Step B testini tamamladı. Portable SPEC koşusundan önce yeni
bundle 12 build job ile üretilmelidir:

```bash
./bundle-out/build-gem5-portable-bundle.sh \
  --source . \
  --target-report ./bundle-out/redhat-environment-20261019T060740Z.txt \
  --cache-dir /home/tarator/github_repos/build/build-cache/gem5-rhel \
  --jobs 12
```

Yeni arşiv çıkarıldıktan sonra `--bundle` yeni çıkarılmış dizine verilmelidir.

## Temel kullanım

Batch scripti tam SPEC CPU kurulumunu kullanmaz. Girdi olarak
`research/scripts/spec-runs` tarafından hazırlanmış taşınabilir export ağacını
alır:

```text
spec-runs/
  test/manifest.tsv
  train/manifest.tsv
  ref/manifest.tsv
  train/<benchmark>/<run-directory>/...
```

Başka bilgisayarda yalnızca o makinedeki `spec-runs` kökü verilmelidir.
Workload manifestleri hedef makinede yeniden üretildiği için eski bilgisayarın
mutlak yolu sonuç sözleşmesine taşınmaz:

```bash
python3 research/gem5_run/se_mode/run_spec_batch.py \
  --bundle /path/to/extracted-gem5-bundle \
  --spec-runs-root /path/on/this-machine/spec-runs \
  --size train \
  --benchmarks 502.gcc_r 531.deepsjeng_r 541.leela_r \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 4 \
  --interval 200M \
  --max-k 10 \
  --seed 1 \
  --warmup 50M \
  --output-root ./research/gem5_run/se_mode/batch_results/train
```

Script, seçilen `--size` altındaki `manifest.tsv` dosyasını okuyup her
benchmark'ın run dizinini deterministik olarak bulur. `--size`, `test`,
`train` veya `ref` olabilir ve varsayılanı `train` değeridir. İstisnai olarak
catalog dışındaki bir dizin açıkça seçilebilir:

```bash
--run-dir 502.gcc_r=/absolute/run_base_train_mytest.0000
```

Birden fazla SPEC komutu olan `502.gcc_r` gibi benchmark'larda bütün komutlar
ayrı workload olarak çalışır. Yalnızca tek komut istenirse:

```bash
--command-index 502.gcc_r=0
```

`--run-dir` ve `--command-index` seçenekleri benchmark başına tekrarlanabilir.

## Çıktılar

```text
<output-root>/
  batch_report.json
  batch_report.md
  workloads/<benchmark>/*.json
  driver_logs/<workload_id>/
    01_profile.driver.log
    02_checkpoint.driver.log
  results/<workload_id>/
    profile/
      profile_manifest.json
      simpoint.bb.gz
      simpoints.txt
      weights.txt
    checkpoints/warmup_<N>/
      checkpoint_set_manifest.json
      checkpoints/*/checkpoint_manifest.json
```

`batch_report.md` her workload'un Step A/Step B durumunu, checkpoint sayısını,
süreyi ve varsa hatayı içerir. `batch_report.json` aynı bilgilerin makine
tarafından işlenebilir biçimidir. Rapor her workload tamamlandığında atomik
olarak güncellenir.

## Resume ve tekrar çalıştırma

Resume varsayılan olarak açıktır. Tamamlanmış bir aşama ancak aşağıdaki
sözleşme alanları aynıysa yeniden kullanılır:

- workload fingerprint;
- gem5 binary SHA-256;
- BBV interval, clustering seed ve `max-k`;
- clock ve bellek boyutu;
- checkpoint warmup değeri;
- checkpoint dosyalarının tamamlanma durumu.

Uyuşmazlık sessizce kabul edilmez. İlgili üretilmiş profile/checkpoint
dizinlerini yeniden oluşturmak için açıkça `--force` verilir. Kaynak SPEC run
dizini silinmez veya değiştirilmez. Workload manifestleri ucuz ve türetilmiş
dosyalar olduğu için her batch başlangıcında aynı girdilerden yenilenir.

Planı simülasyon başlatmadan doğrulamak için:

```bash
python3 research/gem5_run/se_mode/run_spec_batch.py \
  --bundle /path/to/extracted-gem5-bundle \
  --spec-runs-root /path/on/this-machine/spec-runs \
  --size train \
  --benchmarks 502.gcc_r 531.deepsjeng_r \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 4 \
  --dry-run
```

## Kısa doğrulama modu

Bu mod yalnızca altyapı smoke testi içindir; akademik sonuç üretmez:

```bash
python3 research/gem5_run/se_mode/run_spec_batch.py \
  --bundle /path/to/extracted-gem5-bundle \
  --spec-runs-root /path/on/this-machine/spec-runs \
  --size train \
  --benchmarks 502.gcc_r \
  --command-index 502.gcc_r=0 \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 1 \
  --interval 100K \
  --max-k 2 \
  --max-insts 1M \
  --allow-partial-profile \
  --output-root /tmp/spec-batch-smoke
```

## Operasyonel notlar

- `--jobs` aralığı `1..12` ile sınırlıdır.
- Her gem5 süreci kendi 2 GiB simüle edilen belleğini ve yaklaşık birkaç GiB
  host belleğini kullanabilir. `--jobs 4` için yeterli host RAM ve checkpoint
  disk alanı önceden doğrulanmalıdır.
- Bir workload'un manifest hazırlığı, Step A veya Step B aşaması başarısız
  olursa yalnızca o zincir durur; kuyruğa alınmış ve çalışmakta olan diğer
  bağımsız workload'lar devam eder. Batch sonunda dönüş kodu `1` ve rapor
  durumu `failed` olur. Geçersiz bundle/gem5/SimPoint executable gibi bütün
  workload'ları imkânsız kılan global preflight hataları batch'i başlatmaz.
- Step B, stdin yönlendirmeli workload'ları mevcut SE checkpoint FD-offset
  sınırlaması nedeniyle desteklemez. Batch preflight bu workload'ları Step A
  başlamadan reddederek uzun fakat kullanılamaz bir profil koşusunu önler.
  Böyle benchmark'lar için checkpoint restore sözleşmesi ayrıca çözülmeden
  sonuç üretilmemelidir.
- Tam Step A program sonuna kadar çalışır. `train` girdisinde dahi bu aşama ve
  Step B uzun sürebilir.
