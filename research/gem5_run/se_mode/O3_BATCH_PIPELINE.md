# SPEC checkpoint batch ve tam O3 koşuları

Bu araçlar mevcut Step A/Step B çıktılarından çalışır. Step A veya Step B'yi
tekrarlamaz, mevcut sonuçları temizlemez, çalışan gem5 süreçlerini aramaz ya
da durdurmaz. Betikler `research/gem5_run/se_mode/`, bütün yeni çıktılar
mevcut `research-result/` ağacındadır. Komutları repo kökünden çalıştırın.

## 1. Workload ve Step B koşusunu seçme

Test seçimi raporlarda kayıtlı tam workload ID ile yapılır; örneğin
`502.gcc_r.cmd000.cpugcc_r`. Step B koşu ID'si ise mevcut checkpoint seti
dizininin adıdır; örneğin `warmup_50000000`. Bu ID seçilen bütün workload'lar
için ortak kullanılır.

Önce planı doğrulayın. `--dry-run` gem5'i (`--build-info` dahil) çalıştırmaz
ve sonuç dizini oluşturmaz; yalnızca manifestleri, snapshot/binary
varlığını, checkpoint sözleşmelerini ve ağırlıkları kontrol eder. Binary
SHA-256 hesabı için executable dosyasını okur.

```bash
python3 research/gem5_run/se_mode/batch_o3_run.py \
  --batch-root research-result/batch_results/train_reduced_retry01 \
  --gem5 experimental/build/RISCV/gem5.fast \
  --tests 502.gcc_r.cmd002.cpugcc_r \
  --run-id warmup_50000000 --jobs 7 \
  --label baseline --purpose "GCC komut 2 checkpoint O3 baseline" \
  --dry-run
```

Gerçek koşu için aynı komuttan `--dry-run` seçeneğini kaldırın. Mevcut
`502.gcc_r.cmd002.cpugcc_r` setinde dört checkpoint vardır; dolayısıyla
`--jobs 7` ile bu testte en fazla dört gem5 süreci çalışır.
`--run-id warmup_50000000`, Step B'nin oluşturduğu 50M warmup setini seçer.
Ölçüm her checkpoint'in kendi BBV interval'idir (mevcut sette 200M).
Başka set için önce Step B'de uygun çıktıyı oluşturun, sonra örneğin
`--run-id warmup_100000000` seçin; bu seçim checkpoint'in ölçüm başlangıcını
kaydırmaz. Step B koşu ID'si açıkça verilmelidir.

Binary varsayılanı `experimental/build/RISCV/gem5.fast` olarak ayarlıdır.
Başka bir build için `--gem5 build/RISCV/gem5.opt` gibi açık yol verin.
Bir build'in yalnızca dosya adını değiştirmek veya yeniden derlemek CPU
modelini seçmez: bu akış CPU'yu O3 olarak kurar.

Birden çok testin **her checkpoint'i ayrı iş** olarak ortak kuyruğa girer:

```bash
python3 research/gem5_run/se_mode/batch_o3_run.py \
  --batch-root research-result/batch_results/train_reduced_retry01 \
  --tests 502.gcc_r.cmd000.cpugcc_r 502.gcc_r.cmd001.cpugcc_r \
          502.gcc_r.cmd002.cpugcc_r 531.deepsjeng_r.cmd000.deepsjeng_r \
          557.xz_r.cmd000.xz_r 557.xz_r.cmd001.xz_r \
  --run-id warmup_50000000 --jobs 8 \
  --label btb_v1 --purpose "BTB tasarımının mevcut inputlarda ölçümü" \
  --gem5 experimental/build/RISCV/gem5.fast
```

Bu örnek seçilen altı testin **tamamlanmış checkpoint setlerinin** mevcut
olmasını gerektirir. Global ön kontrol hatasında hiçbir iş başlatılmaz.
Testler virgülle de ayrılabilir. `--jobs` pozitif tamsayıdır; CPU/RAM
kapasitesine göre seçilir, üst sınır betikçe dayatılmaz.

`cmd000`, SPEC komutunun sıfır tabanlı indeksidir; SimPoint veya cluster
numarası değildir. ID doğrudan manifestteki `workload_id` ile eşleşir.
Aynı workload farklı input batch'lerinde varsa `--batch-root` zorunludur;
araç sessizce en yeni veya en eski girdiyi seçmez. Tek eşleşme varsa
`research-result/batch_results/` altında otomatik bulur.

