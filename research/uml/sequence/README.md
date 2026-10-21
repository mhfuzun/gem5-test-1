# IBPU ve gem5 O3 sequence diyagramları

Bu dizin, mevcut gem5 çağrılarını önerilen IBPU bağlantılarıyla birlikte
gösterir. Diyagramlar bu çalışma ağacındaki kaynak koddan çıkarıldı;
`[N]` ve `[A]` adımları henüz uygulanmış C++ kodu değildir. Sınıf sözleşmesi:
[class_diagram_v2.puml](../class/class_diagram_v2.puml).

`tick_path.puml` ana akıştır. Diğer diyagramlar onun tahmin, fetch, feedback
ve yaşam döngüsü adımlarını açar. Her dosya tek `@startuml` / `@enduml` çifti
içerir ve bağımsız çizilebilir.

## Okuma sırası

1. [tick_path.puml](tick_path.puml): gem5 event queue → `CPU::tick()` →
   BAC → bridge → IBPU → FTQ → Fetch; ardından Decode, Rename, IEW ve Commit.
   Feedback, model ilerlemesi, override ve FTQ kabulünün sırasını gösterir.
2. [bpu_lookahead.puml](bpu_lookahead.puml): `m` bağımlı mBTB ön hesabı,
   uBTB yolu, donanım sorgu kabulü ve zamanlanmış mBTB karşılaştırması.
   Bu algoritma `CustomBPU` örneğidir; diğer IBPU'ların iç yapısını dayatmaz.
3. [ftq request.puml](<ftq request.puml>): hazır sonucu sahiplik devretmeden
   okuma, FTQ'ya ekleme, sonra `acceptFetchTarget()`; doluluk ve tüketim.
4. [fetch_blocks_merge.puml](fetch_blocks_merge.puml): FTQ block listesi →
   ITLB/I-cache istekleri → retry/completion/fault → sıralı byte parçaları →
   ISA decoder → mevcut Fetch–Decode zaman tamponu.
5. [predecode_feedback.puml](predecode_feedback.puml): `buildInst()` →
   `BAC::updatePC()` → `bindAndPredict()` → dinamik CFI eşleştirmesi;
   block geçişi, predecode düzeltmesi ve tüm target'ın tüketilmesi.
6. [commit_update.puml](commit_update.puml): Commit'in kümülatif
   `doneSeqNum` bilgisi → ledger → modele özel BTB/direction eğitimi.
7. [backend_recovery.puml](backend_recovery.puml): IEW → Commit → BAC/Fetch
   recovery yayını; ayrıca Decode ve Fetch kaynaklı düzeltme yolları.
8. [mbtb_late_recover.puml](mbtb_late_recover.puml): uBTB/mBTB uyuşmazlığı;
   etkilenen tahminin bulunduğu aşamaya göre yerel veya O3 üzerinden onarım.
9. [lifecycle.puml](lifecycle.puml): startup/reset, zamanlanmış uyanma,
   Commit güvenli noktası üzerinden drain, resume ve CPU takeover.

## Okların anlamı

- `[E]`: kaynak kodda bulunan çağrı, dönüş veya sinyal.
- `[N]`: IBPU, bridge veya yardımcı sınıfa eklenecek bağlantı. Var olan
  metodu yeni bir çağırandan kullanmak da bu işaretle gösterilebilir.
- `[A]`: algoritma ya da özel yardımcı işlem. Metot adı görünümündeki bir
  etiketin sınıf diyagramında veya mevcut C++ kodunda tanımlı olması gerekmez.

Kesik dönüş okları veya zaman tamponu teslimleri bir sonraki C++ fonksiyonun
aynı cycle'da çağrıldığı anlamına gelmez. `... delay ...` ve ilgili notlar
sinyalin gecikmesini belirtir. Bellek callback'leri de Fetch tick'inden
ayrı olaylardır. Diyagramlardaki sıra, alt akışlar arasında çağrı/olay
sırasıdır; çizimdeki dikey mesafe bir cycle ölçeği değildir.

## Mevcut çağrıları nereye bağlayacağız?

