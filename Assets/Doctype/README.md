# Doctype

HTML/CSS arayüzleri Unity'de **GPU'da** render eden bir sistem. Layout'u
[litehtml](https://github.com/litehtml/litehtml) yapıyor, çizimin tamamını Unity
üstleniyor, arada CPU rasterizer yok.

Ultralight/CEF gibi çözümlerden farkı: gömülü bir tarayıcı yok. litehtml saf C++
ve **hiçbir şey çizmiyor**; `document_container` arayüzü üzerinden "şu dikdörtgeni
şu renkle doldur", "şu glyph'i şuraya koy" gibi çağrılar gönderiyor. Bu sistem o
çağrıları düz bir quad akışına çevirip tek bir mesh olarak GPU'ya veriyor.

Pratik sonucu: **iOS ve Android dahil her yere derlenir** (Ultralight'ta olmayan),
sayfa başına **tek draw call**, ve binary maliyeti ~1 MB civarı.

---

## Mimari

```
HtmlView.LoadHtml(str)  ──► document::createFromString()      [C++]
           .Layout(width)   ──► document::render(max_width)
           .Record()        ──► document::draw()
                                     │
                                lhu::Container : document_container
                                  • create_font/text_width → stb_truetype + R8 atlas
                                  • draw_*  → LhuQuad[] (düz POD dizi)
                                     │
           ◄──── LhuQuad* + atlas pointer'ları (kopyasız) ─────────┘
           │
   HtmlMeshBuilder → tek dinamik Mesh (8 UV kanalı = quad parametreleri)
           │
   HtmlRenderer → CommandBuffer → RenderTexture
                      shader: Doctype/Quad
```

### Neden tek draw call

Yuvarlak köşe, kenarlık halkası, gradient ve kırpma **CPU'da rasterize
edilmiyor**. Her biri shader'da analitik olarak (signed distance field ile)
yeniden kuruluyor. Quad başına parametreler vertex'in 8 UV kanalında taşınıyor,
dolayısıyla tüm sayfa tek materyal + tek mesh ile çiziliyor:

| Quad tipi | Shader'da nasıl çiziliyor |
|---|---|
| `Rect` | Köşe başına eliptik yarıçaplı yuvarlak dikdörtgen SDF |
| `Border` | Dış SDF ∩ iç SDF (halka) + miter kama maskesi |
| `Glyph` | R8 atlastan örnekleme, vertex rengiyle tint |
| `Image` | RGBA atlastan örnekleme, yuvarlak köşe maskesi |
| `Linear/Radial/ConicGradient` | 256 px'lik LUT dokusundan, `t` fragment'te hesaplanıyor |

**Miter kama numarası:** bir noktanın hangi kenarlığa ait olduğu, dört kenardan
normalize edilmiş nüfuz derinliklerinin (`x/borderLeft`, `y/borderTop`, …) en
küçüğüne bakılarak bulunuyor. Bu, köşegenlerle bölmeye matematiksel olarak denk
ama tek bir `min` işlemi.

---

## Kurulum ve derleme

### UPM ile (önerilen)

Unity 6000.0+, Package Manager, "Add package from git URL":

```
https://github.com/DilaverSerif/doctype-unity.git?path=Assets/Doctype#v0.2.0
```

macOS (universal), Android (arm64-v8a) ve iOS native plugin'leri paketle
birlikte hazır geliyor; derlemeniz gereken bir şey yok. `#v0.2.0` ekini
kaldırırsanız release yerine master'ı takip edersiniz. Aşağıdaki derleme
adımları yalnız native tarafı değiştirenler için.

### macOS (bu makinede doğrulandı)

```bash
./Native/build_macos.sh
```

Universal (arm64 + x86_64) `.bundle` üretip
`Assets/Doctype/Plugins/macOS/` altına koyar. Tek gereksinim Xcode,
CMake gerekmiyor.

### Android (bu makinede derlendi, cihazda ölçüldü)

```bash
./Native/build_android.sh
```

Gereksinim: Unity'nin Android Build Support modülü. NDK oradan bulunuyor;
`cmake` + `ninja` da Unity'nin Android SDK'sıyla geliyor
(`PlaybackEngines/AndroidPlayer/SDK/cmake/<sürüm>/bin` PATH'e eklenmeli,
yoksa `brew install cmake ninja`). Çıktı
`Assets/Doctype/Plugins/Android/libs/arm64-v8a/libDoctype.so`; kök
README'deki bütün telefon ölçümleri bu kütüphaneyle alındı.

### iOS

`Native/CMakeLists.txt` iOS için de kullanılabilir; statik kütüphane olarak
derleyip Xcode projesine ekleyin. `HtmlNative.Lib` iOS'ta zaten
`__Internal`'a çözülüyor.

---

## Kullanım

```csharp
var view = gameObject.AddComponent<HtmlView>();
view.LoadHtml("<body style='font-family:sans-serif'><h1>Merhaba</h1></body>");
// view.Texture -> RenderTexture

view.SetText("#score", "1280");                   // parse yok, artımlı
view.SetStyle("#bar", "width:64%");               // parse yok, artımlı
```

Üç giriş noktasının kontratı — yanlış olanı kullanmak, ölçülen performansı
kaybetmenin en kolay yolu:

| Çağrı | Ne için | Maliyet |
|---|---|---|
| `LoadHtml` | Yapısal/doküman değişiklikleri: yeni ekran, farklı markup | Tam parse + layout; telefonda ~12 ms CPU. Asla kare başına çağırmayın. |
| `SetText` | Çalışma zamanı metin mutasyonu: skor, sayaç, isim | Artımlı; ~1 ms CPU + kısmi redraw |
| `SetStyle` | Çalışma zamanı görsel/layout mutasyonu: renk, konum, bar genişliği | Artımlı; ~1 ms CPU + kısmi redraw |

uGUI ile:

```
RawImage + HtmlRawImage + HtmlView
```

`HtmlRawImage` pointer olaylarını belge koordinatlarına çevirip iletir;
`:hover` CSS'i, `<a>` tıklaması ve scroll çalışır.

#### Birden fazla panel arasında sürükleme

Envanter bir yüzey, hotbar başka bir yüzey olabilir. uGUI bir sürüklemenin
tamamını **başladığı** nesneye yollar, yani hotbar kendi başına bırakmayı hiç
duymaz. `HtmlDrag.View` bunu çözer: her probe, parmağın altındaki **tüm
Doctype yüzeylerine** bakar (EventSystem sıralamasıyla, yani canvas sorting
order ve pass-through filtresi bedava gelir) ve gerçekten üstte olan sayfayı
bildirir. `ItemDropped` hangi belgede hangi elemana düştüğünü söyler.

Bu, HUD'u tek bir ekran boyu yüzey yerine birkaç küçük panele bölmeyi mümkün
kılar: her yüzey yalnızca kendi paneli kadar, aradaki boşlukta hiçbir şey yok.

#### Ekranı kaplayan ama doldurmayan HUD'lar

Yüzey ekranı kaplasa bile sayfanın her yerini boyaması gerekmez. `Pass Through
Empty Areas` açıkken **yalnızca `id`'si olan elemanlar** dokunuşu yakalar; boş
sayfaya düşen dokunuş arkadaki oyuna geçer. Dekoratif sarmalayıcılara `id`
vermeyin, tıklanabilir kutulara verin.

Şeffaf sayfa için `Background` **(0,0,0,0)** olmalı, çünkü yüzey `Blend One
OneMinusSrcAlpha` ile birleşiyor, yani renk kendi alfasıyla çarpılmış olmalı.
Alfası sıfır beyaz (1,1,1,0) geçerli bir premultiplied değer değildir: sayfanın
boyamadığı her yere beyaz ekler.

#### Gamepad / klavye ile gezinme

Focus, pointer emülasyonu değil, kendi durumudur: hover ve `:active` neyse
öyle kalır. Bir eleman gezinmeye yalnız `tabindex` taşıyorsa katılır (değeri
"-1" hariç); `id` tek başına yeterli değildir, çünkü `id` zaten tıklama ve
hit-test anlamı taşır.

```html
<div id="slot1" tabindex="0" data-nav-up="helmet">...</div>
```

```css
#slot1:focus { border-color:#3b82f6; }
```

`HtmlView.MoveFocus(HtmlNavDirection)` yön metriğiyle (yarım düzlem + yanal
sapma cezası + yanal örtüşme bonusu) bir sonraki elemanı seçer;
`data-nav-up/right/down/left="hedefId"` tasarımcı override'ıdır ve metrikten
önce gelir. `HtmlView.Activate()` odaklı elemanı gerçek tıklama yolundan
etkinleştirir: anchor'lar `AnchorClicked`'a, diğerleri `ElementClicked`'a
düşer. `HtmlFocusNavigator` bileşeni bunları EventSystem'in
Move/Submit/Cancel olaylarına bağlar; nesneyi
`EventSystem.SetSelectedGameObject` ile seçili yapmanız yeterlidir.

Çok panelli HUD'da (envanter + hotbar + ekipman gibi) navigator'ın
Inspector'daki **Up/Right/Down/Left komşu** alanlarını bağlayın: focus bir
panelin kenarından çıkınca komşu panele devredilir. Devir bir transaction'dır
(komşu gerçekten bir şey odaklamadıkça veren panel bırakmaz), her panel son
odağını hatırlar (geri dönüşte aynı slota gelinir) ve EventSystem seçimi
otomatik taşınır. Metin girişi ve IME kapsam dışıdır.

vw/vh birimleri viewport değiştiğinde yeniden hesaplanır. Bu önemli, çünkü CSS
viewport'u her zaman yazdığınız referans genişlik değildir: `DeviceScale` 0.5'te
taban yaptığı için küçük ekranlar daha keskin değil, **CSS olarak daha geniş**
bir sayfa alır. px ile ölçülen bir ızgara orada sarar; vw ile ölçülen sarmaz.

### Görseller

`<img src="gold">` için iki hazır sağlayıcı var; ikisi de aynı GameObject'e
eklenip `HtmlView` tarafından otomatik bulunur:

1. **`HtmlSpriteResources` (production önerisi):** build-time bir
   SpriteAtlas'ın sprite'larını doğrudan kullanır; runtime paketleme yok.
   Kontrat: atlasta **Allow Rotation ve Tight Packing kapalı** olmalı ve tüm
   sprite'lar tek sayfada (aynı texture'da) olmalı — kural bozulursa sağlayıcı
   sprite adıyla açık hata verir, sessizce yanlış çizmez.
2. **`HtmlResources`:** normal `Texture2D`'leri çalışırken tek atlasa
   paketler (kaynaklarda Read/Write açık olmalı). `Max Atlas Size`'a
   sığmayan pack, görselleri bulanıklaştırmak yerine yüksek sesle reddedilir.

Özel ihtiyaç için `IHtmlResourceProvider`'ı kendiniz doldurabilirsiniz.

### Fontlar

litehtml font yüklemez; siz vermelisiniz. İki yol:

1. **Önerilen:** `.ttf` dosyasını `.bytes` uzantısıyla projeye koyun (Unity
   `TextAsset` olarak import eder) ve component üstündeki font listesine ekleyin.
2. **Hızlı başlangıç:** `Use System Fonts` işaretli bırakın; macOS'ta Arial,
   Android'de Roboto bulunur. Layout cihazdan cihaza değişebileceği için
   üretimde kendi fontunuzu gömün.

---

## Testler

### Native (Unity'siz, GPU'suz)

```bash
LHU_ROOT="$PWD/Native" ./Native/build/macos/bin/lhu_harness Native/build/out
```

180 check. Ayrıca `Native/tests/lhu_raster.h` içindeki **referans CPU
rasterizer** ile `demo.png` üretir: shader'ın yürütülebilir spesifikasyonu.
Retained quad cache'in doğruluğu ayrı bir araçla kanıtlanıyor:
`lhu_verify_quadcache`, cache'li ve cache'siz kayıtları kare kare
karşılaştırır (2734 kare karşılaştırması). Bunun içinde bir
**state-transition matrix** var: 12 mutasyon aksiyonunun (metin, stil,
hover, scroll, resize, atlas büyümesi, invalidate, reload...) her sıralı
çifti dört karede sınanıyor; her karede quad akışının bayt eşitliği,
raporlanan dirty bölgesinin değişikliği gerçekten kapsadığı ve persistent
mesh'in stable-range sözleşmesinin (önek/sonek quad'ları gerçekten aynı mı)
tuttuğu doğrulanıyor.