Birden çok Step B setini seçmek için
`--run-ids warmup_50000000 warmup_100000000` kullanılır; virgülle ayrılan
ID'ler de kabul edilir. `--run-id` aynı seçeneğin tekil yazımıdır. Seçilen
workload × Step B koşusu çiftlerinin checkpoint'leri ortak `--jobs`
kuyruğuna girer; her çiftin toplamı ve raporu ayrı tutulur. Verilen bütün
setlerin bütün seçili workload'larda bulunması gerekir.

## 2. Tek SPEC komutunu başlangıçtan bitişe O3 ile çalıştırma

```bash
python3 research/gem5_run/se_mode/run_o3_full.py \
  --batch-root research-result/batch_results/train_reduced_retry01 \
  --test 502.gcc_r.cmd002.cpugcc_r \
  --gem5 experimental/build/RISCV/gem5.fast \
  --label full_baseline \
  --purpose "GCC komut 2 tam program O3 CPI referansı"
```

Bu betik mevcut profile manifestinden komutu, binary/input, environment,
sysroot ve sistem ayarlarını alır. Orijinal workload manifestini ve kaynak
input dizinini fingerprint ile doğrular, yeni `workdir/` içine kopyalar ve
**checkpoint restore etmeden**, programın başından başarılı çıkışına kadar
O3 çalıştırır. Step A/Step B'yi tekrar çalıştıran bir pipeline değildir.
Tam başlangıç girdisi için checkpoint snapshot'ı kullanılmaz: snapshot,
programın ilerlemiş halindeki üretilmiş dosyaları içerebilir.
Tam O3 koşusu checkpoint kullanmadığı için Step B koşu ID'si almaz.

Ön kontrol için `--dry-run`; yalnızca kısa altyapı kontrolü için
`--max-insts 1M` eklenebilir. Instruction sınırı nedeniyle erken duran koşu
başarısız/tamamlanmamış sayılır ve tam program CPI sonucu üretmez.
Bu betik bir komut çalıştırır; checkpoint scheduler'ındaki `--jobs` bu
betiğe ait değildir.

## 3. Konfigürasyon ve deney kimliği

Her iki betik de aşağıdaki seçenekleri destekler:

```text
--clock 3GHz
--l1i-size 32KiB --l1d-size 32KiB --l2-size 256KiB
--branch-predictor TAGE_SC_L_64KB
--cpu-param fetchWidth=8 --cpu-param commitWidth=8
--bp-param NAME=VALUE
--configuration-module path/to/configure.py
--label baseline
--purpose "Bu koşuda neyi ölçüyoruz?"
```