| Mevcut yer | Önerilen bağlantı / korunacak davranış |
| --- | --- |
| `CPU::tick()` | BAC → Fetch → Decode → Rename → IEW → Commit sırası. |
| `BAC::checkAndUpdateBPUSignals()` | IBPU modunda `forwardFeedback()`. |
| `BAC::tick()` | Legacy üretim yerine `bridge.service(now, credits)`. |
| `BAC::newFetchTarget()` / `FTQ::insert()` | `makeFT()` ve payload bağlama. |
| `Fetch::ftqReady()` / `FTQ::readHead()` | `BlockFetchEngine::begin(ft)`. |
| `Fetch::fetchCacheLine()` | `nextRead()` ile seçilen adres ve request tag. |
| `Fetch::finishTranslation()` | Sıralı fault ve iptal edilmiş istek kontrolü. |
| `Fetch::processCacheCompletion()` | Tag doğrulama → `onBytes()`. |
| `Fetch::pipelineIcacheAccesses()` | Aynı engine üzerinden sonraki parça. |
| `Fetch::buildInst()` sonrası `BAC::updatePC()` | `bindAndPredict()`. |
| Block bitişi | `finishBlock()` denetimi ve sonraki block'a geçiş. |
| Tüm target'ın `FTQ::popHead()` ile çıkarılması | `onTargetConsumed(key)`. |
| Commit'in `doneSeqNum` yayını | `onCommitUpdate({tid, throughSeq})`. |
| Commit/Decode/Fetch recovery sinyalleri | Tek sahip üzerinden `onRecovery()`. |
| Geç predictor override | Yerel onarım veya yeni `O3OverrideHook`. |
| BAC reset/drain/resume | IBPU yaşam döngüsü ve özel durum temizliği. |
| CPU boşta kalırken model deadline'ı | CPU'ya ait yeni wake event. |

Bu bağlantıların kaynakları:
[cpu.cc](../../../src/cpu/o3/cpu.cc),
[bac.cc](../../../src/cpu/o3/bac.cc),
[fetch.cc](../../../src/cpu/o3/fetch.cc),
[ftq.cc](../../../src/cpu/o3/ftq.cc),
[comm.hh](../../../src/cpu/o3/comm.hh),
[decode.cc](../../../src/cpu/o3/decode.cc),
[iew.cc](../../../src/cpu/o3/iew.cc),
[commit.cc](../../../src/cpu/o3/commit.cc).
Fonksiyon ve mevcut satır konumları diyagramların notlarında yer alır.

`bpuModel` seçimi, sınıf tasarımındaki Python/SimObject yapılandırmasıdır.
Ana tick diyagramı `decoupledFrontEnd=true` varsayar. Model seçilmediğinde
legacy yol korunur. Aynı tahmin için hem legacy `BPredUnit` hem IBPU history
işlemleri çağrılmaz: ayrım mevcut BAC feedback ve drain noktalarının içine
yerleştirilir. Mevcut decoupled kod tek thread'i destekler; döngüler SMT
desteğinin tamamlandığı anlamına gelmez.

## Zamanlama ve algoritma sözleşmesi

`m`, host üzerinde hazırlanacak tahmin sayısıdır. `n`, modellenen mBTB
pipeline gecikmesidir. Bağımlı bir yol oluşturmak için her ön hesaplama
adımında lookup, CFI seçimi ve sonraki sorgu adresi hesabı yapılır. Bunlar
izole history/checkpoint bağlamında yürür; canlı speculative history iki
kez ilerletilmez.

Ön hesaplanan sonuç, gerçek sorgunun PC'si, dinamik yolu ve lookup anındaki
ilgili predictor durumuyla eşleştiğinde kullanılabilir. Training veya
recovery bu bağımlılıkları değiştirdiyse ön hesaplama yenilenir. Tabloyu
sabit tutarak batching yapmak bilinçli bir yaklaşık model olabilir; cycle
cycle çalışan modelle aynı sonuçları verdiği varsayılmamalıdır.

