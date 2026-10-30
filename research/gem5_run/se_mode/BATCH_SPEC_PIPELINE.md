# Paralel SPEC SimPoint pipeline raporu

Step B sonrasında mevcut checkpoint'leri tam workload ID'leriyle (örneğin
`502.gcc_r.cmd000.cpugcc_r`) ve `--run-id warmup_50000000` seçimiyle paralel
O3 çalıştırmak için `batch_o3_run.py`; tek komutu başlangıçtan
bitişe tamamen O3 ile ölçmek için `run_o3_full.py` eklendi. Kullanım,
dosyalama, loglar, cluster/weight/CPI hesabı ve raporlar
[`O3_BATCH_PIPELINE.md`](O3_BATCH_PIPELINE.md) içinde açıklanmıştır.
Bu araçlar mevcut Step A/Step B sonuçlarını korur ve her çağrıda aynı
workload ağacı altında benzersiz yeni sonuç dizinleri oluşturur.

`run_spec_batch.py`, doğrudan verilen SPEC benchmark listesindeki bütün
bağımsız `speccmds.cmd` komutları için şu zinciri yürütür:

```text
workload manifesti -> Step A (BBV + clustering) -> Step B (checkpoint seti)
```

Bir workload içinde Step B kendi Step A çıktısını bekler. Farklı workload
zincirleri `--jobs` sınırına kadar paralel yürür. Örneğin `--jobs 4`, aynı anda
en fazla dört gem5 sürecine izin verir. SimPoint clustering sırasında ilgili
worker meşgul kalacağı için her anda tam dört gem5 süreci bulunması garanti
edilmez.

## VDS dizin sözleşmesi

Script bundle kullanmaz. Varsayılan yollar repo köküne göre çözülür:

```text
build/RISCV/gem5.fast
build/RISCV/gem5.opt
research-out/spec-runs/
research-result/batch_results/
```

gem5 henüz derlenmediyse 12 build job kullanın:

```bash
scons build/RISCV/gem5.fast -j 12
scons build/RISCV/gem5.opt -j 12
```

`--gem5-type fast` varsayılandır. `--gem5-type opt`, repo içindeki
`build/RISCV/gem5.opt` binary'sini seçer. İstisnai bir binary yolu için
`--gem5 /absolute/path/to/gem5.fast` verilebilir; bu seçenek
`--gem5-type` seçimini geçersiz kılar.

## Önerilen kullanım: sadeleştirilmiş train

`--train-dir` doğrudan `manifest.tsv` içeren hazırlanmış input dizinini alır.
Bu nedenle `train_reduced` gibi `test/train/ref` dışındaki bir ad kullanılabilir:

```bash
python3 research/gem5_run/se_mode/run_spec_batch.py \
  --gem5-type fast \
  --train-dir ./research-out/spec-runs/train_reduced \
  --benchmarks 502.gcc_r 531.deepsjeng_r 541.leela_r 557.xz_r \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 4 \
  --interval 200M \
  --max-k 10 \
  --seed 1 \
  --warmup 50M
```

Bu komutun varsayılan çıktı dizini şudur:

```text
research-result/batch_results/train_reduced/
```

Tam train ağacını seçmek için `--train-dir` yolunu
`./research-out/spec-runs/train` yapın. Eski boyut seçimi de korunur:

```bash
--spec-runs-root ./research-out/spec-runs --size train
```

`--spec-runs-root` verilmezse repo içindeki `research-out/spec-runs`
varsayılır. Dolayısıyla standart train için yalnızca `--size train` yeterlidir.
`--train-dir` ve `--spec-runs-root` aynı komutta birlikte kullanılamaz.

Başka bir sonuç alt dizini istenirse `--output-root` verilebilir, ancak veri
dağılmasını önlemek için bu yol repo içindeki `research-result` altında olmak
zorundadır:

```bash
--output-root ./research-result/experiments/train_reduced_fast
```

## SPEC input düzeni

Seçilen doğrudan input dizini şu biçimde olmalıdır:

```text
train_reduced/
  manifest.tsv
  502.gcc_r/<run-directory>/speccmds.cmd
  531.deepsjeng_r/<run-directory>/speccmds.cmd
  ...
```

`manifest.tsv`, benchmark ile onun altındaki run dizinini eşler:

```text
benchmark<TAB>run_dir
502.gcc_r<TAB>run_base_train_mytest.0000
```

Birden fazla SPEC komutu olan `502.gcc_r` gibi benchmark'larda bütün komutlar
ayrı workload olarak çalışır. Yalnızca tek komut istenirse:

```bash
--command-index 502.gcc_r=0
```

İstisnai bir run dizini `--run-dir BENCHMARK=/absolute/path` ile override
edilebilir. `--run-dir` ve `--command-index` benchmark başına tekrarlanabilir.

