using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using UnityEngine;

namespace Doctype.Samples
{
    /// <summary>
    /// A navigable, animated demo page driven entirely from C#.
    /// </summary>
    /// <remarks>
    /// litehtml has no JavaScript and no CSS animations or transitions, so both
    /// navigation and animation are done the way this system expects: compute
    /// the values in C#, write them into the markup as inline styles, and let
    /// the page be re-parsed and re-laid-out. That makes this the harshest
    /// runtime test in the project — at 60 fps the whole pipeline runs sixty
    /// times a second, and <see cref="HtmlView.TotalMs"/> shows what it
    /// costs.
    /// </remarks>
    [RequireComponent(typeof(HtmlView))]
    [AddComponentMenu("Doctype/Samples/Demo Controller")]
    public class HtmlDemoController : MonoBehaviour
    {
        public enum Page
        {
            Overview,
            Animation,
            Performance,
            Typography,
            Images,
            About,
        }

        [Tooltip("Rebuild every frame so the animated pages move. Off = rebuild on a timer.")]
        [SerializeField] private bool _animate = true;

        [Tooltip("Rebuild interval when animation is off, in seconds.")]
        [SerializeField] private float _idleRefreshInterval = 0.25f;

        [Tooltip("Optional. When set, the Performance page can change the cap from inside the document.")]
        [SerializeField] private FrameRateLimiter _frameRateLimiter;

        private HtmlView _view;
        private readonly StringBuilder _sb = new StringBuilder(8192);

        // Ring buffer of recent frame times, drawn as a live chart.
        private const int HistoryLength = 48;
        private readonly float[] _frameMs = new float[HistoryLength];
        private int _historyHead;

        private Page _page = Page.Overview;
        private float _pageEnteredAt;

        /// <summary>
        /// True while the overview page is on screen and its live values carry
        /// the ids <see cref="RefreshOverviewStats"/> writes to. Cleared on a
        /// page change so the first frame of a new page rebuilds properly.
        /// </summary>
        private bool _statsBound;
        private float _nextRefresh;
        private float _smoothedFps = 60f;
        private int _rebuilds;
        private string _lastLink = "-";

        private int _buttonClicks;
        private string _lastButton = "-";

        /// <summary>Times the page has been rebuilt since enable.</summary>
        public int Rebuilds => _rebuilds;

        /// <summary>The page currently on screen.</summary>
        public Page CurrentPage => _page;

        public string LastClickedLink => _lastLink;

        /// <summary>How many times a demo button has been pressed.</summary>
        public int ButtonClicks => _buttonClicks;

        /// <summary>data-action of the last button pressed.</summary>
        public string LastButtonAction => _lastButton;

        public bool Animate
        {
            get => _animate;
            set
            {
                _animate = value;
                _nextRefresh = 0f;
            }
        }

        // Culture-invariant formatting. A Turkish (or German, or French) locale
        // would otherwise write "0,5" into the CSS, which is not a number.
        private static string F(float value, string format = "0.###")
        {
            return value.ToString(format, CultureInfo.InvariantCulture);
        }

        private static string Rgba(int r, int g, int b, float a)
        {
            return $"rgba({r},{g},{b},{F(a, "0.###")})";
        }

        private HtmlResources _resources;
        private readonly List<Texture2D> _generated = new List<Texture2D>();

        private void Awake()
        {
            _view = GetComponent<HtmlView>();

            BuildIcons();

            if (_frameRateLimiter == null)
            {
                _frameRateLimiter = GetComponent<FrameRateLimiter>() ??
                                    FindAnyObjectByType<FrameRateLimiter>();
            }
        }

        private void OnEnable()
        {
            _view.AnchorClicked += OnAnchorClicked;

            // This is how game code reaches a button in the markup: bind by the
            // element's id. The binding survives reloads, so it can be set up
            // once here even though the page is rebuilt every frame.
            _view.BindClick("btn-play", OnDemoButton);
            _view.BindClick("btn-save", OnDemoButton);
            _view.BindClick("btn-quit", OnDemoButton);

            _pageEnteredAt = Time.unscaledTime;
            _nextRefresh = 0f;
        }

