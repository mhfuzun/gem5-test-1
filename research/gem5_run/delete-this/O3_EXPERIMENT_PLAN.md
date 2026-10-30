# SimPoint checkpoint'lerinden O3 deney ve yayın planı

## 1. Önerilen ana deney

Her checkpoint için yeni bir gem5 süreci başlat:

```text
Atomic checkpoint'i O3 ile restore et
  -> manifestteki actual_warmup kadar O3 çalıştır
  -> yalnızca istatistik sayaçlarını sıfırla
  -> bir BBV interval kadar O3 ölçüm yap
  -> istatistikleri kaydet ve bu koşuyu bitir
```

Mevcut 200M BBV interval ve 50M checkpoint warmup sözleşmesi için başlangıç
önerim **50M committed instruction warmup + 200M committed instruction
ölçüm**. Bu süreler başlangıç tercihleridir; 50M'nin yeterliliği ayrıca
doğrulanmalıdır. Daha erken bir SimPoint için gerçek warmup 50M'den kısa
olabilir; `checkpoint_manifest.json` içindeki değer esas alınır.

Warmup sırasında branch predictor, BTB, cache ve diğer yapılar hedef O3
konfigürasyonu altında normal şekilde güncellenir. Ölçüm sırasında da bu
güncellemeler devam eder. Buradaki "eğitim", çalışan işlemcinin normal
adaptasyonudur; ölçümde predictor öğrenmesini kapatmak anlamına gelmez.
`m5.stats.reset()` cache'i veya predictor tablolarını temizlememelidir.
Özel frontend kodunun reset davranışı da bu açıdan doğrulanmalıdır.

