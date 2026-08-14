# Doctype

HTML/CSS arayüzleri Unity'de **GPU'da** render eden bir sistem. Layout'u
[litehtml](https://github.com/litehtml/litehtml) yapıyor, çizimin tamamını Unity
üstleniyor — arada CPU rasterizer yok.

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

### macOS (bu makinede doğrulandı)

```bash
./Native/build_macos.sh
```

Universal (arm64 + x86_64) `.bundle` üretip
`Assets/Doctype/Plugins/macOS/` altına koyar. Tek gereksinim Xcode —
CMake gerekmiyor.

### Android (kod hazır, bu makinede derlenmedi)

```bash
./Native/build_android.sh arm64-v8a
```

Gereksinimler: Android NDK, `cmake` + `ninja`, ve Unity'nin Android Build
Support modülü. **Bu makinede üçü de kurulu değil**, dolayısıyla Android yolu
yazıldı ama çalıştırılmadı — ilk derlemeyi doğrulanmamış kabul edin.

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
```

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

Şeffaf sayfa için `Background` **(0,0,0,0)** olmalı — yüzey `Blend One
OneMinusSrcAlpha` ile birleşiyor, yani renk kendi alfasıyla çarpılmış olmalı.
Alfası sıfır beyaz (1,1,1,0) geçerli bir premultiplied değer değildir: sayfanın
boyamadığı her yere beyaz ekler.

vw/vh birimleri viewport değiştiğinde yeniden hesaplanır. Bu önemli, çünkü CSS
viewport'u her zaman yazdığınız referans genişlik değildir: `DeviceScale` 0.5'te
taban yaptığı için küçük ekranlar daha keskin değil, **CSS olarak daha geniş**
bir sayfa alır. px ile ölçülen bir ızgara orada sarar; vw ile ölçülen sarmaz.

### Fontlar

litehtml font yüklemez; siz vermelisiniz. İki yol:

1. **Önerilen:** `.ttf` dosyasını `.bytes` uzantısıyla projeye koyun (Unity
   `TextAsset` olarak import eder) ve component üstündeki font listesine ekleyin.
2. **Hızlı başlangıç:** `Use System Fonts` işaretli bırakın — macOS'ta Arial,
   Android'de Roboto bulunur. Layout cihazdan cihaza değişebileceği için
   üretimde kendi fontunuzu gömün.

---

## Testler

### Native (Unity'siz, GPU'suz)

```bash
LHU_ROOT="$PWD/Native" ./Native/build/macos/bin/lhu_harness Native/build/out
```

58 assertion. Ayrıca `Native/tests/lhu_raster.h` içindeki **referans CPU
rasterizer** ile `demo.png` üretir — shader'ın yürütülebilir spesifikasyonu.

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
hesaplanıp inline style olarak yazılıyor, sayfa her karede yeniden parse +
layout ediliyor. Animasyon sayfası bilerek farklı renderer yollarını zorluyor:
dönen konik gradient, nefes alan radial gradient, boyut+renk atan yuvarlak kare,
28 çubukluk sinüs dalgası, kayan bar.

**Kare hızı sınırı.** `FrameRateLimiter` ayrı bir bileşen (demoya gömülü değil,
her sahneye eklenebilir). Varsayılan **61 fps** — tam 60 yerine bir kare pay,
limiter ile ekran tazeleme hızının birbiriyle yarışmasını önlüyor. Sınır yalnızca
VSync kapalıyken geçerli olduğu için bileşen ikisini birlikte ayarlıyor.
Performans sayfasından belge içinden `fps://30` gibi linklerle değiştirilebiliyor.

### Ölçülen maliyet

Animasyon sayfası her karede yeniden kurulurken (307 quad, 680x460, M-serisi Mac):

| Aşama | Süre |
|---|---|
| Parse (gumbo + CSS) | ~1.7 ms |
| Layout | ~0.03 ms |
| Kayıt + çizim | ~0.5 ms |
| **Toplam** | **~2.3 ms ort. / 2.8 ms en kötü** |

Dikkat çeken nokta: **maliyet neredeyse tamamen yeniden parse**, layout pratikte
bedava. 60 fps bütçesinin (16.6 ms) ~%14'ü. Her karede animasyon şart değilse
`Animate`'i kapatın; şartsa bir sonraki optimizasyon adımı DOM'u yeniden parse
etmeden güncellemek (litehtml'in `create_element` / `append_children_from_string`
API'lerini C ABI'ye açmak) olur — layout zaten ucuz olduğu için kazanç doğrudan
o 1.7 ms'den gelir.

### Unity (EditMode, gerçek GPU)

```bash
Unity -batchmode -projectPath . -runTests -testPlatform EditMode -testResults results.xml
```

22 test. `-quit` **kullanmayın** (testler çalışmadan çıkar) ve `-nographics`
**kullanmayın** (GPU testleri anlamsızlaşır).

`HtmlRenderTests` gerçek bir `RenderTexture`'a çizip `ReadPixels` ile geri
okur; bozuk bir SDF, ters çevrilmiş projeksiyon veya renk uzayı hatası burada
patlar.

### Unity (PlayMode, runtime)

```bash
Unity -batchmode -projectPath . -runTests -testPlatform PlayMode -testResults pm.xml
```

16 test. EditMode testleri `HtmlDocument`/`HtmlRenderer`'ı doğrudan
sürüyor; PlayMode testleri ise `HtmlView`'ın **MonoBehaviour yaşam
döngüsünden** geçiyor — `OnEnable`, `LateUpdate` render döngüsü, yüzeyin yeniden
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
- **Görseller** boru hattı bağlandı (`IHtmlResourceProvider`) ama hazır bir
  atlas sağlayıcısı bu sürümde yok — kendi implementasyonunuzu verin.
- **Karmaşık metin şekillendirme.** stb_truetype kullanılıyor: Latin/Türkçe
  kusursuz, Arapça/Farsça/Hintçe için HarfBuzz gerekir.
- `text-transform` yalnızca ASCII (Türkçe i/İ dönüşümü doğru değil).

## Bilinen sınırlar

- Vertex başına 144 bayt. 2000 quad'lık bir sayfa kare başına ~1.1 MB vertex
  yüklüyor. Instancing veya structured buffer'a geçilebilir; GLES3 uyumluluğu
  için bilinçli olarak yapılmadı.
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