        private void OnDisable()
        {
            _view.AnchorClicked -= OnAnchorClicked;
            _view.ClearClickBindings();
        }

        private void OnDestroy()
        {
            foreach (Texture2D t in _generated)
            {
                if (t != null)
                {
                    Destroy(t);
                }
            }
            _generated.Clear();
        }

        private void OnDemoButton(HtmlElementClick click)
        {
            _buttonClicks++;
            _lastButton = string.IsNullOrEmpty(click.Action) ? click.Id : click.Action;
            _nextRefresh = 0f;

            Debug.Log($"[Doctype] butona basildi: id='{click.Id}' action='{click.Action}' " +
                      $"(toplam {_buttonClicks})", this);
        }

        /// <summary>Switches page and restarts the enter transition.</summary>
        public void GoTo(Page page)
        {
            if (_page == page)
            {
                return;
            }

            _page = page;
            _pageEnteredAt = Time.unscaledTime;
            _nextRefresh = 0f;
            _statsBound = false;
        }

        private void OnAnchorClicked(string url)
        {
            _lastLink = url;
            _nextRefresh = 0f;

            if (url.StartsWith("page://"))
            {
                string name = url.Substring("page://".Length);
                if (System.Enum.TryParse(name, true, out Page page))
                {
                    GoTo(page);
                }

                return;
            }

            if (url.StartsWith("fps://"))
            {
                string value = url.Substring("fps://".Length);

                if (_frameRateLimiter != null)
                {
                    if (value == "off")
                    {
                        _frameRateLimiter.SetLimited(false);
                    }
                    else if (int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int fps))
                    {
                        _frameRateLimiter.SetTargetFrameRate(fps);
                    }
                }

                return;
            }

            if (url == "anim://toggle")
            {
                Animate = !_animate;
            }
        }

        private void Update()
        {
            float dt = Mathf.Max(Time.unscaledDeltaTime, 0.0001f);
            _smoothedFps = Mathf.Lerp(_smoothedFps, 1f / dt, 0.08f);

            _frameMs[_historyHead] = dt * 1000f;
            _historyHead = (_historyHead + 1) % HistoryLength;

            if (!_animate && Time.unscaledTime < _nextRefresh)
            {
                return;
            }

            _nextRefresh = Time.unscaledTime + _idleRefreshInterval;
            _rebuilds++;

            // Once the overview page has finished sliding in, every value on it
            // that still moves is text. Writing those in place skips re-parsing
            // the whole page to change four numbers, which is the case
            // HtmlView.SetText exists for. Watch the CPU stat drop when this
            // path takes over.
            if (_statsBound && Transition() >= 1f)
            {
                if (_page == Page.Overview)
                {
                    RefreshOverviewStats();
                    return;
                }

                if (_page == Page.Animation && _animate)
                {
                    RefreshAnimation(Time.unscaledTime);
                    return;
                }
            }

            _view.LoadHtml(BuildDocument());
            _statsBound = _page == Page.Overview || _page == Page.Animation;
        }

        /// <summary>Eased 0..1 progress of the page-enter transition.</summary>
        private float Transition()
        {
            const float duration = 0.22f;
            float t = Mathf.Clamp01((Time.unscaledTime - _pageEnteredAt) / duration);

            // Cubic ease-out: fast at the start, settles softly.
            return 1f - Mathf.Pow(1f - t, 3f);
        }

