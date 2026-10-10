# IBPU üzerinden gem5 entegrasyonu: V2

[Sınıf diyagramı](class_diagram_v2.puml) iki görünüm içerir: bileşenler ve
sahiplik; ortak veri/feedback sözleşmeleri. Bu bir tasarım önerisidir;
mevcut gem5 kaynakları ve [ilk diyagram](class_diagram.puml) değiştirilmedi.
İncelenen checkout: `f6026859cd1d258e2060762d470c3e47ef385d13`.

**Evet: IBPU'nun ürettiği hedef FTQ'ya konup mevcut O3 hattında devam
edilebilir.** Bunun için hedefin blok listesini anlayan küçük bir fetch
yardımcısı ve BAC içinde feedback adaptörü gerekir. Yalnızca `insert()`
çağrısı eklemek yeterli değildir: mevcut hedef tek aralıktır, tek branch
history'si taşır ve fetch ilk taken branch'te durur.

## 1. Seçim sınırı IBPU'dur

`SingleTakenLineBPU`, `TwoTakenBPU`, `CustomBPU` bağımsız implementasyonlardır.
Her biri kendi BTB'lerini, sorgu adreslemesini, direction predictor'ını,
RAS/indirect predictor'ını ve pipeline zamanlamasını seçer. BAC veya FTQ
hangi tabloların bulunduğunu bilmez. İki taken desteği, sırayla yürütülen
iki taken geçişi tahmin etmek anlamındadır; iki alternatif yolu birlikte
yürütmek değildir.

`IBTB` ve `BufferedBPU` ortak işlevleri yeniden kullanmak için isteğe bağlı
yardımcılardır. Bütün BPU'lar uBTB+mBTB topolojisine zorlanmaz. İstenirse
başka bir model doğrudan `IBPU` implement eder. gem5 `BPredUnit` public
predict/update/squash fonksiyonları virtual olmadığından bu arayüz ayrı
kurulur; mevcut `BranchPredictor` sınıfını türetmek tek başına yeterli olmaz.

Önerilen seçim; henüz mevcut bir config API'si değildir:

```python
cpu.decoupledFrontEnd = True
cpu.bpuModel = TwoTakenBPU(btb=TwoTakenBTB(...))
# Alternatif: SingleTakenLineBPU(btb=SingleLineBTB(...))
# Alternatif: CustomBPU(ubtb=UBTB(...), mbtb=MBTB(...))
```

`bpuModel=NULL` eski BAC/branchPred yolunu korur. Yeni modda prediction,
history ve training sahibi IBPU'dur; eski BPredUnit aynı branch için ayrıca
çalıştırılmaz. Legacy BPredUnit kullanmak isteyen model kendi adaptörünü
sağlar. Python yalnızca başlangıç seçimi yapar; cycle başına çağrılmaz.

## 2. FetchTarget'ın anlamı

İlk diyagramdaki çok bloklu hedefe `BpuFetchTarget` adı verildi. Böylece
gem5'in mevcut `gem5::o3::FetchTarget` sınıfıyla isim karışıklığı önlenir.
Mevcut sınıfa bu payload'ın bir handle'ı eklenir; FTQ'nun
`list<FetchTargetPtr>`, insert/read/pop ve probe mekanizmaları korunur.

Bir FTQ kaydı bir `BpuFetchTarget` taşır. İçindeki `blocks[]` tahmini yürütme
sırasındadır. Örneğin şu hedef, farklı adreslerde iki taken geçiş içerir:

```text
FT #42
  block[0]: [0x1000, 0x1010), CFI @0x100c taken -> 0x8000
  block[1]: [0x8000, 0x8010), CFI @0x800c taken -> 0x3000
sonraki hedefin fetch başlangıcı: 0x3000
```

Bu örnekte CFI'lar 4 byte'tır. `0x1010..0x7fff` aralığı fetch edilmez.
Blok sayısı otomatik olarak taken sayısı + 1 değildir; son taken'ın hedef
bloğu sonraki FT'de yer alabilir. Birden fazla not-taken CFI aynı blokta
bulunabilir; history her CFI için ayrı eşleşir.

`nextQueryPC` BPU sorgu cursor'ıdır; fetch adresi blokların `startPC/nextPC`
alanlarından gelir. CFI adreslemesi aradaki normal instruction'ları atlamaz.
`limitExclusive` byte sınırıdır; mevcut inclusive `endPC` ile aynı anlama
gelmez. Yeni modda aktif bloğun API'si kullanılır; bütün payload için
`min(start)..max(end)` aralığı oluşturulmaz.

## 3. Blok sayısı kadar fetch ve merge

Yeni `BlockFetchEngine`, Fetch'in sahibi olduğu bir C++ yardımcı sınıfıdır.
Kendi memory portunu veya instruction decoder'ını kurmaz:

1. FTQ başındaki hedef için block cursor başlatır.
2. Her bloğun gerekli byte parçalarını belirler. Fetch, mevcut
   `fetchCacheLine -> translateTiming -> ICachePort` yoluyla bunları ister.
3. Yanıtlar `predictionId/blockIndex/fragmentIndex/requestId` ile eşleşir.
   Byte'lar PC bilgili `MergedFetchStream` parçalarına yerleştirilir.
4. Hazır yürütme prefix'i mevcut ISA decoder'ına, gerçek PC'siyle verilir.
   `buildInst`, `fetchQueue` ve `FetchStruct -> Decode` yolu kullanılır.
5. Blok tamamlanınca cursor ilerler. FTQ kaydı ancak bütün hedef tüketilince
   veya recovery ile iptal edilince bırakılır.

Merge, farklı adreslerdeki byte'ları tek sanal kesintisiz aralığa dönüştürmez.
Yukarıdaki örnekte instruction sırası `0x1000...0x100c, 0x8000...0x800c`
olur; her DynInst kendi gerçek PC'sini taşır. Tüm hedefin tamamlanmasını
beklemek zorunlu değildir; hazır prefix tüketilebilir. Bütün blokları
bekleten atomik bir merge gerekiyorsa bu ayrıca modellenen bir politikadır.

**Minimum kaynak değişikliği için tek outstanding demand isteği/thread
korunur.** Bloklar farklı cycle'larda alınabilir ve aynı sıralı akışta
birleşir. Bu, çok bloklu hedef desteğidir; aynı cycle'da iki cache line
okunduğu iddiası değildir. Gerçek paralel istek desteği için aşağıdaki
ek değişiklikler gerekir.

Mantıksal blok sayısı ile I-cache transaction sayısı her zaman eşit değildir:
blok cache/fetch-buffer/page sınırından bölünebilir veya gerekli byte'lar
buffer'da bulunabilir. Request sayısı yerine bütün blokların gerekli
byte'larının sağlanması garanti edilir. Aynı adres tekrar ziyaret edilirse
byte reuse mümkün olsa bile yeni dinamik instruction/history örneği oluşur.

RVC gibi değişken uzunluklarda contiguous sınırı aşan instruction, sonraki
byte parçasıyla tamamlanır. Decoder'ın hesapladığı gerçek sequential PC
korunur; sonraki blok başında daha önce tüketilmiş byte'lar tekrar decode
edilmez. Tahmin edilen sınır/CFI bu PC ile bağdaşmıyorsa predecode recovery
uygulanır. Taken geçişinde yarım instruction byte'ları hedefle birleştirilmez.
Fault'lar kendi PC/blok sırasıyla mevcut fault taşıyan DynInst yoluna girer;
genç veya iptal edilmiş bir blok fault'u daha yaşlı akışın önüne geçirilmez.

## 4. IBPU bağlantılarının sözleşmesi

| İşlem | Anlamı |
| --- | --- |
| `tick(now, credits)` | Pipeline ve olayları ilerletir; FTQ-full olsa da kontrol olaylarını işler. |
| `canSupply / peekFetchTarget` | Hazır ve geçerli baş sonucu görür; tekrarlanan peek history'yi değiştirmez. |
| `acceptFetchTarget` | Başarılı FTQ insert'inden sonra sonuç queue'dan bırakılır. |
| `onPredecodeFeedback` | Gerçek StaticInst/PC ile CFI token'ını InstSeqNum'a bağlar; fetch'in kullanacağı prediction'ı döndürür. |
| `onCommitUpdate` | Var olan cumulative `doneSeqNum` sınırına kadar training/retirement yapar. |
| `onRecovery` | Decode/execute misprediction, fetch repair, trap gibi nedenleri ayrı işleyip doğru suffix'i geri alır. |
| `peekOverride / ackOverride` | Geç predictor düzeltmesini FTQ alanı beklemeden sunar; uygulandıktan sonra onaylanır. |
| `onTargetConsumed` | Fetch tüketimini bildirir; history'nin commit veya slow-stage tamamlanmasından önce silinmesi anlamına gelmez. |
| `beginDrain / resume / isDrained / nextWakeup` | Pending event/history ve CPU uyku/uyanma lifecycle'ını kapsar. |

`PredictionLedger` modelin içinde prediction/CFI -> InstSeqNum eşlemesini
tutar. Kaynak aynı PC olsa bile block index ve dinamik kimlikler korunur.
Predecode conditional branch'in gerçek direction'ını veya indirect
target'ını bilemez; sadece instruction metadata'sını bildirir. Tahminde
bulunmayan branch için de bir token oluşturulur. İptal edilenlerin dışında
commit'e ulaşan branch'in son kullanılan prediction'ı, varsa recovery ile
düzeltilmiş sonucu ledger'dan alınır. Böylece normal training için Commit
içine yeni instruction başına callback koymak gerekmez.