`issueCycle`, `readyCycle` ve `acceptedCycle` ayrı tutulur. Örneğin iki
sorgu cycle 0'da kabul edilir, uBTB gecikmesi 1 ve mBTB gecikmesi 4 olursa,
sonuçlar cycle 1 ve 4'te görünür. Ek redirect gecikmesi yoksa yanlış yol
1–3 arasında ilerleyebilir. Host kodunun uyuşmazlığı erken hesaplaması,
slow sonucunu erken görünür yapmaz veya gecikmeyi yeniden başlatmaz.
Sorgu kabul aralığı, pipeline stall'ları ve redirect gecikmesi ayrıca
modellenir. Etkili düzeltme zamanına kadar yanlış yolda ilerleme sürer;
daha erken durdurma isteniyorsa bu ayrı bir model politikasıdır.

FTQ dolduğunda hazır sonuçların bütün zaman damgaları topluca kaydırılmaz.
FTQ'ya aktarım durur; yeni sorgu kabulü gerçek model kaynaklarına bağlıdır.
Kabul edilmiş sorguların tamamlanması ve kontrol olayları ilerlemelidir.
Elastik pipeline stall'ı varsa yalnızca etkilenen aşamalar ve henüz
tamamlanmamış işler onun kurallarına göre ötelenir. Kontrol kuyruğu dolu
veri kuyruğunun arkasında kilitlenmemelidir.

BAC, Fetch'ten önce çalıştığı için FTQ'ya eklenen sonuç aynı CPU tick'inde
Fetch tarafından görülebilir. İstenen yayın gecikmesi `readyCycle` veya
açık bir zamanlama sınırıyla belirtilmelidir. Mevcut `bacToFetchDelay`
alanı tek başına FTQ okumalarını geciktirmiyor. Fetch'in bu cycle sonunda
boşalttığı slot da daha önce çalışmış BAC kabulüne geriye dönük kredi olmaz.

## Çok block'lu fetch ve dinamik kimlik

Bir FTQ girdisi bir target grubudur; içinde birden çok `FetchBlock` olabilir.
Engine, block'ları tahmin edilen yürütme sırasıyla işler. Her block için
gereken bellek parçaları farklı sayıda olabilir; block sayısı doğrudan
I-cache istek sayısı değildir. Hazır bir byte aralığı yeniden kullanılabilir.

Asgari değişiklik seçeneği mevcut thread başına tek bekleyen demand isteği
üzerinden ilerler. Bağımsız çoklu I-cache isteği için ek request/retry
yönetimi gerekir. Merge, sıralı ve PC bilgili byte akışıdır; uzak iki block
arasındaki adres boşluğu fetch edilmez. Her instruction gerçek PC'sini
korur. Yarım instruction yalnızca adresçe devam eden parçalardan kurulur;
taken geçişinin iki tarafındaki byte'lar bir instruction olarak birleşmez.

`buildInst()` instruction'ı Fetch'in yerel kuyruğuna zaten ekler. Sonraki
`updatePC()` aynı nesnenin prediction alanlarını doldurur. Fetch tick'inin
sonunda Decode'a gönderim `decodeWidth` ile sınırlıdır. Yeni helper'lar bu
sırayı ve fault sırasını korur.

PC veya FTSeqNum tek başına recovery için yeterli değildir. Dinamik key,
generation, block/CFI konumu ve gerektiğinde InstSeqNum birlikte kullanılır.
Block ve instruction adresli lookup'un seçimi model içinde kalır; bridge
daima bu ortak target/feedback sözleşmesiyle çalışır.

## Feedback ve recovery sınırları

BAC önce Commit squash'ını işler ve döner. Commit squash yoksa kümülatif
commit güncellemesi yapılabilir; ardından Decode squash, ardından Fetch
resteer kontrol edilir. Dolayısıyla aynı BAC tick'inde normal commit update
ile daha genç Decode recovery birlikte gerçekleşebilir.

`doneSeqNum`, normal yayında commit sınırı, squash yayınında recovery
sınırıdır. İkincisi commit eğitimi değildir. Normal commit yayını her branch
için outcome taşımaz; ledger, yürütülen DynInst'in kullandığı tahminle
tutarlı olmalı ve recovery'de düzeltilmelidir. Daha fazla yürütme verisi
isteyen model ayrıca bir feedback bağlantısı gerektirir.