Özel modül Step C'nin mevcut `configure(board, processor,
cache_hierarchy, config)` sözleşmesini kullanır. `--label` dosya adına uygun
harf/rakam/`._-` içerir. `--purpose` raporda açıklama olarak saklanır.
`warmup_50000000` gibi Step B koşu ID'si girdi setini tanımlar; her yeni
çağrı ayrıca üstüne yazmayı önleyen şu benzersiz session kimliğini üretir:

```text
<UTC tarih-saat>_<label>_<binary SHA256 ilk 12 karakter>_<rastgele kimlik>
```

Aynı parametrelerle tekrar çalıştırmak da yeni bir kimlik oluşturur.
Önceki koşu tekrar kullanılmaz, üzerine yazılmaz; bu betiklerde `--force`
yoktur. Yarım kalmış koşunun yerine yeni session açılır. Bir workload'u
yeniden çalıştırmak için o workload'un test adını tekrar seçin. Otomatik
resume veya başarısız checkpoint'i tek başına yeniden birleştirme yoktur.

JSON raporları binary'nin mutlak yolu, SHA-256, sürüm/build bilgisi,
yakındaki Git commit'i, tracked yerel diff'in hash'i, host/Python bilgisi,
orchestrator kaynak dosyalarının hash'leri, gerçek komutlar ve O3
parametrelerini saklar. Commit, mevcut ağacın bilgisidir; binary'nin hangi
yerel değişikliklerle derlendiğini tek başına kanıtlamaz. Binary hash'i
hangi executable'ın kullanıldığını ayırır. Koşu sürerken binary veya
betikleri değiştirmeyin; sabit deney girdileri kullanın.

Checkpoint ve O3 commit'leri farklıysa Step C varsayılan olarak reddeder.
Uyumluluğu değerlendirdikten sonra batch'e `--allow-gem5-mismatch`
verilebilir. Farklı binary SHA değerleri ve aynı commit, checkpoint
uyumluluğunun doğrulandığı anlamına gelmez. `--dry-run` da gerçek O3
restore uyumluluğunu veya SimObject parametrelerini çalıştırarak sınamaz.

## 4. Dizin ve log sözleşmesi

Mevcut `profile/` ve `checkpoints/` dizinleri korunur. Profil dizini
`o1_profile/`, checkpoint dizini `o2_checkpoint/` adındaysa bu isimler de
tanınır (aynı manifest ve `warmup_<N>/` alt yapısıyla).

```text
research-result/batch_results/<input>/
  o3_batches/<session>/
    batch_report.json
    batch_report.md
  results/502.gcc_r.cmd002.cpugcc_r/
    profile/                         # mevcut; okunur
    checkpoints/warmup_50000000/      # mevcut; okunur
    o3_runs/warmup_50000000/<session>/
      report.json
      report.md                      # tüm cluster'lar + toplam
      sp_000_interval_00000004.driver.log
      sp_000_interval_00000004/
        run_manifest.json
        runtime_config.json
        runtime_status.json
        checkpoint_restore.json      # kaynak/view hash'leri ve VMA yolları
        restore_checkpoint/
          m5.cpt                     # sadece bu koşuya ait metadata kopyası
          board.physmem.store0.pmem   # kaynak bellek dosyasına symlink
        run.log                      # gem5 stdout + stderr
        stats.txt
        config.json                  # gem5 SimObject konfigürasyonu
        config.ini
        workdir/                     # bu restore'a özel snapshot kopyası
      ...
    o3_full_runs/<başka-session>/
      report.md                      # tam O3 CPI/IPC
      run_manifest.json
      runtime_config.json
      runtime_status.json
      run.log
      stats.txt
      config.json
      config.ini
      workdir/                       # orijinal input'un bu koşuya kopyası