        private string BuildDocument()
        {
            float enter = Transition();
            float clock = Time.unscaledTime;

            _sb.Clear();
            AppendStyle();

            _sb.Append("<body><div class=\"shell\">");
            AppendNav();

            // The transition slides the content in and fades it up. There is no
            // CSS transition to lean on, so both are inline values.
            _sb.Append("<div class=\"page\" style=\"margin-left:")
               .Append(F((1f - enter) * 26f)).Append("px;color:")
               .Append(Rgba(230, 233, 240, enter)).Append("\">");

            switch (_page)
            {
                case Page.Animation: AppendAnimationPage(clock, enter); break;
                case Page.Performance: AppendPerformancePage(); break;
                case Page.Typography: AppendTypographyPage(); break;
                case Page.Images: AppendImagesPage(); break;
                case Page.About: AppendAboutPage(); break;
                default: AppendOverviewPage(enter); break;
            }

            _sb.Append("</div></div></body>");
            return _sb.ToString();
        }

        private void AppendStyle()
        {
            _sb.Append(@"<html><head><style>
  body { margin:0; background:#0f1117; font-family:sans-serif; font-size:14px; color:#e6e9f0; }
  .shell { margin:14px; }
  .nav { margin-bottom:12px; }
  .nav a { display:inline-block; padding:6px 13px; margin-right:5px; border-radius:8px;
           background:#1a1f2e; color:#9aa4c0; border:1px solid #262d40; }
  .nav a:hover { background:#243050; color:#dbe6ff; border:1px solid #3a4b7a; }
  .nav a.on { background:linear-gradient(135deg,#2563eb,#7c3aed); color:#fff; border:1px solid #6d8cff; }
  .card { padding:16px 18px; border-radius:14px; border:1px solid #2a3350;
          background:linear-gradient(135deg,#161b29,#1d2540); margin-bottom:12px; }
  h1 { font-size:21px; margin:0 0 4px 0; color:#fff; }
  h2 { font-size:14px; margin:0 0 10px 0; color:#8e97b3; font-weight:normal; }
  .stat { display:inline-block; margin-right:24px; }
  .k { font-size:10px; color:#7c86a3; display:block; }
  .v { font-size:20px; color:#7dd3fc; }
  table { width:100%; border-collapse:collapse; font-size:13px; }
  td { padding:6px 8px; border-bottom:1px solid #232b45; }
  td.r { text-align:right; color:#8e97b3; }
  .bar { height:8px; border-radius:4px; background:#1c2338; }
  .bar > i { display:block; height:8px; border-radius:4px;
             background:linear-gradient(to right,#22d3ee,#3b82f6); }
  .chart { height:56px; border-radius:8px; background:#12172a; padding:4px; }
  .chart i { display:inline-block; width:6px; margin-right:1px;
             background:linear-gradient(to top,#1d4ed8,#38bdf8); border-radius:2px; }
  .pill { display:inline-block; padding:5px 12px; border-radius:999px; font-size:12px;
          background:#1e2740; color:#9fb4e8; margin-right:6px; }
  .pill.on { background:#2563eb; color:#fff; }
  .pill:hover { background:#33406b; color:#fff; }
  .muted { color:#7c86a3; font-size:12px; }
  .btn { display:inline-block; padding:9px 18px; margin-right:8px; border-radius:9px;
         font-size:14px; color:#dbe6ff; background:#233056; border:1px solid #3a4b7a; }
  .btn:hover { background:#2f4585; color:#ffffff; }
  .btn:active { background:#1b2547; }
</style></head>");
        }

        private void AppendNav()
        {
            _sb.Append("<div class=\"nav\">");
            NavLink(Page.Overview, "Genel");
            NavLink(Page.Animation, "Animasyon");
            NavLink(Page.Performance, "Performans");
            NavLink(Page.Typography, "Tipografi");
            NavLink(Page.Images, "Gorseller");
            NavLink(Page.About, "Hakkinda");
            _sb.Append("</div>");
        }

        private void NavLink(Page page, string label)
        {
            _sb.Append("<a href=\"page://").Append(page).Append('"');

            if (_page == page)
            {
                _sb.Append(" class=\"on\"");
            }

            _sb.Append('>').Append(label).Append("</a>");
        }

        private void AppendOverviewPage(float enter)
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1>litehtml &rarr; Unity GPU</h1>");
            _sb.Append("<h2>CPU'da layout, GPU'da SDF ile cizim. Tek mesh, tek draw call.</h2>");

            Stat("FPS", F(_smoothedFps, "0"), "stat-fps");
            Stat("QUAD", _view.QuadCount.ToString(CultureInfo.InvariantCulture), "stat-quad");
            Stat("CPU", F(_view.TotalMs, "0.00") + " ms", "stat-cpu");
            Stat("REBUILD", _rebuilds.ToString(CultureInfo.InvariantCulture), "stat-rebuild");

            // Fills up as the enter transition completes, so the page arrives
            // with motion instead of appearing all at once.
            _sb.Append("<div class=\"bar\" style=\"margin-top:14px\"><i style=\"width:")
               .Append(F(enter * 100f)).Append("%\"></i></div>");

            _sb.Append("<table style=\"margin-top:12px\">");
            Row("Belge yuksekligi", F(_view.DocumentHeight, "0") + " px", "row-height");
            Row("Yuzey", _view.Texture != null
                    ? $"{_view.Texture.width}x{_view.Texture.height}"
                    : "-", "row-surface");
            Row("Kare hizi siniri", LimiterLabel(), "row-limiter");
            Row("Son tiklanan link", _lastLink, "row-link");
            _sb.Append("</table>");

            _sb.Append("<p class=\"muted\" style=\"margin:12px 0 0 0\">")
               .Append("Bu sayfanin sayilari SetText ile yerinde guncelleniyor, parse yok. ")
               .Append("Animasyon sayfasi inline stil degistirdigi icin her karede yeniden ")
               .Append("parse + layout edilir.</p>");

            _sb.Append("</div>");

            AppendButtonCard();
        }

        /// <summary>
        /// Real buttons wired to C# handlers.
        /// </summary>
        /// <remarks>
        /// Each one carries an id that <see cref="OnEnable"/> bound with
        /// HtmlView.BindClick, plus a data-action the handler reads. Pressing
        /// one writes to the Unity console.
        /// </remarks>
        private void AppendButtonCard()
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1 style=\"font-size:16px\">Butonlar</h1>");
            _sb.Append("<h2>Her biri C#'ta BindClick ile bagli; basinca Debug.Log calisir.</h2>");

            Button("btn-play", "play", "Oyna");
            Button("btn-save", "save-game", "Kaydet");
            Button("btn-quit", "quit", "Cik");

            _sb.Append("<p class=\"muted\" style=\"margin:12px 0 0 0\">Basilan: <b>")
               .Append(_lastButton).Append("</b> &nbsp;|&nbsp; toplam ")
               .Append(_buttonClicks).Append(" tiklama</p>");

            _sb.Append("</div>");
        }

        private void Button(string id, string action, string label)
        {
            _sb.Append("<button id=\"").Append(id)
               .Append("\" data-action=\"").Append(action)
               .Append("\" class=\"btn\">").Append(label).Append("</button>");
        }

        private void AppendAnimationPage(float clock, float enter)
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1>Animasyon</h1>");
            _sb.Append("<h2>litehtml'de CSS animasyonu yok. Degerler C#'ta hesaplanip ")
               .Append("inline style olarak yaziliyor.</h2>");

            // Every animated element carries an id and gets its style from the
            // helpers below, so RefreshAnimation can rewrite exactly the same
            // declarations in place without the two ever drifting apart.
            _sb.Append("<div id=\"a-conic\" style=\"").Append(ConicStyle(clock)).Append("\"></div>");
            _sb.Append("<div id=\"a-radial\" style=\"").Append(RadialStyle(clock)).Append("\"></div>");

            _sb.Append("<div style=\"display:inline-block;vertical-align:top;width:84px;height:84px\">")
               .Append("<div id=\"a-pulse\" style=\"").Append(PulseStyle(clock)).Append("\"></div></div>");

            // A travelling sine wave of bars: many small boxes, each with its
            // own animated height, which stresses layout rather than fill rate.
            _sb.Append("<div style=\"margin-top:16px;height:60px\">");
            for (int i = 0; i < BarCount; i++)
            {
                _sb.Append("<div id=\"a-bar").Append(i).Append("\" style=\"").Append(BarStyle(clock, i))
                   .Append("\"></div>");
            }
            _sb.Append("</div>");

            // Bar that slides back and forth using margin, not transform.
            _sb.Append("<div class=\"bar\" style=\"margin-top:14px\"><i id=\"a-slide\" style=\"")
               .Append(SlideStyle(clock)).Append("\"></i></div>");

            _sb.Append("<p id=\"a-stats\" class=\"muted\" style=\"margin:14px 0 0 0\">")
               .Append(AnimationStats()).Append("</p>");

            _sb.Append("</div>");
            AppendAnimationToggle();
        }

        const int BarCount = 28;

        private string AnimationStats() =>
            "Her kare: " + F(_view.ParseMs, "0.00") + " ms parse + " +
            F(_view.LayoutMs, "0.00") + " ms layout + " +
            F(_view.DrawMs, "0.00") + " ms cizim  |  " + _view.QuadCount + " quad";

        // Exercises the conic path, which nothing else in the demo touches.
        private static string ConicStyle(float clock) =>
            "display:inline-block;width:84px;height:84px;border-radius:42px;margin-right:18px;" +
            "background:conic-gradient(from " + F(clock * 90f % 360f) +
            "deg,#22d3ee,#3b82f6,#a855f7,#22d3ee)";

        private static string RadialStyle(float clock) =>
            "display:inline-block;width:84px;height:84px;border-radius:16px;margin-right:18px;" +
            "background:radial-gradient(circle " + F(30f + Mathf.Sin(clock * 2.1f) * 14f) +
            "px at 50% 50%,#f472b6,#1e1b4b)";

        private static string PulseStyle(float clock)
        {
            float pulse = (Mathf.Sin(clock * 3f) + 1f) * 0.5f;
            float size = 44f + pulse * 34f;
            return "width:" + F(size) + "px;height:" + F(size) +
                   "px;margin:" + F((84f - size) * 0.5f) + "px;border-radius:" +
                   F(8f + pulse * 34f) + "px;background:" + Rgba(34, 211, 238, 0.35f + pulse * 0.65f);
        }

        private static string BarStyle(float clock, int i)
        {
            float h = 10f + (Mathf.Sin(clock * 3.2f - i * 0.32f) + 1f) * 0.5f * 46f;
            return "display:inline-block;vertical-align:bottom;width:10px;margin-right:3px;" +
                   "border-radius:3px;height:" + F(h) + "px;background:" + Rgba(56, 189, 248, 0.35f + h / 90f);
        }

        private static string SlideStyle(float clock) =>
            "margin-left:" + F((Mathf.Sin(clock * 1.4f) + 1f) * 0.5f * 62f) + "%;width:38%";

        /// <summary>
        /// Rewrites the animated declarations in place. This is the case
        /// HtmlView.SetStyle exists for: the page changes no text, so
        /// SetText cannot help it, and rebuilding the markup re-parses
        /// everything to move a few numbers.
        /// </summary>
        private void RefreshAnimation(float clock)
        {
            _view.SetStyle("#a-conic", ConicStyle(clock));
            _view.SetStyle("#a-radial", RadialStyle(clock));
            _view.SetStyle("#a-pulse", PulseStyle(clock));
            _view.SetStyle("#a-slide", SlideStyle(clock));

            for (int i = 0; i < BarCount; i++)
            {
                _view.SetStyle("#a-bar" + i, BarStyle(clock, i));
            }

            // The read-out is text, so it takes the cheaper path.
            _view.SetText("#a-stats", AnimationStats());
        }

        private void AppendAnimationToggle()
        {
            _sb.Append("<div class=\"card\" style=\"padding:12px 16px\">");
            _sb.Append("<a href=\"anim://toggle\" class=\"pill")
               .Append(_animate ? " on" : "").Append("\">")
               .Append(_animate ? "Animasyon acik" : "Animasyon kapali")
               .Append("</a>");
            _sb.Append("<span class=\"muted\">Kapatinca sayfa saniyede ")
               .Append(F(1f / _idleRefreshInterval, "0")).Append(" kez yenilenir.</span>");
            _sb.Append("</div>");
        }

        private void AppendPerformancePage()
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1>Performans</h1>");
            _sb.Append("<h2>Olculen degerler, tahmin degil.</h2>");

            Stat("PARSE", F(_view.ParseMs, "0.00") + " ms");
            Stat("LAYOUT", F(_view.LayoutMs, "0.00") + " ms");
            Stat("CIZIM", F(_view.DrawMs, "0.00") + " ms");
            Stat("TOPLAM", F(_view.TotalMs, "0.00") + " ms");

            // Frame-time history, oldest on the left. Scaled to the window's own
            // peak rather than a fixed ms-per-pixel: an uncapped build runs
            // sub-millisecond frames, and a fixed scale would flatten the chart
            // into a straight line that tells you nothing.
            float peak = 0.001f;
            for (int i = 0; i < HistoryLength; i++)
            {
                peak = Mathf.Max(peak, _frameMs[i]);
            }

            _sb.Append("<div class=\"chart\" style=\"margin-top:14px\">");
            for (int i = 0; i < HistoryLength; i++)
            {
                float ms = _frameMs[(_historyHead + i) % HistoryLength];
                float h = Mathf.Max(2f, ms / peak * 46f);
                _sb.Append("<i style=\"height:").Append(F(h)).Append("px\"></i>");
            }
            _sb.Append("</div>");
            _sb.Append("<p class=\"muted\" style=\"margin:6px 0 0 0\">Son ").Append(HistoryLength)
               .Append(" kare &nbsp;|&nbsp; en yuksek ").Append(F(peak, "0.00")).Append(" ms")
               .Append(" &nbsp;|&nbsp; glyph atlasi ").Append(_view.FontAtlasSize.x).Append('x')
               .Append(_view.FontAtlasSize.y).Append("</p>");

            _sb.Append("</div>");
            AppendFrameRateCard();
        }

        private void AppendFrameRateCard()
        {
            _sb.Append("<div class=\"card\" style=\"padding:12px 16px\">");
            _sb.Append("<div style=\"margin-bottom:8px\">Kare hizi siniri: <b>")
               .Append(LimiterLabel()).Append("</b></div>");

            if (_frameRateLimiter == null)
            {
                _sb.Append("<span class=\"muted\">Sahnede FrameRateLimiter yok.</span></div>");
                return;
            }

            FpsPill(30);
            FpsPill(61);
            FpsPill(120);

            _sb.Append("<a href=\"fps://off\" class=\"pill")
               .Append(_frameRateLimiter.IsLimited ? "" : " on").Append("\">Sinirsiz</a>");

            _sb.Append("<p class=\"muted\" style=\"margin:10px 0 0 0\">")
               .Append("Sinir yalnizca VSync kapaliyken gecerlidir; limiter ikisini birlikte ayarlar. ")
               .Append("61, ekran tazeleme hiziyla yaris etmemek icin bir kare pay birakir.</p>");

            _sb.Append("</div>");
        }

        private void FpsPill(int fps)
        {
            bool on = _frameRateLimiter != null && _frameRateLimiter.IsLimited &&
                      _frameRateLimiter.TargetFrameRate == fps;

            _sb.Append("<a href=\"fps://").Append(fps.ToString(CultureInfo.InvariantCulture))
               .Append("\" class=\"pill").Append(on ? " on" : "").Append("\">")
               .Append(fps).Append(" fps</a>");
        }

        private string LimiterLabel()
        {
            if (_frameRateLimiter == null)
            {
                return "-";
            }

            return _frameRateLimiter.IsLimited
                ? _frameRateLimiter.TargetFrameRate.ToString(CultureInfo.InvariantCulture) + " fps"
                : "sinirsiz";
        }

        private void AppendTypographyPage()
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1>Tipografi</h1>");
            _sb.Append("<h2>Metin, glyph atlasindan quad olarak ciziliyor.</h2>");

            _sb.Append("<p style=\"font-size:24px;margin:0 0 6px 0\">Yigit, dun sabah kahvalti yapti.</p>");
            _sb.Append("<p style=\"margin:0 0 6px 0\"><b>Kalin</b>, <i>egik</i>, ")
               .Append("<u>alti cizili</u>, <s>ustu cizili</s> ve <span style=\"color:#f472b6\">renkli</span>.</p>");

            _sb.Append("<ul style=\"margin:8px 0\">")
               .Append("<li>Isaretli liste</li><li>Ikinci madde</li></ul>");
            _sb.Append("<ol style=\"margin:8px 0\">")
               .Append("<li>Numarali liste</li><li>Ikinci madde</li></ol>");

            _sb.Append("<table style=\"margin-top:10px\">");
            Row("Font motoru", "stb_truetype");
            Row("Atlas", "R8, talep uzerine buyur");
            Row("Sekillendirme", "yok (Latin/Turkce)");
            _sb.Append("</table>");

            _sb.Append("</div>");
        }

        /// <summary>
        /// Draws the demo's icons in code, so the sample needs no art assets and
        /// the textures are readable without an import setting.
        /// </summary>
        private void BuildIcons()
        {
            _resources = gameObject.GetComponent<HtmlResources>() ??
                         gameObject.AddComponent<HtmlResources>();

            _generated.Add(HtmlDemoIcons.Coin(64));
            _generated.Add(HtmlDemoIcons.Gem(64));
            _generated.Add(HtmlDemoIcons.Potion(64));

            _resources.Register("coin", _generated[0]);
            _resources.Register("gem", _generated[1]);
            _resources.Register("potion", _generated[2]);

            _view.Resources = _resources;
        }

        private void AppendImagesPage()
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1>Gorseller</h1>");
            _sb.Append("<h2>Uc ikon kodla cizildi, tek bir atlasa paketlendi ve markup onlara ")
               .Append("adiyla ulasiyor. Sayfa yine tek draw call.</h2>");

            // Natural size: an img with no CSS size takes the texture's own.
            _sb.Append("<div style=\"display:flex;margin-top:6px\">");
            IconCell("coin", "coin", "64x64");
            IconCell("gem", "gem", "64x64");
            IconCell("potion", "potion", "64x64");
            _sb.Append("</div>");

            _sb.Append("<p class=\"muted\" style=\"margin:14px 0 6px 0\">CSS ile olceklenmis ")
               .Append("(ayni doku, ayni atlas):</p>");

            _sb.Append("<div style=\"display:flex;align-items:flex-end\">");
            foreach (int px in new[] { 24, 40, 64, 96 })
            {
                _sb.Append("<div style=\"width:110px\">")
                   .Append("<img src=\"gem\" style=\"width:").Append(px).Append("px;height:").Append(px)
                   .Append("px\">")
                   .Append("<div class=\"muted\" style=\"font-size:11px\">").Append(px).Append(" px</div></div>");
            }
            _sb.Append("</div>");

            _sb.Append("</div>");

            // --- inventory grid -------------------------------------------------
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1 style=\"font-size:16px\">Envanter</h1>");
            _sb.Append("<h2>Slotlar flex ile diziliyor. inline-block ile denemeyin: litehtml ")
               .Append("vertical-align'i yok sayiyor ve dolu slot bos olandan asagi kayiyor.</h2>");

            _sb.Append("<div style=\"display:flex;margin-top:4px\">");
            Slot("slot-1", "coin", "128");
            Slot("slot-2", "gem", "7");
            Slot("slot-3", "potion", "3");
            Slot("slot-4", null, null);
            _sb.Append("</div>");

            _sb.Append("<p class=\"muted\" style=\"margin:12px 0 0 0\">Her slotun bir id'si var; ")
               .Append("ElementAt bir noktadaki slotu, TryGetElementRect kutusunu soyluyor. ")
               .Append("Surukle-birak icin gereken ikisi de bu.</p>");
            _sb.Append("</div>");
        }

        /// <summary>One labelled icon at its natural size.</summary>
        private void IconCell(string name, string label, string note)
        {
            _sb.Append("<div style=\"width:110px\">")
               .Append("<img src=\"").Append(name).Append("\">")
               .Append("<div style=\"font-size:12px\">").Append(label).Append("</div>")
               .Append("<div class=\"muted\" style=\"font-size:11px\">").Append(note).Append("</div>")
               .Append("</div>");
        }

        /// <summary>An inventory slot, optionally holding an item and a count.</summary>
        private void Slot(string id, string icon, string count)
        {
            _sb.Append("<div id=\"").Append(id)
               .Append("\" style=\"width:72px;height:72px;margin-right:10px;border-radius:10px;")
               .Append("background:#12172a;border:1px solid #232b45\">");

            if (icon != null)
            {
                _sb.Append("<img src=\"").Append(icon).Append("\" style=\"width:48px;height:48px;margin:8px\">")
                   .Append("<div class=\"muted\" style=\"font-size:11px;margin:-10px 0 0 8px\">x")
                   .Append(count).Append("</div>");
            }

            _sb.Append("</div>");
        }

        private void AppendAboutPage()
        {
            _sb.Append("<div class=\"card\">");
            _sb.Append("<h1>Hakkinda</h1>");
            _sb.Append("<h2>Gomulu tarayici yok.</h2>");

            _sb.Append("<p style=\"margin:0 0 8px 0\">litehtml yalnizca parse ve layout yapar; ")
               .Append("hicbir sey cizmez. Cizim cagrilarini duz bir quad akisina cevirip ")
               .Append("tek mesh olarak GPU'ya veriyoruz.</p>");

            _sb.Append("<table>");
            Row("litehtml", "BSD-3-Clause");
            Row("gumbo-parser", "Apache-2.0");
            Row("stb_truetype", "public domain");
            Row("Platformlar", "macOS dogrulandi, Android hazir");
            _sb.Append("</table>");

            _sb.Append("</div>");
        }

        /// <summary>
        /// Rewrites the overview page's live values without re-parsing it. Every
        /// value that can change between frames is listed here; anything missing
        /// would silently freeze at whatever the last full rebuild wrote.
        /// </summary>
        private void RefreshOverviewStats()
        {
            _view.SetText("#stat-fps", F(_smoothedFps, "0"));
            _view.SetText("#stat-quad", _view.QuadCount.ToString(CultureInfo.InvariantCulture));
            _view.SetText("#stat-cpu", F(_view.TotalMs, "0.00") + " ms");
            _view.SetText("#stat-rebuild", _rebuilds.ToString(CultureInfo.InvariantCulture));

            _view.SetText("#row-height", F(_view.DocumentHeight, "0") + " px");
            _view.SetText("#row-surface", _view.Texture != null
                ? $"{_view.Texture.width}x{_view.Texture.height}"
                : "-");
            _view.SetText("#row-limiter", LimiterLabel());
            _view.SetText("#row-link", _lastLink);
        }

        private void Stat(string key, string value, string id = null)
        {
            _sb.Append("<span class=\"stat\"><span class=\"k\">").Append(key)
               .Append("</span><span class=\"v\"");
            if (id != null)
            {
                _sb.Append(" id=\"").Append(id).Append("\"");
            }
            _sb.Append(">").Append(value).Append("</span></span>");
        }

        private void Row(string key, string value, string id = null)
        {
            _sb.Append("<tr><td>").Append(key).Append("</td><td class=\"r\"");
            if (id != null)
            {
                _sb.Append(" id=\"").Append(id).Append("\"");
            }
            _sb.Append(">").Append(value).Append("</td></tr>");
        }
    }
}