`HistoryHandle` opaque'tur: GHR/PHR yanında RAS, indirect state, folded
history ve predictor metadata'sının restore politikasını model belirler.
FTQ pop history'yi yok etmez. Rollback dinamik yaş sırasının tersinde yapılır;
henüz FTQ'ya yayınlanmamış genç state de kapsanır. Bekleyen forecast cache'i
donanım kapasitesine eklenmez. `readyCycle: Cycles` ile gem5 `Tick` karışmaz;
CPU event planlamasında clock-domain dönüşümü açık yapılır.

## 5. Mevcut gem5 kaynaklarında gerekli değişiklikler

| Dosya / bağlantı | Değişiklik |
| --- | --- |
| [BaseO3CPU.py](../../src/cpu/o3/BaseO3CPU.py) | Opsiyonel `bpuModel` parametresi; block/taken fetch bütçeleri. Legacy config korunur. |
| [bac.hh](../../src/cpu/o3/bac.hh), [bac.cc](../../src/cpu/o3/bac.cc) | `Gem5BpuBridge` sahibi olur. Yeni modda `generateFetchTargets` yerine ready sonuç admission; `updatePC/updatePreDecode` üzerinden CFI binding ve kullanılan prediction; mevcut commit/decode/fetch feedback'ini IBPU'ya iletme. Reset/drain/status da modele yönlenir. |
| [ftq.hh](../../src/cpu/o3/ftq.hh), [ftq.cc](../../src/cpu/o3/ftq.cc) | FetchTarget'a çok bloklu payload; FTQ kaydını son bloktan sonra pop; IBPU modunda legacy `bpuHistory` kontrolleri yerine consumed/repair kontrolü; early override için selective squash/suffix replacement. |
| [fetch.hh](../../src/cpu/o3/fetch.hh), [fetch.cc](../../src/cpu/o3/fetch.cc) | `BlockFetchEngine`, aktif block cursor ve PC bilgili buffer. Request completion/translation/retry/fault/squash bağları; bloklar arasında yürütme sırasıyla decode; taken sonrası devam ve hedef bitiş mantığı. |
| [o3/SConscript](../../src/cpu/o3/SConscript), [pred/SConscript](../../src/cpu/pred/SConscript) | Yeni helper kaynakları ve IBPU SimObject/model kayıtları. |
| [fdp.cc](../../src/mem/cache/prefetch/fdp.cc), gerektiğinde [fdp.hh](../../src/mem/cache/prefetch/fdp.hh) | FDP kullanılıyorsa prefetch aralıklarını her bloktan al; disjoint bloklar arasını prefetch etme. Suffix iptal/güncellemesini block kimliğiyle ilişkilendir. |

Yeni dosya önerisi: `cpu/pred/DecoupledBPU.py`, `ibpu.hh`,
`bpu_types.hh`, model implementasyonları ve isteğe bağlı ortak history/queue
yardımcıları; `cpu/o3/gem5_bpu_bridge.hh/.cc`,
`cpu/o3/block_fetch_engine.hh/.cc`. Diyagramdaki küçük value type'lar ayrı
ayrı dosya veya SimObject olmak zorunda değildir.

Mevcut kodda özellikle şu noktalar değişmeden bırakılırsa tasarım işlemez:

- [bac.cc:519](../../src/cpu/o3/bac.cc): model tick'ini yalnızca `Running`
  durumuna bağlamak, FTQ-full altında slow sonuçları/override'ı dondurur.
- [bac.cc:788](../../src/cpu/o3/bac.cc): yalnızca çıkış branch'inden history
  almak, blok içi çoklu CFI'yı kapsamaz. Phantom/missing CFI ve block sonu
  eşleşmeyen token'lar da `finishBlock` ile denetlenmelidir.
- [fetch.cc:1196](../../src/cpu/o3/fetch.cc): `!predictedBranch` döngü şartı
  ilk taken'da durur. Yeni modda geçerli sonraki blok, byte hazırlığı ve
  bütçe varsa devam edilmeli; toplam `fetchWidth` artırılmamalıdır.
- [fetch.cc:1317](../../src/cpu/o3/fetch.cc): tek aralık `inRange` ve FT pop
  davranışı, aktif block/whole target ayrımını kullanmalıdır. Geriye veya
  aynı adrese taken durumunda yalnızca sayısal aralık kontrolü yetmez.
- [fetch.cc:372](../../src/cpu/o3/fetch.cc): tek buffer'a kopyalanan yanıt,
  doğru block/fragment'e bağlanmalı; eski yanıt yeni hedefi doldurmamalıdır.