```

Batch özeti her workload/Step B koşusu çiftinin raporuna bağlantı verir.
JSON ve Markdown raporlarında tam workload ID ve `step_b_run_id` bulunur.
Test raporunda cluster ID,
interval index, ağırlık, warmup, gerçek instruction/cycle sayıları, CPI,
IPC, ağırlıklı CPI katkısı, overshoot, durum ve driver log bağlantısı
bulunur. Detaylı gem5 hataları checkpoint'in `run.log` dosyasında,
Python/ön hazırlık hataları `.driver.log` dosyasındadır. JSON'daki mutlak
yollar VDS'de doğrudan izlenebilir; arşiv başka köke taşınırsa bu bağlantılar
eski kökü gösterir.

Markdown ve JSON raporları önce oluşturulur, her checkpoint tamamlandığında
atomik dosya değişimiyle güncellenir. Bu değişim sadece yeni session'ın
raporlarına uygulanır. Koşu başında raporda bekleyen işler de görünür.
İşlerden biri başarısız olsa bile diğer işler devam eder. En az bir hata
varsa batch dönüş kodu `1`, global ön kontrol hatası `2`, kullanıcı
kesintisi `130` olur. İptalde yalnızca bu çağrının oluşturduğu alt süreç
session'ları durdurulur; daha önce çalışan gem5 süreçlerine dokunulmaz.

Restore için profil/checkpoint manifestleri, `m5.cpt`, bellek dosyaları ve
filesystem snapshot gerekir. Eski SPEC export dizini ve ayrı workload
manifesti Step C için aranmaz. Dinamik executable'ın sysroot'u ve manifestte
referans verilen diğer harici host kaynakları yine bulunmalıdır. Snapshot
veya kaynak ağaçtaki mevcut dosyalar silinmez; her iş kendi kopyasında
çalışır. Manifestlerdeki mutlak yolların makinede geçerli olması gerekir.

## 5. CPI, weight, cluster ve toplam ne demek?

| Terim | Anlamı |
| --- | --- |
| BBV | Bir instruction interval'indeki basic block davranış vektörü |
| Cluster | Benzer BBV interval'lerinin oluşturduğu grup |
| SimPoint | Bir cluster'ı temsil eden seçili interval |
| Cluster ID | Clustering çıktısının kimliği; checkpoint ordinal'i değildir |
| Interval index | Sıfır tabanlı temsilci pencere indeksi |
| Ordinal | Checkpoint'lerin başlangıç konumuna göre sırası (`sp_000` vb.) |
| Weight, `w_i` | Cluster'ın profillenen interval'lerdeki instruction payı |
| Warmup | Ölçüm öncesi O3/cache/predictor durumunu ısıtan yürütme |
| `N_i` | Ölçümde, warmup sonrası gerçekten commit edilen instruction |
| `C_i` | Aynı ölçüm penceresinde CPU cycle sayısı |
| CPI | Bir instruction başına CPU cycle; `C_i/N_i` |
| IPC | Bir CPU cycle başına instruction; `N_i/C_i` |
| Katkı | Cluster'ın toplam tahmine katkısı; `w_i*CPI_i` |
| Overshoot | İstenen sayıyı geniş commit ile aşan instruction |
| Wall seconds | Host'ta geçen süre; simüle edilen CPU cycle sayısı değildir |

Her checkpoint restore sonrası manifestteki gerçek warmup kadar çalışır.
Sonra sayaçlar reset edilir, bir BBV interval kadar ölçüm alınır. Cache ve
predictor çalışmaya devam eder. Ölçüm uzunluğu sonraki checkpoint'e olan
mesafe değildir.

```text
CPI_i = C_i / N_i
IPC_i = N_i / C_i
CPI_test = sum_i(w_i * CPI_i)
IPC_test_estimated = 1 / CPI_test
```

Örneğin ağırlıklar `0.25, 0.75`, CPI'ler `2, 4` ise toplam CPI
`0.25*2 + 0.75*4 = 3.5`, tahmini IPC `1/3.5 = 0.285714` olur.
`sum(w_i*IPC_i)` kullanılmaz. CPI için toplam örnek cycle'ını toplam örnek
instruction'a bölmek de cluster ağırlıklarını yok sayar.

Toplam için bütün beklenen cluster/interval çiftleri tam bir kez mevcut
olmalı ve hepsi başarılı ölçülmelidir. Ağırlıklar profile manifestindeki
değerlerle eşleşmeli, sonlu/negatif olmayan sayılar olmalı ve toplamları
`1e-4` toleransla 1 olmalıdır. Dosya yazımındaki ondalık yuvarlamalar
nedeniyle toplam `0.9999997` olabilir; ham ağırlıklar kullanılır. Eksik
cluster atlanmaz ve kalan ağırlıklar 1'e normalize edilmez. Eksik test
raporu cluster durumlarını korur, toplam alanını boş bırakır.

Stats parser son tam `Begin/End Simulation Statistics` bloğunu okur.
Tek CPU `.numCycles` sayacı gerekir; `simInsts`, runtime'da ölçülen gerçek
instruction farkıyla tam eşleşmelidir. Boş, kesilmiş veya tutarsız dump
başarısız iş olarak raporlanır. Branch misprediction sayaçlarının varlığı
CPI raporunun ön koşulu değildir. Gerçek overshoot JSON/Markdown'a yazılır.

`502.gcc_r.cmd002.cpugcc_r` toplamı kendi input/komutuna ve seçilen Step B
koşusuna aittir. Başka workload veya Step B setlerinin ağırlıklarıyla
birleştirilmez; batch tek bir ortak SPEC CPI'si üretmez.
Profilin BBV kapsamı ve toplam instruction sayısı raporda ayrıdır. Son kısa
BBV interval'i ve tamamlanmamış profil kapsamı tam program tahmini olarak
sunulmamalıdır. Tam O3 referansı ise programın başlangıcındaki soğuk durum
ve son kısa interval dahil bütün yürütmeyi ölçer. Bu farklar nedeniyle tam
O3 CPI ve SimPoint tahmini birebir aynı olmak zorunda değildir.

## 6. SSH üzerinden log izleme ve doğrulama

SSH oturumundan bağımsız çalıştırmak için komutu `tmux` veya `screen`
içinde başlatabilirsiniz. Mevcut batch'in süreçlerine müdahale etmeyin.
Betik başlangıçta rapor yolunu, iş bitimlerinde test/checkpoint durumunu
yazar. İlgili yeni session logunu izlemek için:

```bash
RUN=research-result/batch_results/<input>/results/<workload>
tail -f "$RUN/o3_runs/warmup_50000000/<session>/<checkpoint>.driver.log"
```

Betiklerin simülasyon gerektirmeyen testleri:

```bash
python3 -m unittest discover \
  -s research/gem5_run/se_mode/tests -p 'test_o3_batch.py' -v