Metin ölçüleri [Ahem](https://github.com/litehtml/litehtml/tree/master/containers/test/fonts)
fontuyla test ediliyor: her glyph tam 1em genişliğinde dolu bir kare olduğu için
"advance tam 100px" gibi **kesin** iddialar kurulabiliyor.

### Demo sahnesi

```bash
Unity -batchmode -projectPath . -executeMethod Doctype.EditorTools.HtmlDemoSceneBuilder.Build -quit
```

`Assets/Doctype/Samples/HtmlDemo.unity` üretir: Camera + EventSystem
(yeni Input System modülüyle) + Canvas + RawImage üzerinde `HtmlView` +
`HtmlRawImage` + `HtmlDemoController`. Sahne elle değil **script ile**
üretiliyor; serialize edilmiş GUID yığını olan bir `.unity` dosyası
gözden geçirilebilir değil.

Sahnede beş sayfa ve üstte bir menü var: **Genel · Animasyon · Performans ·
Tipografi · Hakkında**. Gezinme `<a href="page://...">` linkleriyle; JavaScript
olmadığı için tıklama `AnchorClicked` üzerinden C#'a düşüyor ve controller sayfayı
değiştiriyor. Sayfa girişinde 0.22 sn'lik kayma + solma geçişi var.

**Animasyon.** litehtml'de CSS `animation`/`transition` **yok**. Değerler C#'ta
hesaplanıp `SetStyle` ile inline style olarak yazılıyor: parse yok, layout
artımlı, redraw yalnız değişen bölge. Animasyon sayfası bilerek farklı
renderer yollarını zorluyor:
dönen konik gradient, nefes alan radial gradient, boyut+renk atan yuvarlak kare,
28 çubukluk sinüs dalgası, kayan bar.

**Kare hızı sınırı.** `FrameRateLimiter` ayrı bir bileşen (demoya gömülü değil,
her sahneye eklenebilir). Varsayılan **61 fps**: tam 60 yerine bir kare pay,
limiter ile ekran tazeleme hızının birbiriyle yarışmasını önlüyor. Sınır yalnızca
VSync kapalıyken geçerli olduğu için bileşen ikisini birlikte ayarlıyor.
Performans sayfasından belge içinden `fps://30` gibi linklerle değiştirilebiliyor.

### Ölçülen maliyet

Güncel ve ayrıntılı ölçümler kök [README](../../README.md)'nin
"Measurements" bölümünde (Xiaomi 22101316I, Vulkan, senaryo başına 300 kare).
Özet: değişmeyen bir sayfa renderer tarafında **hiçbir iş** yapmaz (parse,
layout, kayıt, mesh upload, offscreen redraw yok; RenderTexture'ın Canvas'ta
composite edilme maliyeti normal şekilde devam eder). Kare başına bir
`SetText`/`SetStyle` ~1 ms CPU + ~3.4 ms GPU. Her karede `LoadHtml` çağırmak
ise ~12 ms CPU'luk bilinçli bir **negatif kontroldür** (benchmark'taki
`hud-reload` satırı): yanlış API'nin bedelini ölçmek için var, gerçek
kullanım yolu değil.