gem5'in resmî SimPoint restore örneği warmup bitişinde sayaçları sıfırlayıp
bir SimPoint interval'i çalıştırır. Örnek x86 ve Timing CPU kullanır;
RISC-V Atomic -> O3 uyumluluğunun kanıtı değildir.
[Resmî restore örneği](https://github.com/gem5/gem5/blob/stable/configs/example/gem5_library/checkpoints/simpoints-se-restore.py)

## 2. Sonraki checkpoint'e kadar devam edilmeyecek

Her SimPoint, kendi cluster'ının temsilcisidir. Checkpoint'ler arasındaki
mesafe ölçüm uzunluğu değildir. Aradaki instruction'ları O3 ile çalıştırmak
veya önceki örnekten predictor/cache durumu taşımak bu deneyin parçası olmaz.

Örneğin `I = 200M`, `W = 50M`, SimPoint indeksi `20` ise:

```text
S = 20 * 200M = 4,000M       ölçüm başlangıcı
C = S - 50M  = 3,950M       checkpoint

3,950M                 4,000M                            4,200M
restore ----- 50M O3 -----|---------- 200M O3 ---------------|
          warmup         reset           measurement      dump/stop
```

Bir sonraki seçilen SimPoint `10,000M`'deyse onun checkpoint'inden yeni bir
süreç başlatılır. İki koşu bağımsızdır; farklı host çekirdeklerinde paralel
çalışabilir. Aynı checkpoint, baseline ve her aday tasarım için yeniden
restore edilir. O3 ile eğitilmiş bir adayın durumunu başka adaya aktarmayız.

## 3. 100M mi, 200M mi ölçüm?

Ana sonuçta `measurement = BBV interval` kullan. Mevcut profil 200M ise
200M ölç. Bu seçim, ağırlığı hesaplanan temsilci bölgeyi ölçmeni sağlar.

200M'lik bir temsilcinin yalnızca ilk 100M'sini ölçmek teknik olarak mümkün,
fakat elde edilen sonuç aynı cluster'ın 200M temsilcisiyle eşdeğer kabul
edilemez. Ağırlıklar pencereyi kısaltınca otomatik olarak düzelmez.

100M ölçüm istenirse iki seçenek vardır:

1. Ana deney için Step A'yı 100M interval ile yeniden çalıştır; ardından
   Step B checkpoint'lerini yeni SimPoint'ler için üret.
2. Mevcut 200M noktalarındaki 100M ölçümleri keşif/süre azaltma deneyi olarak
   kullan; 200M sonuçlarına karşı hatasını ayrıca göster.

SimPoint geliştiricilerinin kullanım akışı da seçilen noktanın kendi
interval uzunluğu kadar simüle edilmesini tarif eder.
[SimPoint kullanım açıklaması](https://cseweb.ucsd.edu/~calder/simpoint/software-FAQ.htm)

## 4. Warmup yeterliliği nasıl doğrulanacak?

50M'yi yeterli kabul etmeden önce, baseline ve önemli aday tasarımları aynı
ölçüm başlangıçlarında 50M ve 100M warmup ile karşılaştır. Başlangıçta yüksek
ağırlıklı ve farklı davranış gösteren noktaları pilot olarak seç; yayın
deneyinde mümkünse bütün noktalarda kontrol et. Büyük veya yavaş öğrenen
yapılarda sonuçlar değişiyorsa 200M gibi daha uzun bir warmup da dene.

**Warmup değiştiğinde ölçüm başlangıcı sabit kalmalı.** 50M önceden alınmış
checkpoint'i restore edip 100M warmup çalıştırmak ölçümü 50M kaydırır.
100M deneyi için aynı profile manifestinden ayrı Step B seti üret:

```bash
python3 research/gem5_run/se_mode/02_checkpoint.py \
  --gem5 build/RISCV/gem5.fast \
  --profile-manifest research-result/batch_results/train_reduced/results/531.deepsjeng_r.cmd000.deepsjeng_r/profile/profile_manifest.json \
  --warmup 100M
```

Ölçüm penceresi her iki sette de `[S, S + I)` olur; checkpoint konumu
`max(0, S-W)`, gerçek warmup `min(W,S)` olur. `S=0` noktası için programın
öncesinde instruction bulunmadığından warmup sıfırdır; bu gerçek başlangıç
durumunu sırf soğuk diye atlama.

Kontrol edilecekler: ağırlıklı CPI, aday/baseline speedup, branch MPKI,
BTB miss davranışı ve I-cache MPKI. Yalnızca toplam CPI'ye bakmak,
noktalar arasında birbirini götüren hataları saklayabilir. Örnek bir kabul
hedefi ağırlıklı CPI ve speedup'ın yaklaşık %1 içinde kalmasıdır; bu evrensel
bir standart değildir. Beklenen katkı %1 düzeyindeyse çok daha küçük
metodolojik hata gerekir. Eşiği sonuçları görmeden belirle ve raporla.

## 5. Mevcut scriptlerle çalıştırma

`run_spec_batch.py` workload + Step A + Step B üretir. Step C için henüz
ayrı bir toplu O3 scheduler yoktur; `03_run_o3.py` bir checkpoint ve bir
konfigürasyon çalıştırır. Deney işi şu çarpımdır:

```text
workload x SimPoint x O3 konfigürasyonu x warmup seti
```

Önce checkpoint set manifestindeki `checkpoint_manifests` listesini oku.
Her girdiyi aşağıdaki komuta `--checkpoint-manifest` olarak ver. Buradaki
`<checkpoint-id>` gerçek dizin adıyla değiştirilmelidir:

```bash
python3 research/gem5_run/se_mode/03_run_o3.py \
  --gem5 build/RISCV/gem5.opt \
  --checkpoint-manifest research-result/batch_results/train_reduced/results/531.deepsjeng_r.cmd000.deepsjeng_r/checkpoints/warmup_50000000/checkpoints/<checkpoint-id>/checkpoint_manifest.json \
  --l1i-size 32KiB \
  --l1d-size 32KiB \
  --l2-size 256KiB \
  --branch-predictor TAGE_SC_L_64KB
```

`--measurement` verilmezse checkpoint'teki BBV interval kullanılır.
Warmup CLI'dan verilmez; manifestten otomatik okunur. 200M profil için
`--measurement 200M` yazılması da aynı sonucu verir.

Varsayılan çıktı, ilgili workload'un `o3_runs/<checkpoint-id>/<run-id>/`
altına gider; batch'in profilleri `research-result` içinde olduğundan Step C
çıktısı da orada kalır. Her tasarımın bütün checkpoint koşularını ayrı bir
sonuç kökünde tutmak toplamayı kolaylaştırır. Bunun için `--output` ile
örneğin şu dizin sözleşmesini kullanabilirsin:

```text
research-result/o3_experiments/<experiment>/
  baseline/<workload-id>/warmup_50M/<checkpoint-id>/
  candidate_A/<workload-id>/warmup_50M/<checkpoint-id>/
  baseline/<workload-id>/warmup_100M/<checkpoint-id>/
```

O3 için `gem5.opt`, Atomic hazırlığı için `gem5.fast` kullanılabilir;
ikisi aynı incelenmiş kaynak ağacından üretilmeli. `fast/opt` host executable
seçimidir; simüle edilen CPU'nun Atomic/O3 seçimiyle aynı şey değildir.
Kaynak değişiklikleri ve build seçenekleri de deney kaydına alınmalıdır.

## 6. SimPoint sonuçları nasıl birleştirilecek?

`w_i`: cluster'ın temsil ettiği instruction payı; `N_i`: gerçek ölçülen
committed instruction; `C_i`: ölçümdeki CPU cycle sayısı olsun.

```text
CPI_i = C_i / N_i
IPC_i = N_i / C_i

CPI_workload = sum_i(w_i * CPI_i)
IPC_estimated = 1 / CPI_workload
              = 1 / sum_i(w_i / IPC_i)
```

`sum_i(w_i * IPC_i)` kullanma. IPC burada ağırlıklı harmonik ortalama ile
birleşir; ağırlıklar instruction payıdır, cycle payı değildir. SimPoint 3.0
makalesi de IPC için önce CPI ağırlıklandırılmasını açıkça belirtir.
[SimPoint 3.0, bölüm 5](https://cseweb.ucsd.edu/~calder/papers/JILP-05-SimPoint3.pdf)

Örnek: ağırlıklar `0.5, 0.5`, IPC'ler `1, 2` olsun:

```text
CPI_weighted = 0.5 * 1 + 0.5 * 0.5 = 0.75
IPC_estimated = 1 / 0.75 = 1.3333
```

Aritmetik ağırlıklı IPC `1.5` verir ve bu örnekte performansı fazla tahmin
eder. Noktaların farklı hızlarda çalışması zaten bu ayrımı gerekli kılar.

Mevcut `summarize.py`, ağırlıklı CPI ve onun tersini hesaplar. Tek workload,
tek konfigürasyon ve tek warmup setinin sonuç kökünü ver:

```bash
python3 research/gem5_run/se_mode/summarize.py \
  --runs-root research-result/o3_experiments/exp01/baseline/531.deepsjeng_r.cmd000.deepsjeng_r/warmup_50M
```

Araç bütün SimPoint'lerin mevcut olmasını, tekrar olmamasını ve ağırlık
toplamını kontrol eder. Birden fazla konfigürasyon aynı kökteyse seçim ister;
bu nedenle yukarıdaki ayrı dizin düzenini öneriyorum.

## 7. Baseline ile kıyaslama

Önce her tasarım için bütün SimPoint'lerden workload CPI'sini hesapla.
Ardından aynı workload'un baseline ve aday sonuçlarını karşılaştır:

```text
Speedup = CPI_baseline / CPI_candidate
        = IPC_candidate / IPC_baseline       (aynı frekansta)
Performance_gain_percent = 100 * (Speedup - 1)
Time_reduction_percent = 100 * (1 - 1 / Speedup)
```

Örneğin CPI `1.00 -> 0.90` ise speedup `1.1111`, performans artışı `%11.11`,
yürütme süresi azalması `%10` olur. Bunları aynı yüzde diye sunma.

Ana sonuç için `sum(weight * checkpoint_speedup)` da kullanma; önce iki
tasarımın CPI'lerini ayrı ayrı birleştir, sonra oranlarını al.

Frekanslar farklıysa IPC tek başına hız değildir:

```text
Estimated_time = N_workload * CPI_workload / frequency
Speedup = (CPI_baseline / frequency_baseline)
          / (CPI_candidate / frequency_candidate)
```

Frontend/BTB değişikliğini incelerken frekans, backend, cache, bellek ve
workload'ları sabit tut. Kapasite artışıyla algoritmik katkıyı ayırmak için
eşit depolama bütçeli karşılaştırmalar ve bileşenleri tek tek kapatan
ablation deneyleri ekle. gem5 IPC sonucu tek başına alan, güç veya erişilebilir
saat frekansı kazancı göstermez.

## 8. Birden fazla workload ve benchmark

SimPoint ağırlığı sadece üretildiği program/komut/input için geçerlidir.
`gcc.cmd000` ağırlığını `gcc.cmd001` veya `leela` ile ortak havuzda toplama.

Çok komutlu benchmark için iki açık raporlama seçeneği vardır:

- Her komutu bağımsız workload olarak göster; genel ortalamanın komutlara
  mı benchmark'lara mı eşit ağırlık verdiğini belirt.
- Bir benchmark'ın bütün komutlarının ardışık çalışmasının toplamını
  tahmin et: her komutun `N_j * CPI_j / f_j` süresini topla, ardından
  baseline toplamını aday toplamına böl. Eşit komut IPC ortalaması kullanma.

İkinci seçenek için `N_j`, seçilen input'un toplam instruction sayısıdır;
sadece simüle edilen checkpoint instruction toplamı değildir. Profillemede
kapsanmayan kısa son interval varsa hangi instruction kapsamının tahmin
edildiğini ve toplam coverage'ı ayrıca belirt.

Bağımsız benchmark'lar için her birinin speedup'ını raporla; genel tasarım
karşılaştırmasında önceden belirlenmiş benchmark kümesinin geometrik
ortalamasını kullan:

```text
GM_speedup = exp(sum_b(log(Speedup_b)) / benchmark_count)
```

Bu eşit benchmark ağırlıklı deney özeti, resmî SPEC skoru değildir. Dört
benchmark kullanıyorsan iddianın kapsamı bu dört benchmark ve seçilen
input'lardır. Yalnızca iyi sonuç veren workload'ları ortalamaya dahil etme.

## 9. IPC yanında açıklayıcı metrikler

Ana performans metrikleri: ağırlıklı CPI, tahminî IPC ve baseline'a göre
speedup. Neden hızlandığını göstermek için frontend'e uygun ek metrikler
topla:

| Metrik | Birleştirme yaklaşımı |
| --- | --- |
| Branch MPKI | `sum(w_i * 1000 * mispredictions_i / N_i)` |
| BTB veya I-cache MPKI | `sum(w_i * 1000 * misses_i / N_i)` |
| Miss rate | `sum(w_i * misses_i/N_i) / sum(w_i * accesses_i/N_i)` |
| Stall cycle / instruction | `sum(w_i * stall_cycles_i/N_i)` |
| Stall cycle payı | `sum(w_i * stall_cycles_i/N_i) / CPI_workload` |

Miss rate yüzdelerini doğrudan ağırlıklı ortalamak, access yoğunluğu farklı
fazlarda yanlış sonuç verebilir. Fetch bandwidth, squash/recovery ve BTB
hit/miss dağılımları için de numerator ve denominator'ın anlamını yaz.

Özel BTB sayacı, branch predictor yanlış tahmin sayacı ve pipeline squash
sayacı farklı olayları sayabilir. Sayaç isimlerini ve hangi olayda arttığını
kaynak koddan doğrula; speculative olaylar için "committed instruction
başına olay" şeklinde açık bir tanım kullan. Sayaç düşüşünün IPC kazancına
dönüşüp dönüşmediğini de göster.

## 10. Yayın öncesi doğrulama ve mevcut araçların sınırları

1. Aynı binary/input/SimPoint/weight kümesini bütün tasarımlarda kullan.
   Workload fingerprint, gem5 commit ve yerel diff, build seçenekleri,
   cache/CPU/BP parametreleri, seed ve host araç sürümlerini sakla.
2. RISC-V Atomic checkpoint -> O3 restore akışını kısa, tamamı O3 ile de
   çalıştırılabilen bir workload'da doğrula. PC/instruction devamlılığı ve
   dosya snapshot'larının doğru restore edildiğini kontrol et.
3. Warmup'tan sonra stats reset olduğunu; gerçek measurement instruction
   sayısı ve cycle sayısının aynı pencereye ait olduğunu doğrula.
   O3 commit genişliği nedeniyle birkaç instruction overshoot kaydedilebilir;
   CPI/MPKI'da gerçek sayıyı kullan ve büyük sapmaları hata kabul et.
4. Erken program çıkışı, eksik checkpoint, tamamlanmamış profil veya eksik
   ağırlık varsa ana sonucu üretme. Başarısız noktayı atıp kalan ağırlıkları
   sessizce 1'e normalize etme.
5. Aynı koşuyu iki kez tekrarlayıp instruction/cycle ve bitiş durumunu
   karşılaştır. Deterministik koşunun tekrarları örnekleme hatasına güven
   aralığı vermez; cluster'lar da bağımsız rastgele deney tekrarları değildir.
6. Uygulanabilir küçük input'larda tam O3 yürütmeyle SimPoint tahminini
   karşılaştır. `abs(CPI_estimated/CPI_full - 1)` hatasını ve speedup/ranking
   tutarlılığını ölç; birkaç clustering seed ve `max-k` ile hassasiyeti incele.
7. 50M/100M warmup karşılaştırmasını aynı ölçüm penceresinde yap. Hem mutlak
   CPI hem aday/baseline oranı kararlı mı kontrol et.

Mevcut scriptler ölçümün temel akışını destekler; aşağıdakiler yayın
kontrolünde ayrıca ele alınmalıdır:

- `03_run_o3.py`, 200M profilden 100M ölçüme izin verir ve farklılığı
  `measurement_matches_bbv_interval` alanına yazar. `summarize.py` bu alan
  `false` olduğunda otomatik reddetmez; ana deneyde bunu dışarıdan denetle.
- `summarize.py` konfigürasyon gruplamasına warmup setini katmaz. Aynı
  konfigürasyonun 50M/100M sonuçlarını aynı sonuç köküne koyma. Ayrıca
  summarizer'ın grup kimliği, Step C'nin checkpoint'e özgü run kimliğinden
  farklıdır.
- Özetleyici `simInsts`, tek `.numCycles` sayacı ve desteklenen branch
  sayaçlarından metrik çıkarır. `stats.txt` birden fazla dump içeriyorsa
  kullanılan bloğun measurement bloğu olduğunu ve `simInsts` değerinin
  runtime'daki measurement delta ile uyumunu doğrula. Bu parser bütün
  özel frontend istatistikleri için genel bir doğrulayıcı değildir.
- Batch, Step C'yi otomatik başlatmaz. Çok sayıda deney için eklenecek O3
  scheduler; bağımsız hata yalıtımı, resume ve yalnızca tam sonuç setinde
  toplama sözleşmesini korumalıdır.

## 11. Sadeleştirilmiş train ile hangi iddia yapılabilir?

SimPoint, yalnızca profillenen binary/input akışını temsil eder.
`train_reduced` üstünde program sonuna kadar profil almak, orijinal train
veya ref input'larını temsil ettiğini göstermez.

Önerim: `train_reduced` ile altyapıyı doğrula ve tasarım keşfi yap; yayın
değerlendirmesinde mümkünse önceden ayrılmış daha büyük train/ref input'ları
da kullan. Kaynak sınırı nedeniyle yalnızca reduced input kullanılacaksa
hangi girdinin nasıl azaltıldığını, instruction kapsamını, faz sayısını ve
genelleme sınırını açıkça bildir. Parametreleri ayarladığın input'larla
bağımsız değerlendirme input'larını ayırmak sonuçların ikna gücünü artırır.

## 12. Uygulama sırası ve teslim edilecek sonuçlar

1. Bir workload ve tek checkpoint'te baseline O3 restore/ölçüm smoke testi.
2. O workload'un bütün checkpoint'lerinde baseline, 50M warmup + 200M ölçüm.
3. Aynı checkpoint'lerde aday tasarım; iki ayrı ağırlıklı CPI ve speedup.
4. 100M warmup setiyle sabit pencere hassasiyet deneyi.
5. Bütün seçili workload'lara yayma; açıklayıcı metrikler ve ablation'lar.
6. Benchmark başına speedup grafiği, geometrik ortalama, warmup ve
   örnekleme doğrulama tabloları; ham manifest/stats arşivi.

Örneğin 4 workload, workload başına 10 nokta ve 3 konfigürasyon için 120
O3 işi vardır. Her iş 50M + 200M instruction çalıştırırsa toplam yaklaşık
30 milyar detaylı instruction simüle edilir. Gerçek nokta sayısını Step A
belirler; `max-k=10` her zaman 10 nokta üretmez. Bu hesabı VDS zaman ve
disk bütçesi planında kullan.

Bütün ham sonuçları ve türetilmiş özetleri `research-result` altında tut.
Rsync ile kopyalanmış arşivler inceleme için uygundur; mevcut manifestler
mutlak yollar içerdiğinden başka makinede yeniden restore/toplama için aynı
yolların bulunması veya ayrı bir yol taşıma çözümü gerekir.

## Kaynaklar ve yerel kod

- [SimPoint 3.0 makalesi](https://cseweb.ucsd.edu/~calder/papers/JILP-05-SimPoint3.pdf)
- [SimPoint kullanım açıklaması](https://cseweb.ucsd.edu/~calder/simpoint/software-FAQ.htm)
- [gem5 resmî SimPoint restore örneği](https://github.com/gem5/gem5/blob/stable/configs/example/gem5_library/checkpoints/simpoints-se-restore.py)
- Yerel [Step B](02_checkpoint.py), [Step C](03_run_o3.py),
  [warmup/measurement olayları](gem5_config.py),
  [ağırlıklı özetleyici](summarize.py).

Buradaki 50M/100M warmup, 200M ana ölçüm ve kabul eşiği önerileri bu araştırma
için deney tasarımı tercihleridir; kaynakların bütün sistemler için garanti
ettiği değerler olarak sunulmamalıdır.