## Çıktılar

```text
research-result/batch_results/<input-name>/
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

Sonuçların tamamını VDS'den yerel bilgisayara almak için örneğin:

```bash
rsync -a --info=progress2 \
  user@vds:/absolute/path/to/gem5-test-1/research-result/ \
  ./research-result/
```

## Hata yalıtımı ve resume

Bir workload'un manifest hazırlığı, Step A veya Step B aşaması başarısız
olursa yalnızca o workload zinciri durur. Çalışmakta olan ve kuyrukta bekleyen
diğer workload'lar devam eder. Batch sonunda en az bir hata varsa dönüş kodu
`1`, rapor durumu `failed` olur. gem5 veya SimPoint binary'sinin bulunamaması
gibi bütün workload'ları etkileyen global doğrulama hataları ise hiçbir worker
başlatılmadan dönüş kodu `2` ile sonlanır.

Resume varsayılan olarak açıktır. Tamamlanmış bir aşama ancak workload
fingerprint'i, gem5 SHA-256 değeri, BBV/clustering parametreleri, sistem
ayarları ve warmup sözleşmesi aynıysa yeniden kullanılır. Uyuşmazlık sessizce
kabul edilmez; ilgili üretilmiş aşamayı yenilemek için `--force` verilmelidir.

Planı simülasyon başlatmadan doğrulamak için ana komuta `--dry-run` ekleyin.
Bu doğrulama seçilen benchmark'ların manifest ve `speccmds.cmd` dosyalarını,
gem5 binary'sini, SimPoint binary'sini, sysroot'u ve üretilecek workload
yollarını kontrol eder.

## Kısa altyapı testi

Bu mod yalnızca smoke test içindir; akademik sonuç üretmez:

```bash
python3 research/gem5_run/se_mode/run_spec_batch.py \
  --gem5-type fast \
  --train-dir ./research-out/spec-runs/train_reduced \
  --benchmarks 502.gcc_r \
  --command-index 502.gcc_r=0 \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 1 \
  --interval 100K \
  --max-k 2 \
  --max-insts 1M \
  --allow-partial-profile \
  --output-root ./research-result/smoke/train_reduced
```

## Operasyonel notlar

- `--jobs` aralığı `1..12` ile sınırlıdır.
- Her gem5 süreci yaklaşık birkaç GiB host belleği ve checkpoint disk alanı
  kullanabilir. `--jobs 4` seçmeden önce VDS RAM ve disk kapasitesi
  doğrulanmalıdır.
- Step B, stdin yönlendirmeli workload'ları mevcut SE checkpoint FD-offset
  sınırlaması nedeniyle desteklemez. Böyle bir workload diğerlerini
  durdurmadan hata olarak raporlanır.
- Tam Step A program sonuna kadar çalışır. Sadeleştirilmiş train girdisinde
  dahi Step A ve Step B uzun sürebilir.

## SYSROOT sorunu

#### 1. Mevcut sysroot'u kontrol et
```
SYSROOT=/opt/riscv/sysroot

ls -ld "$SYSROOT"

ls -l "$SYSROOT/lib/ld-linux-riscv64-lp64d.so.1"
ls -l "$SYSROOT/lib/libc.so.6"
ls -l "$SYSROOT/lib/libstdc++.so.6"
```

#### 2. Eksikse RISC-V kütüphanelerini kur
Ubuntu tabanlı Linux Mint'te:
```
sudo apt update

sudo apt install -y \
    libc6-riscv64-cross \
    libstdc++6-riscv64-cross \
    libgcc-s1-riscv64-cross
```

Bu paketler RISC-V için glibc, C++ standart kütüphanesi ve GCC runtime bileşenlerini sağlar.

Kurulumu doğrula:
```
ls /usr/riscv64-linux-gnu/lib/ld-linux-riscv64-lp64d.so.1
```

#### 3. /opt/riscv/sysroot oluştur
Mevcut sysroot dosyalarının üzerine yazmadan, boş bir sysroot için:
```
sudo mkdir -p /opt/riscv/sysroot

sudo ln -s \
    /usr/riscv64-linux-gnu/lib \
    /opt/riscv/sysroot/lib
```

## Yanlışlıkla Ctrl+C ve SSH kopmasına karşı koruma

Uzun koşuyu terminalden ayrı bir session'da arka planda başlat. Böylece
terminaldeki Ctrl+C simülasyonlara gitmez; yalnızca log izleyicisini
durdurur. `setsid` yeni session oluşturur, `nohup` terminal kapanışındaki
SIGHUP sinyaline karşı korur. `tmux` içinde foreground çalıştırmak tek
başına Ctrl+C koruması sağlamaz.

Aşağıdaki örnek önceki kesilmiş sonuçları korumak için yeni bir çıktı
dizini kullanır. Yeniden başlatmadan önce VDS'de eski batch veya sahipsiz
gem5 süreçleri kalmış mı kontrol et:

```bash
ps -C gem5.fast,gem5.opt -o pid,ppid,etime,args
pgrep -af '[r]un_spec_batch.py|[0]1_profile.py|[0]2_checkpoint.py'
```

Repo kökünde, kullanılmamış bir `--output-root` seçerek başlat:

```bash
mkdir -p research-result/logs