```

Bu testler geçici dizin ve sahte gem5 süreci kullanarak paralellik sınırını,
bağımsız hataları, tam set toplamını, tekrar koşularda dosya korunmasını,
snapshot üzerinden restore hazırlığını, stats tutarlılığını ve tam O3
başarılı/erken bitiş ayrımını doğrular. Gerçek restore için aşağıdaki ayrı
smoke testi kullanılabilir; tam SPEC yürütme bu testlerin kapsamı dışındadır.

## 7. File-backed VMA ve sysroot restore düzeltmesi

Bazı gem5 sürümleri checkpoint'teki dosyaya bağlı VMA adını host üzerinde
doğrudan açar. Örneğin `/lib/libm.so.6` için `path_redirects` veya
`interpreter_root` bu restore yolunda uygulanmaz; dosya sysroot içinde
bulunsa bile `Failed to open ... while unserializing file-backed VMA`
hatası oluşur.

Step C artık `checkpoint_restore.py` ile kendi çıktı dizininde özel bir
`restore_checkpoint/` görünümü hazırlar. `m5.cpt` kopyasında sadece
`fileOffset` içeren VMA bölümlerinin `name` alanları çözülür:

```text
/lib/libm.so.6 -> /opt/riscv/sysroot/lib/libm.so.6
/lib/libc.so.6 -> /opt/riscv/sysroot/lib/libc.so.6
/work/input   -> bu koşunun workdir/input dosyası
```

Eski Step B workdir/snapshot host yolları da bu koşunun özel workdir'ine
eşlenir. Bellek içeriği, VMA adres aralıkları, dosya offset'leri ve FD
kayıtları değiştirilmez. Orijinal checkpoint'e yazılmaz. Fiziksel bellek
dosyalarına yalnızca restore okuması için symlink verilir; kaynak
checkpoint bu nedenle yerinde kalmalıdır. `runtime_config.json` yeni
görünüme işaret eder; kaynak checkpoint'in yolu manifestte korunur.
Yol eşlemeleri ve iki `m5.cpt` SHA-256 değeri `checkpoint_restore.json`
dosyasına ve run manifestine kaydedilir.

Bu, mevcut binary'lerle çalışan Step C uyumluluk düzeltmesidir; gem5 C++
kodu ve mevcut executable'lar değiştirilmez, yeniden derleme gerekmez.
`batch_o3_run.py` aynı Step C'yi çağırdığı için düzeltmeyi otomatik kullanır.
`--dry-run`, gereken VMA kaynaklarının doğru sysroot/snapshot altında
okunabildiğini de kontrol eder; eksik kütüphanede yanlış host kütüphanesine
geri dönüş yapılmaz.

Yol çözümü ve kaynak checkpoint'in korunması için birim testleri:

```bash
python3 -m unittest discover \
  -s research/gem5_run/se_mode/tests -p 'test_checkpoint_restore.py' -v
```

Gerçek gem5 ile kısa restore regresyon kontrolü:

```bash
CKPT_SET=research-result/batch_results/train_reduced_retry01/results
CKPT_SET="$CKPT_SET/502.gcc_r.cmd000.cpugcc_r/checkpoints/warmup_50000000"
POINT=sp_001_interval_00000126
CKPT_MANIFEST="$CKPT_SET/checkpoints/$POINT/checkpoint_manifest.json"
python3 research/gem5_run/se_mode/tests/run_checkpoint_restore_smoke.py \
  --gem5 build/RISCV/gem5.fast \
  --checkpoint-manifest "$CKPT_MANIFEST" \
  --measurement 10000 --timeout 120
```

Bu kontrol **0 warmup + 10.000 instruction** tanılama penceresi çalıştırır;
SimPoint CPI sonucu olarak kullanılmaz. GDB listener'ı kapalıdır ve süre
sınırı vardır. Her çağrı `research-result/o3_validation/<yeni-session>/`
altında log, runtime/stats, `validation.json` ve `validation.md` oluşturur.
Kaynak checkpoint dosyalarının ve filesystem snapshot'ın önce/sonra
checksum'larını karşılaştırır. Kullanıcı kesintisinde veya timeout'ta
yalnızca kendi başlattığı süreç durdurulur.
