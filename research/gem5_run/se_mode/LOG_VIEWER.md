# Telefonda girişsiz log izleme

`log_viewer.py`, JSON listesinde açıkça seçtiğin dosyaların son kısmını
gösteren küçük bir servistir. Python 3 dışında uygulama bağımlılığı yoktur.
Telefonda dosya seçimi, elle yenileme, 10 saniyede otomatik yenileme ve son
satırı takip etme bulunur. Log HTML sayfasına sunucuda yerleştirilir;
görüntüleme ve yenileme için JavaScript/fetch gerekmez. Dosya seçimi
JavaScript varsa otomatik, yoksa "Görüntüle / Yenile" düğmesiyle uygulanır.
Otomatik yenileme tam sayfayı yeniden yükler; kapatmak için kutuyu kaldırıp
uygula. Metin seçip kopyalayacağında otomatik yenilemeyi kapatabilirsin.

```text
Telefon -- HTTPS --> Cloudflare Tunnel veya Caddy
                               |
                        127.0.0.1:8765
                               |
                      seçilmiş log dosyaları
```

Servis sadece localhost dinler. İnternete girişsiz açıldığında bağlantıya
ulaşan herkes listedeki dosyaları okuyabilir; HTTPS bunu gizli yapmaz.
Dosyaları listeye eklemek bu dosyaların tamamının kamuya açıklanmasına izin
vermektir. Şu an son kısmı gösterilse de zamanla bütün içerik okunabilir.
Log içindeki kullanıcı adı, yol, komut veya token otomatik sansürlenmez.

## 1. Yayımlanacak dosyaları iliştir

Repo kökünde örnek yapılandırmayı kişisel sonuç dizinine kopyala:

```bash
mkdir -p research-result/monitor
cp -n research/gem5_run/se_mode/examples/public_logs.json \
  research-result/monitor/public_logs.json
```

`research-result/monitor/public_logs.json` dosyasını düzenle:

```json
{
  "logs": [
    {
      "id": "batch",
      "label": "train_reduced_retry01.log",
      "path": "research-result/logs/train_reduced_retry01.log"
    },
    {
      "id": "report",
      "label": "Batch durum raporu",
      "path": "research-result/batch_results/train_reduced_retry01/batch_report.md"
    }
  ]
}
```