Predecode, CFI'nin gerçek konumunu/tipini ve bilinen statik hedefini ortaya
çıkarabilir. Conditional outcome veya indirect hedefin yürütme sonucunu
vermez. `CFI.isPrediction=false` tek başına bu sonuçların doğrulandığı
anlamına gelmez. Predecode onarımı diyagramlarda Fetch resteer → BAC feedback
yoluyla bir kez uygulanır; eski bir recovery onu geçersiz kılabilir.

mBTB override bir tahmin düzeltmesidir. Etkilenen talimat veya değiştirilecek
kaynak tahmini Fetch'i geçtiyse yalnızca FTQ'yu temizlemek yeterli olmaz.
Yeni O3 arbitration bağlantısı, kaynak instruction kimliğini korumalı,
gerçek execution/trap recovery'siyle önceliklendirmeli ve uygulama sonucunu
bildirmelidir. `ackOverride()` ancak uygulanma veya geçersiz sayılma
kesinleştikten sonra verilir. Kaynak DynInst tahmini güvenli biçimde
güncellenemiyorsa kaynak da replay edilir; bu ek zamanlama maliyeti taşır.

## Sequence çalışmasının ortaya çıkardığı arayüz boşlukları

Bu noktalar sınıf diyagramını değiştirmeden sequence'lerde işaretlendi:

- `BAC::updatePC(inst, pc, ft)` Fetch'in `BlockCursor` bilgisini taşımıyor.
  Cursor erişimi ve onarım sınırını Fetch'e iletecek ortak durum gerekir;
  mevcut boolean dönüş yalnızca predicted-taken bilgisini verir.
- `finishBlock()` dönüşü `void`. Son denetimde bulunan onarım, normal
  tüketimden önce kaydedilmeli ve aynı recovery yoluna yönlendirilmelidir.
- `onTargetConsumed()` için BAC/bridge üzerinden adlandırılmış forwarding
  metodu henüz sınıf sözleşmesinde yok. Payload çıkarılınca bildirim bir kez
  yapılmalı; ledger verisi commit/recovery ve slow kullanıcıları bitene kadar
  korunmalıdır.
- `FTQ::popHead(false)` iki farklı durumdur: legacy history varsa girdiyi
  bırakır; Locked durumda girdiyi çıkarıp yine `false` döner. Temizleme
  yalnızca dönüş değerine dayanamaz.
- `FetchRead.size`, mevcut üç argümanlı `fetchCacheLine()` metoduna otomatik
  bağlanmaz. İlk uygulama hizalı `fetchBufferSize` isteklerini kullanıp
  parçaların geçerli aralıklarını seçebilir.
- Mevcut `FetchComm`, yalnızca squash ve nextPC taşır. Predecode onarımı
  için dinamik recovery sınırı taşıyan ek alan veya sidecar gerekir.
- Mevcut `Fetch::doSquash()` tüm yerel fetchQueue'yu temizler. Sağlam prefix'i
  koruyan yerel predictor düzeltmesi ayrıca uygulanmalıdır.
- `O3OverrideHook` için istek/arbitration yanında asenkron tamamlanma bilgisi
  gerekir. Yeni broadcast, predictor nedenini gerçek branch outcome'dan
  ayırmalı; aynı olay iki kez `onRecovery()` üretmemelidir.
- `nextWakeup()` CPU domain'inde cycle deadline döndürür. CPU bunu event
  tick'ine çevirmeli, boşta kalınca korumalı, reset/recovery/drain sırasında
  yenilemeli veya iptal etmelidir. Aynı clock edge'de iki CPU tick çalışmaz.

## Çizim doğrulaması

Sadece bu belge/diyagramlar değiştirilir; gem5 build'i gerekmez. Yerel
PlantUML ile 12 iş parçacığı kullanarak tüm diyagramları çizmek için:

```bash
JAVA_TOOL_OPTIONS=-Djava.awt.headless=true plantuml \
  -nbthread 12 -charset UTF-8 -failfast2 -tsvg \
  -o /tmp/gem5-bpu-sequences research/uml/sequence/*.puml
```

Komut depo kökünden çalıştırılır. SVG çıktıları kaynak dizininin dışında
kalır; her `.puml` dosyası ayrı bir diyagram üretir.