FTQ slot birimi artık hedef grubudur. `maxFTPerCycle` grup admission
bütçesi olarak tanımlanabilir; block fetch, taken fetch, BPU issue ve
`fetchWidth/decodeWidth` ayrı bütçelerdir. İki taken BPU seçmek memory
portu veya instruction genişliğini kendiliğinden artırmaz. Modelin slot
serbest bırakma noktası fetch'ten geçse ledger ayrıca donanım kredi
rezervasyonu tutar. Aynı paket iki kez kapasiteye sayılmaz.

## 6. Kaynak değişikliğini büyüten iki durum

**Eşzamanlı block fetch:** Mevcut `memReq[tid]`, tek `fetchBuffer` ve
`retryPkt/retryTid` çoklu outstanding isteği temsil etmez. Paralel modelde
request tablosu, bounded response/merge buffer, translation callback'leri,
retry kuyruğu, geç yanıt iptali ve fault sıralaması genişletilir.
`maxOutstandingFetchRequests`, istek/cycle ve byte/cycle limitleri açık
modellenir. Cache'in gerçek kabul bant genişliği de konfigürasyonda
eşleşmelidir. Sadece `fetchCacheLine()` fonksiyonunu N kez çağırmak olmaz.

**Yayınlanmış yol için geç mBTB override:** Henüz FTQ/buffer içinde kalan
suffix bridge+FTQ+fetch yardımıyla düzeltilir. Fetch edilmiş fakat decode'a
gönderilmemiş genç DynInst'ler fetchQueue'dan da kaldırılır. Daha ileri
gitmiş instruction varsa [comm.hh](../../src/cpu/o3/comm.hh) ve mevcut O3
squash dağıtımına, tipik olarak [commit.cc](../../src/cpu/o3/commit.cc)
üzerinden, açık bir override isteği eklenir; ilgili header da güncellenir.
Normal decode/execute recovery'siyle dinamik yaş/geçerlilik arbitration'ı
yapılır. Rename/IEW/ROB algoritmalarını değiştirmek yerine mevcut squash
yolu kullanılır; yeni neden gerçek branch sonucu gibi training yapmaz.
Mevcut `Fetch::bacResteer` fetch'ten BAC'a çalışır ve bu yolun yerine geçmez.

Override kaynağının kendisi daha önce DynInst olmuşsa gerekiyorsa source
instruction da refetch edilir veya kullanılan prediction alanları tutarlı
güncellenir. Sadece gençleri temizleyip source'un eski `predPC` değerini
bırakmak aynı hatayı yeniden tetikleyebilir. Önceden çözülmüş gerçek sonuç
geç predictor tahminiyle ezilmez. İptal yalnızca etkilenen suffix'e uygulanır;
hayatta kalan prefix'in history ve memory cevapları korunur.

Erken uBTB yayınlayan modelde bu recovery zorunludur. Sonuçları mBTB
doğrulamasına kadar yayınlamamak daha küçük bir başlangıç implementasyonu
sağlar, fakat yanlış yol fetch/cache/backend etkilerini ölçmez; ayrı bir
model olarak adlandırılmalıdır. Normal çok bloklu fetch+merge için
Decode/Rename/IEW'nin instruction taşıma arayüzünü değiştirmek gerekmez.

## 7. Uygulama ve doğrulama sırası

1. `bpuModel=NULL` baseline'ı koruyarak IBPU ve bridge ekle; tek blokla
   aynı instruction akışını ve feedback lifecycle'ını doğrula.
2. Çok bloklu payload, sıralı fragment fetch ve PC bilgili merge ekle.
   İlk taken'dan sonra ikinci bloğun gerçek PC ile devam ettiğini doğrula.
3. Çoklu CFI token'ları, backend recovery, BTB miss/phantom CFI ve
   FTQ-full sırasında pending event işleyişini doğrula.
4. Erken yayınlı modeller için selective/late override desteğini tamamla.
5. Gerekliyse çoklu outstanding fetch'i ayrıca ekle ve timing'i doğrula.

Asgari senaryolar: iki uzak blok ve iki taken; aynı bloğa dönüş; aynı blokta
iki not-taken CFI; page/cache sınırı ve RVC instruction; ikinci blokta
fault; FTQ-full altında override; kısmen tüketilmiş hedefte correction;
commit ile çakışan geç sonuç; response sonrası squash; drain/resume.
Bir grup `fetchWidth` kadar instruction bütçesini her blokta yeniden alamaz.

Bu teslim yalnızca PUML/Markdown içerir. PlantUML kontrolü/render ve
whitespace kontrolü yeterlidir; gem5 build/simülasyon yapılmaz.
Implementasyon başladığında derlemeler `-j12` ile çalıştırılmalıdır.