### Unity (EditMode, gerçek GPU)

```bash
Unity -batchmode -projectPath . -runTests -testPlatform EditMode -testResults results.xml
```

54 test. `-quit` **kullanmayın** (testler çalışmadan çıkar) ve `-nographics`
**kullanmayın** (GPU testleri anlamsızlaşır).

`HtmlRenderTests` gerçek bir `RenderTexture`'a çizip `ReadPixels` ile geri
okur; bozuk bir SDF, ters çevrilmiş projeksiyon veya renk uzayı hatası burada
patlar.

### Unity (PlayMode, runtime)

```bash
Unity -batchmode -projectPath . -runTests -testPlatform PlayMode -testResults pm.xml
```

40 test. EditMode testleri `HtmlDocument`/`HtmlRenderer`'ı doğrudan
sürüyor; PlayMode testleri ise `HtmlView`'ın **MonoBehaviour yaşam
döngüsünden** geçiyor: `OnEnable`, `LateUpdate` render döngüsü, yüzeyin yeniden
oluşturulması, `Destroy` sonrası temizlik. Ömür ve kare-kare hatalar burada yaşar.

Kapsam: yüzey üretimi, içerik değişiminin bir sonraki karede ekrana düşmesi,
hover'ın yeniden parse etmeden repaint etmesi, **resize sonrası hover durumunun
korunması** (regresyon), `<a>` tıklamasının C# event'ine ulaşması, 30 kare
boyunca her karede içerik değiştirme (atlas büyümesi + mesh tampon yeniden
kullanımı), destroy/recreate döngüsü ve tam sahnenin çalışması.