Göreli `path` değerleri repo köküne göredir; mutlak yol da kullanılabilir.
URL'de dosya yolu bulunmaz, sadece `batch` gibi bir ID bulunur. Dosyanın
henüz oluşmamış olması servis başlangıcını engellemez. Başka dosyayı
iliştirmek için yeni bir kayıt ekleyip sadece viewer servisini yeniden
başlat; aşağıdaki [yeniden başlatma adımlarını](#viewer-restart) uygula.
Wildcard veya dizin yayını yoktur.
Dosyaları kopyalamak veya symlink oluşturmak gerekmez. Son yol bileşeninde
symlink olan dosyalar reddedilir; gerçek dosya yolunu kullan.

## 2. Viewer'ı VDS'de başlat

```bash
nohup setsid python3 -u research/gem5_run/se_mode/log_viewer.py \
  --catalog research-result/monitor/public_logs.json \
  --port 8765 \
  </dev/null >>research-result/monitor/viewer.log 2>&1 &
```

Sunucuda kontrol et:

```bash
curl --fail http://127.0.0.1:8765/api/logs
```

Bu komut dosya ID'lerini ve etiketlerini listeler. Arayüz localhost'ta
`http://127.0.0.1:8765/` adresindedir; telefondaki localhost bu VDS değildir.
Telefondan erişim için aşağıdaki HTTPS yöntemlerinden birini kullan.

<a id="viewer-restart"></a>

### JSON değişikliğinden sonra viewer'ı yeniden başlat

`public_logs.json` yalnızca başlangıçta okunur. Kayıt ekleme/silme, `path`
veya `label` değişikliği için viewer'ı yeniden başlatmak gerekir. Mevcut log
dosyasına yeni satırlar yazılması için yeniden başlatma gerekmez; sayfayı
yenilemek yeterlidir.

Aşağıdaki komutları repo kökünde, viewer'ı başlatan kullanıcıyla çalıştır.
Önce yeni yapılandırmayı çalışan servisi durdurmadan kontrol et:

```bash
python3 research/gem5_run/se_mode/log_viewer.py \
  --catalog research-result/monitor/public_logs.json --check
```

JSON/yapılandırma hatası varsa önce düzelt. Henüz oluşmamış bir log için
`file_not_found` görülmesi viewer'ın başlamasına engel değildir.

Çalışan viewer'ın PID'sini ve komutunu bul:

```bash
pgrep -u "$(id -u)" -af '[l]og_viewer.py'
```

Listede `--catalog research-result/monitor/public_logs.json` ve `--port 8765`
ile çalışan satırı seç. Birden fazla viewer varsa yalnızca ilgili olanı
durdur. Aşağıdaki iki komutta `PID` yerine o satırın başındaki sayıyı yaz:

```bash
kill -TERM PID
ps -p PID -o pid=,args=
```

`ps` artık süreç göstermediğinde viewer kapanmıştır. Hâlâ gösteriyorsa kısa
bir süre sonra tekrar kontrol et; kapanmadan ikinci viewer başlatma.
Ardından güncel JSON ile yeniden başlat:

```bash
nohup setsid python3 -u research/gem5_run/se_mode/log_viewer.py \
  --catalog research-result/monitor/public_logs.json \
  --port 8080 \
  </dev/null >>research-result/monitor/viewer.log 2>&1 &
```

```bash
curl --fail http://127.0.0.1:8765/api/logs
```

Yanıtta yeni ID/etiketleri doğrula ve telefondaki sayfayı yenile. Servis
yanıt vermiyorsa `tail -n 50 research-result/monitor/viewer.log` ile hatayı
incele. Bu işlem gem5 simülasyonlarını veya Cloudflare tünelini durdurmaz;
çalışan tünelin adresi aynı kalır. `pkill python` veya porta göre toplu
sonlandırma kullanma.

## 3. En az kurulum: geçici Cloudflare bağlantısı

Domain veya Cloudflare hesabı gerektirmeyen Quick Tunnel ile başlayabilirsin.
VDS'ye `cloudflared` kurulumunu
[resmî indirme sayfasından](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/downloads/)
işletim sistemine uygun şekilde yap. Ardından:

```bash
nohup setsid cloudflared tunnel --url http://127.0.0.1:8765 \
  </dev/null >>research-result/monitor/tunnel.log 2>&1 &

tail -f research-result/monitor/tunnel.log
```

Logda çıkan `https://...trycloudflare.com` bağlantısını telefonda aç.
Sayfa giriş istemez. `tail` izlerken Ctrl+C sadece log izleyicisini kapatır.
Viewer ve tünel simülasyonlardan bağımsızdır; kapanmaları gem5'i durdurmaz.

Bu yöntemde 8765 portunu güvenlik duvarından açma. Tünel dışarıya bağlanır;
HTTPS bağlantısını Cloudflare sonlandırır ve seçtiğin içerik Cloudflare
üzerinden geçer. Rastgele URL bir erişim yetkisi veya parola değildir.

Quick Tunnel geçici test/geliştirme içindir: yeniden başlatılınca adres
değişir ve uptime garantisi yoktur. Sabit adres gerektiğinde domain ile
kalıcı Cloudflare Tunnel veya aşağıdaki Caddy seçeneğini kullan.
[Quick Tunnel dokümanı](https://developers.cloudflare.com/tunnel/get-started/quick-tunnels/)

## 4. Sabit adres alternatifi: domain + Caddy

Domain'in varsa örneğin `logs.example.com` DNS kaydını VDS'ye yönlendir.
Mevcut Caddy yapılandırmana şu siteyi ekle:

```caddyfile
logs.example.com {
    reverse_proxy 127.0.0.1:8765
}
```

Caddy kurulumu ve reload işlemini sunucundaki mevcut servis düzenine göre
yap. Bu yöntem için Caddy'ye 80/443 erişimi gerekir; 8765 localhost'ta kalır.
Caddy domain için HTTPS sertifikasını yönetebilir.
[Resmî HTTPS reverse proxy açıklaması](https://caddyserver.com/docs/quick-starts/reverse-proxy)

## Dosya var ama arayüz okuyamıyorsa

Viewer'ı çalıştıran aynı kullanıcıyla, repo kökünden kontrol et:

```bash
python3 research/gem5_run/se_mode/log_viewer.py \
  --catalog research-result/monitor/public_logs.json --check

curl -i 'http://127.0.0.1:8765/api/log?id=batch'
curl -i 'http://127.0.0.1:8765/api/log?id=report'
```

`--check` web sunucusu açmaz; her ID için gerçekten kullanılan dosya yolunu,
okunabilirlik durumunu ve varsa hatayı yerel terminale yazar. Göreli yollar
yapılandırma dosyasının dizinine değil repo köküne göredir. Bir dosya
okunamıyorsa dönüş kodu `1`, hepsi okunabiliyorsa `0` olur.

Arayüz hataları birbirinden ayrılır:

- `404 [file_not_found]`: viewer'ın yapılandırmasındaki yol bulunamadı.
- `403 [permission_denied]`: servis kullanıcısının dosya/dizin izni yetersiz.
- `422 [symlink_rejected]`: gerçek dosya yolu yerine symlink seçildi.
- `422 [not_regular_file]`: dizin, FIFO veya cihaz seçildi.
- `404 [unknown_log]`: istenen ID çalışan servisin listesinde yok.
- `HTTP 502` veya JSON yanıtı alınamaması: proxy/tünel yanıtını kontrol et.
- Ağ hatası veya zaman aşımı: istek tamamlanamadı; dosyanın yokluğunu göstermez.

Public API yerel dosya yollarını hata yanıtına koymaz. `--check` çıktısında
görünen yollar yönetici teşhisi içindir.

Localhost ve public HTTPS `/api/log?id=batch` istekleri doğru içerikle
HTTP 200 dönüyorsa dosya bu istekler sırasında okunabiliyordur. Eski
JavaScript istemcisi tarayıcıdaki tüm TypeError hatalarını ağ hatası olarak
gösterebiliyordu. Güncel sayfa bu isteği tarayıcıdan yapmaz; logu sunucu
tarafında HTML içine güvenli biçimde koyar. Dosya hataları sayfada doğrudan
gösterilir. "Ham yanıtı aç" bağlantısı API yanıtıyla karşılaştırma sağlar.

Kod güncellendiğinde `log_viewer.py` ile `log_viewer_assets` dizinini birlikte
aktar ve yalnızca viewer'ı yeniden başlat. Catalog değişiklikleri de yeniden
başlatma gerektirir; [adımlar yukarıda](#viewer-restart). HTML/JS her istekte
diskten okunur; telefonda sayfayı tamamen yeniden yükle. gem5 ve
çalışan tünelin bu güncelleme için yeniden başlatılması gerekmez.

## Güvenlik sınırları ve kullanım

- Uygulamada dosya yükleme, dosya değiştirme veya komut çalıştırma yoktur.
  İnternetten bir dosya yolunu değiştirerek başka dosya okutulamaz.
- Loglar HTML/Markdown olarak işlenmez, düz metin gösterilir. Liste ve log
  cevapları cache'lenmez; arama motorlarına indekslememe başlığı verilir.
  Bu başlıkların erişim kontrolü olmadığını unutma.
- Her istek varsayılan en fazla son 64 KiB'ı okur. `--tail-bytes 262144`
  ile en fazla 256 KiB seçilebilir. Aynı anda en fazla 16 bağlantı işlenir.
  Bu sınırlar uygulamanın yükünü azaltır; DDoS koruması garantisi değildir.
  Public trafik için HTTPS proxy/tünel katmanını kullan.
- FIFO, cihaz ve son bileşeni symlink olan dosyalar yayımlanmaz. Yapılandırma
  ve dosyaların üst dizinleri güvenilir sunucu kullanıcısının kontrolünde
  kalmalıdır. Servisi root olarak çalıştırmak gerekmez.
- Servisi durdurmak için `pgrep -af '[l]og_viewer.py'` ile PID'yi belirle,
  ardından yalnızca o PID'ye `kill -TERM PID` gönder. Tünel için de
  `pgrep -af '[c]loudflared tunnel'` ile aynı yaklaşımı kullan. gem5'e veya
  bütün Python süreçlerine topluca sinyal gönderme.
- `nohup`/`setsid` terminal kopmasına karşıdır; VDS reboot sonrasında bu
  süreçleri yeniden başlatmak gerekir. Kalıcı kullanımda aynı komutlar
  ayrı systemd servisleri olarak işletilebilir.
- Bir logun güncellenmemesi gem5'in durduğunu kanıtlamaz. BBV profiling uzun
  süre sessiz olabilir. Batch raporu da en son kaydedilen durumu gösterir.

Bu repodaki uygulama ve testler yerel hazırlıktır. VDS'de viewer ve tünel
başlatılmadan kullanılabilir bir internet adresi oluşmaz.