nohup setsid python3 -u research/gem5_run/se_mode/run_spec_batch.py \
  --gem5-type fast \
  --train-dir ./research-out/spec-runs/train_reduced \
  --benchmarks 502.gcc_r 531.deepsjeng_r 541.leela_r 557.xz_r \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 4 \
  --interval 200M \
  --max-k 10 \
  --seed 1 \
  --warmup 50M \
  --output-root ./research-result/batch_results/train_reduced_retry01 \
  </dev/null >>research-result/logs/train_reduced_retry01.log 2>&1 &
```

Sondaki `&` gereklidir. `tee` pipeline'ı yerine bütün çıktılar doğrudan
loga yönlendirilir. Logu izlemek için:

```bash
tail -f research-result/logs/train_reduced_retry01.log
```

Burada Ctrl+C yalnızca `tail` sürecini kapatır. SSH'dan çıkıp yeniden
bağlandıktan sonra aynı komutla logu izleyebilirsin. Sonuç raporunun
`status` ve `finished_at` alanlarını da kontrol et; arka planda başlatılması
işin hatasız tamamlanacağını garanti etmez. Bu yöntem terminal kaynaklı
sinyalleri ayırır; sunucunun yeniden başlamasından sonra otomatik devam
sağlamaz.

Kesilen Step A'nın tamamlanmamış BBV dosyasından otomatik instruction
seviyesinde devam desteği yoktur. Tamamlanmış ve uyumlu aşamalar aynı sonuç
kökünde resume ile kullanılabilir; kesilmiş aşamalar ise kendiliğinden
yeniden yazılmaz. Yukarıdaki yeni sonuç kökü bütün işleri baştan çalıştırır
ve eski dosyaları korur. Eski rapordaki `running` değeri tek başına sürecin
hâlâ canlı olduğunu göstermez.

Foreground `tmux` kullanımını tercih edersen ilgili pane'de koşudan önce
`stty intr undef` çalıştırarak terminalin Ctrl+C -> SIGINT dönüşümünü
kapatabilirsin. Koşu bittikten sonra `stty intr '^C'` ile geri aç. Yalnızca
Python'da KeyboardInterrupt yakalamak yeterli değildir; terminal SIGINT'i
aynı foreground process grubundaki gem5 süreçlerine de gönderir.

Ctrl+C normalde SIGINT'tir (`-2`). Önceki rapordaki `-10`, SIGUSR1 ile
sonlandırılmadır; ayrı bir sinyal kaynağıdır. Stats almak için sinyal
gönderirken `ps -C gem5.fast -o pid,ppid,etime,args` çıktısındaki gerçek
gem5 PID'sini kullan. `pgrep -f gem5.fast` Python wrapper'ları da eşleyebilir.

Kaynak: [setsid kullanım açıklaması](https://man7.org/linux/man-pages/man1/setsid.1.html).

### VDS SSH Broken-Pipe Sorunu (tmux alternatifi)
#### tmux kur ve başlat
```
sudo apt install tmux -y
cd ~/gem5-test-1
tmux new -s gem5-spec
```

#### Batch'i yeniden başlat
```
mkdir -p research-result/logs
set -o pipefail
```

```
python3 -u research/gem5_run/se_mode/run_spec_batch.py \
  --gem5-type fast \
  --train-dir ./research-out/spec-runs/train_reduced \
  --benchmarks 502.gcc_r 531.deepsjeng_r 541.leela_r 557.xz_r \
  --sysroot /opt/riscv/sysroot \
  --simpoint-bin ../SimPoint/bin/simpoint \
  --jobs 4 \
  --interval 200M \
  --max-k 10 \
  --seed 1 \
  --warmup 50M \
  2>&1 | tee "research-result/logs/spec_$(date +%Y%m%d_%H%M%S).log"
```

#### mux içinde ayrılma
```
tmux'tan ayrıl
Komut çalışırken:
Ctrl + B --> D
Böylece normal SSH terminaline dönersin; simülasyonlar sunucuda çalışmaya devam eder.
```

#### termianle geri dönme
```
tmux attach -t gem5-spec
```

Son oluşturulan batch logunu izleyebilirsin:
```
tail -f "$(ls -t ~/gem5-test-1/research-result/logs/spec_*.log | head -1)"
```