Demoya özel testler (`HtmlDemoTests`): menüyle gezinme, **nav bar'a gerçek
piksel tıklaması**, animasyonun kareler arası piksel değiştirdiği, kapatılınca
sayfanın bit-bit sabit kaldığı, fps sınırının Unity'ye ulaştığı (ve VSync'in
kapandığı), belge içinden `fps://` linkiyle sınırın değiştirilebildiği, ve
**yeniden kurma maliyetinin kare bütçesinde kaldığı**.

`HtmlRuntimeCapture` her sayfayı ayrı PNG olarak yazar
(`demo_overview.png`, `demo_animation.png`, …).

### Görsel karşılaştırma

```
Unity -batchmode -projectPath . \
      -executeMethod Doctype.EditorTools.HtmlCapture.CaptureDemo -quit
```

`Native/tests/demo.html` dosyasını GPU'da render edip `demo_gpu.png` yazar.
Aynı dosyayı native harness CPU'da render eder. Son ölçüm: **piksellerin
%73'ü bit-bit aynı, %96'sı ≤4 fark, ortalama fark 1.3/255**; kalan fark yalnızca
antialias kenarlarında ve CPU'nun sRGB, Unity'nin linear uzayda harmanlamasından
kaynaklanıyor.

---

## Kapsam

**Çalışan:** blok/inline/float layout, flexbox, tablolar, `border-radius`
(eliptik dahil), kenar başına kenarlık, linear/radial/conic gradient, `overflow`
kırpma, metin dekorasyonları, liste işaretleyicileri, `:hover`, `<a>` tıklaması,
`@media` sorguları, `<link>`/`@import` (host callback ile).

**Yok:**
- **JavaScript.** litehtml'de JS motoru yok. Dinamik UI için DOM'u C# tarafından
  güncelleyip yeniden layout alın.
- **CSS animasyon/transition/transform.** litehtml desteklemiyor.
- **Karmaşık metin şekillendirme.** stb_truetype kullanılıyor: basit
  Latin/Türkçe/Kiril oyun UI metinleri için yeterli; Arapça/Farsça/Hintçe
  için HarfBuzz + BiDi gerekir.
- `text-transform` ASCII + Latin-1 + Latin Extended-A basit eşlemeleri kapsar
  (ç/ş/ğ/ö/ü dahil); Türkçe i/İ-ı/I dörtlüsü için `HtmlView.SetLanguage("tr",
  "tr-TR")` çağırın (yüklemeden önce). Bire-çok eşlemeler (ß→SS) kapsam dışı.

## Bilinen sınırlar

- Vertex başına 108 bayt (yarıçap, kenarlık ve klip alanları half precision).
  2000 quad'lık bir sayfa tam yeniden kayıtta ~850 KB vertex yükler; kısmi
  güncellemede yalnız değişen kareler yüklenir. Cihaz ölçümü bant genişliğinin
  darboğaz olmadığını gösterdi; instancing/structured buffer bu yüzden (ve
  GLES3 uyumluluğu için) yapılmadı.
- İç içe yuvarlak kırpmalarda en içteki yarıçaplar kullanılıyor (yaklaşım).
- `border-style` yalnızca `solid`; `dashed`/`dotted`/`double` solid'e düşüyor.

## Lisanslar

| Bileşen | Lisans |
|---|---|
| litehtml | BSD-3-Clause |
| gumbo-parser (litehtml içinde) | Apache-2.0 |
| stb_truetype | Public domain / MIT |
| Ahem fontu (yalnızca testlerde) | OFL |
| Bu wrapper | projenizin lisansı |
