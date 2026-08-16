using System;
using System.Collections.Generic;
using Unity.Collections;
using UnityEngine;

namespace Doctype
{
    /// <summary>
    /// Renders an HTML document into a RenderTexture you can use anywhere:
    /// a RawImage, a world-space quad, a material slot.
    /// </summary>
    /// <remarks>
    /// Layout runs on the CPU in litehtml; everything visible is drawn by the
    /// GPU from an analytic quad stream. Nothing is rasterized per-pixel on the
    /// CPU except glyph coverage, which is cached in an atlas.
    /// </remarks>
    [ExecuteAlways]
    [AddComponentMenu("Doctype/View")]
    public class HtmlView : MonoBehaviour
    {
        [Header("Content")]
        [Tooltip("Takes precedence over the inline HTML below when assigned.")]
        [SerializeField] private TextAsset _htmlAsset;

        [TextArea(4, 20)]
        [SerializeField] private string _html =
            "<body style='font-family:sans-serif;padding:16px'><h1>Doctype</h1><p>Hello from Unity.</p></body>";

        [TextArea(2, 10)]
        [SerializeField] private string _userCss = "";

        [Header("Surface")]
        [SerializeField] private Vector2Int _size = new Vector2Int(800, 600);

        [Tooltip("Grow the texture vertically to fit the laid-out document.")]
        [SerializeField] private bool _autoHeight;

        [Tooltip("Cleared to before the page is drawn. Premultiplied: the surface composites " +
                 "with Blend One OneMinusSrcAlpha, so a colour has to be scaled by its own " +
                 "alpha. Transparent is (0,0,0,0), not white at zero alpha — that one adds " +
                 "white to everything behind wherever the page paints nothing.")]
        [SerializeField] private Color _background = new Color(0f, 0f, 0f, 0f);

        [Tooltip("Multiplies CSS pixels, like a browser's devicePixelRatio.")]
        [SerializeField, Range(0.5f, 4f)] private float _deviceScale = 1f;

        [Header("Fonts")]
        [Tooltip("Fall back to fonts installed on the platform when the list below is empty.")]
        [SerializeField] private bool _useSystemFonts = true;

        [SerializeField] private List<HtmlFontEntry> _fonts = new List<HtmlFontEntry>();

        [SerializeField] private string _defaultFontFamily = "sans-serif";
        [SerializeField] private float _defaultFontSize = 16f;

        [Header("Performance")]
        [Tooltip("litehtml re-parses its default stylesheet for every document, and that is the single " +
                 "biggest cost of a rebuild. The trimmed sheet drops form controls and legacy tags.")]
        [SerializeField] private HtmlMasterStylesheet _masterStylesheet = HtmlMasterStylesheet.GameUI;

        private readonly Dictionary<string, Action<HtmlElementClick>> _pendingBindings =
            new Dictionary<string, Action<HtmlElementClick>>();

        private HtmlDocument _document;
        private HtmlRenderer _renderer;
        private RenderTexture _target;
        private Color _lastBackground;

        // Parsing, laying out and drawing are tracked separately: re-parsing on
        // every resize would be wasteful and would also drop hover/active state.
        private bool _needsReload = true;
        private bool _needsLayout = true;
        private bool _needsRender = true;
        private int _resourceVersion = -1;
        private Vector2Int _laidOutFor;
        private float _laidOutScale;

        // Pointer state has to be re-applied after a re-parse. litehtml decides
        // a click happened when press and release land on the *same element
        // instance*, and re-parsing throws those instances away — so without
        // this, any click spanning more than one rebuild is silently lost, and
        // :hover flickers off every frame.
        private Vector2 _lastPointer;
        private bool _hasPointer;
        private bool _pointerDown;
        private bool _documentWasReparsed;

        // Re-parsing is ~97% of a rebuild's CPU cost, and almost all of that is
        // litehtml re-parsing its own default stylesheet — a fixed price paid
        // per document, regardless of how small the page is. A page that
        // regenerates identical markup (a UI that only changes when something
        // happens) should never pay it.
        private string _loadedHtml;
        private string _loadedCss;
        private bool _forceReload;

        /// <summary>Reloads skipped because the markup was unchanged.</summary>
        public int SkippedReloads { get; private set; }

        // Real measurements rather than guesses: animating means re-parsing and
        // re-laying-out every frame, and you need to know what that costs.
        // Fully qualified: System.Diagnostics.Debug would otherwise collide
        // with UnityEngine.Debug.
        private readonly System.Diagnostics.Stopwatch _stopwatch = new System.Diagnostics.Stopwatch();

        /// <summary>Fires when a link inside the document is clicked.</summary>
        public event Action<string> AnchorClicked;

        /// <summary>Fires when the CSS cursor under the pointer changes.</summary>
        public event Action<string> CursorChanged;

        /// <summary>
        /// Fires when a non-anchor element is clicked, innermost first.
        /// </summary>
        /// <remarks>
        /// Use <see cref="BindClick"/> when you know the element's id; this
        /// event is for cases where you want to inspect tag, class or
        /// data-action instead.
        /// </remarks>
        public event Action<HtmlElementClick> ElementClicked;

        /// <summary>The rendered surface. Recreated when the size changes.</summary>
        public RenderTexture Texture => _target;

        public HtmlDocument Document => _document;

        /// <summary>Height of the laid-out document, which can exceed the texture height.</summary>
        public float DocumentHeight => _document?.DocumentHeight ?? 0f;

        /// <summary>Quads drawn in the last frame — a direct measure of GPU cost.</summary>
        public int QuadCount => _renderer?.LastQuadCount ?? 0;

        /// <summary>Kilobytes of vertex data uploaded over this view's lifetime.
        /// With the persistent mesh this grows by the changed span, not by the
        /// page; the benchmark's vertex column is the per-frame difference.</summary>
        public double UploadedKbTotal => _renderer?.UploadedKbTotal ?? 0d;

        /// <summary>How much of the surface the last render repainted.</summary>
        public HtmlDirtyMode LastDirtyMode => _renderer?.LastDirtyMode ?? HtmlDirtyMode.Full;

        /// <summary>Pixels the last partial repaint touched, for instrumentation.</summary>
        public RectInt LastDirtyPixels => _renderer?.LastDirtyPixels ?? default;

        /// <summary>
        /// One of the native retained display list's counters, or -1 when there
        /// is no document. For instrumentation only — see HtmlBenchmark.
        /// </summary>
        public long QuadCacheStat(HtmlNative.QuadCacheStat which) =>
            _document?.QuadCacheStat(which) ?? -1L;

        /// <summary>Milliseconds spent parsing the markup in the last reload.</summary>
        public float ParseMs { get; private set; }

        /// <summary>Milliseconds spent in litehtml's layout pass.</summary>
        public float LayoutMs { get; private set; }

        /// <summary>Milliseconds spent recording quads and issuing the draw.</summary>
        public float DrawMs { get; private set; }

        /// <summary>Total CPU cost of the last full rebuild.</summary>
        public float TotalMs => ParseMs + LayoutMs + DrawMs;

        // ParseMs/LayoutMs/DrawMs are "the last time this ran", and they keep
        // reporting it on every frame where it did not. That is what you want on
        // a stats panel and exactly wrong for a benchmark, which needs to know
        // how much work a frame actually did: a HUD that changes nothing would
        // otherwise report the cost of the load it did once, forever.
        //
        // These are the honest version. Monotonic, so a caller samples them at
        // both ends of a window and divides.

        /// <summary>Documents parsed since this view was created.</summary>
        public int ReloadCount { get; private set; }

        /// <summary>Layout passes run since this view was created.</summary>
        public int LayoutCount { get; private set; }

        /// <summary>Frames recorded and drawn since this view was created.</summary>
        public int RenderCount { get; private set; }

        /// <summary>Milliseconds spent parsing, over the view's whole lifetime.</summary>
        public double ParseMsTotal { get; private set; }

        /// <summary>Milliseconds spent in layout, over the view's whole lifetime.</summary>
        public double LayoutMsTotal { get; private set; }

        /// <summary>Milliseconds spent recording and drawing, over the view's whole lifetime.</summary>
        public double DrawMsTotal { get; private set; }

        /// <summary>
        /// Current glyph atlas dimensions.
        /// </summary>
        /// <remarks>
        /// Worth surfacing: an atlas that keeps growing on a page that is not
        /// adding new characters means glyphs are being re-packed rather than
        /// reused, and once it hits its ceiling text stops being drawn at all.
        /// </remarks>
        public Vector2Int FontAtlasSize { get; private set; }

        public IHtmlResourceProvider Resources
        {
            get => _document != null ? _document.Resources : _pendingResources;
            set
            {
                // Remembered rather than dropped: the document is not built until
                // OnEnable, and callers normally wire this up from Awake, which
                // runs first. Silently discarding it there produced a page that
                // laid images out at zero size with no error anywhere.
                _pendingResources = value;

                if (_document != null)
                {
                    _document.Resources = value;
                }
            }
        }

        private IHtmlResourceProvider _pendingResources;

        private void OnEnable()
        {
            Initialize();
        }

        private void OnDisable()
        {
            Teardown();
        }

        private void OnValidate()
        {
            _size.x = Mathf.Max(1, _size.x);
            _size.y = Mathf.Max(1, _size.y);

            // Inspector edits can touch the markup itself, so re-parse.
            _needsReload = true;
            _forceReload = true;
        }

        private void Initialize()
        {
            Teardown();

            try
            {
                _document = new HtmlDocument();

                if (_pendingResources != null)
                {
                    _document.Resources = _pendingResources;
                }
            }
            catch (Exception e)
            {
                Debug.LogError($"[Doctype] native plugin unavailable: {e.Message}", this);
                return;
            }

            _document.AnchorClicked += url => AnchorClicked?.Invoke(url);
            _document.CursorChanged += cursor => CursorChanged?.Invoke(cursor);
            _document.ElementClicked += click => ElementClicked?.Invoke(click);

            // Bindings registered before the document existed (or before a
            // re-init) would otherwise be silently dropped.
            foreach (KeyValuePair<string, Action<HtmlElementClick>> pair in _pendingBindings)
            {
                _document.BindClick(pair.Key, pair.Value);
            }

            _document.SetMasterStylesheet(_masterStylesheet);

            RegisterFonts();

            _renderer = new HtmlRenderer();

            _loadedHtml = null;
            _loadedCss = null;
            _needsReload = true;
            _forceReload = true;
        }

        private void Teardown()
        {
            _renderer?.Dispose();
            _renderer = null;

            _document?.Dispose();
            _document = null;

            if (_target != null)
            {
                _target.Release();
                DestroySafely(_target);
                _target = null;
            }
        }

        private static void DestroySafely(UnityEngine.Object obj)
        {
            if (obj == null)
            {
                return;
            }

            if (Application.isPlaying)
            {
                Destroy(obj);
            }
            else
            {
                DestroyImmediate(obj);
            }
        }

        private void RegisterFonts()
        {
            int registered = 0;

            foreach (HtmlFontEntry entry in _fonts)
            {
                byte[] data = entry?.Resolve();
                if (data == null)
                {
                    continue;
                }

                if (_document.RegisterFont(entry.Family, data, entry.Weight, entry.Italic))
                {
                    registered++;
                }
                else
                {
                    Debug.LogWarning($"[Doctype] could not parse font '{entry.Family}'", this);
                }
            }

            if (registered == 0 && _useSystemFonts)
            {
                foreach (HtmlFontEntry entry in HtmlSystemFonts.Discover())
                {
                    byte[] data = entry.Resolve();
                    if (data != null && _document.RegisterFont(entry.Family, data, entry.Weight, entry.Italic))
                    {
                        registered++;
                    }
                }
            }

            if (registered == 0)
            {
                Debug.LogError(
                    "[Doctype] no fonts registered. Assign fonts on the component (as .bytes TextAssets) " +
                    "or enable Use System Fonts on a platform that has them.", this);
                return;
            }

            _document.SetDefaultFont(_defaultFontFamily, _defaultFontSize);
        }

        /// <summary>
        /// Runs <paramref name="handler"/> when the element with this id is
        /// clicked. Survives reloads and component re-initialisation.
        /// </summary>
        /// <example>
        /// <code>
        /// view.BindClick("play", _ => Debug.Log("Play basildi"));
        /// view.LoadHtml("&lt;button id='play'&gt;Oyna&lt;/button&gt;");
        /// </code>
        /// </example>
        public void BindClick(string elementId, Action<HtmlElementClick> handler)
        {
            if (string.IsNullOrEmpty(elementId))
            {
                return;
            }

            if (handler == null)
            {
                _pendingBindings.Remove(elementId);
            }
            else
            {
                _pendingBindings[elementId] = handler;
            }

            _document?.BindClick(elementId, handler);
        }

        /// <summary>Removes every id binding.</summary>
        public void ClearClickBindings()
        {
            _pendingBindings.Clear();
            _document?.ClearClickBindings();
        }

        /// <summary>Replaces the document content and schedules a relayout.</summary>
        public void LoadHtml(string html, string userCss = null)
        {
            _html = html;
            _htmlAsset = null;

            if (userCss != null)
            {
                _userCss = userCss;
            }

            _needsReload = true;
        }

        /// <summary>
        /// Rewrites the text of one element and schedules the layout that has to
        /// follow it, without re-parsing the document. Returns false when the
        /// text was already what you asked for, so the caller can do nothing.
        /// </summary>
        /// <remarks>
        /// This is the path for values that change every frame -- a score, a
        /// counter, a timer. Rebuilding the markup and calling
        /// <see cref="LoadHtml"/> instead re-parses the whole page and re-runs
        /// selector matching, which on a mid-range phone is milliseconds.
        /// <para>
        /// Only text is covered: changing a class, an attribute or the set of
        /// elements still needs a full reload. Returns false, leaving the page
        /// untouched, when the selector matches nothing or the element holds
        /// child elements rather than only text.
        /// </para>
        /// </remarks>
        /// <summary>
        /// Replaces the inline style of one element without re-parsing, and
        /// schedules the layout that must follow. Returns false when the style
        /// was already what you asked for.
        /// </summary>
        /// <remarks>
        /// The path for animating a bar's height, a gradient's angle or a
        /// colour. Unlike <see cref="SetText"/> it can never skip layout — a
        /// declaration has no measured size to compare against — so the cost
        /// scales with the mutated element's subtree. Animate a leaf, not the
        /// container a long list hangs off.
        /// </remarks>
        public bool SetStyle(string selector, string css)
        {
            if (_document == null || !_document.SetStyle(selector, css))
            {
                return false;
            }

            ParseMs = 0f;
            _needsLayout = true;
            return true;
        }

        public bool SetText(string selector, string text)
        {
            if (_document == null || !_document.SetText(selector, text))
            {
                return false;
            }

            // Nothing was parsed this frame, and the stat pages read this.
            ParseMs = 0f;
            _needsLayout = true;
            return true;
        }

        /// <summary>
        /// Re-parses, lays out and draws on the next update, even if the markup
        /// is unchanged.
        /// </summary>
        public void MarkDirty()
        {
            _needsReload = true;
            _forceReload = true;
        }

        /// <summary>Lays out and draws again without re-parsing, keeping hover state.</summary>
        public void MarkLayoutDirty()
        {
            _needsLayout = true;
        }

        /// <summary>
        /// How many surface pixels one CSS pixel covers — a browser's
        /// devicePixelRatio. Raising it renders the same layout at a higher
        /// resolution rather than making everything smaller.
        /// </summary>
        /// <remarks>
        /// Pair this with <see cref="SetSize"/>: the CSS viewport the document
        /// lays out against is the surface size divided by this, so setting the
        /// surface to the real pixel count and this to the display's scale keeps
        /// a layout authored in CSS pixels the same physical size on any screen.
        /// <para>
        /// Accepts up to 8 rather than the inspector slider's 4, because a
        /// high-DPI phone against a small reference resolution can legitimately
        /// land above it.
        /// </para>
        /// </remarks>
        public float DeviceScale
        {
            get => _deviceScale;
            set
            {
                float wanted = Mathf.Clamp(value, 0.5f, 8f);
                if (!Mathf.Approximately(wanted, _deviceScale))
                {
                    _deviceScale = wanted;
                    _needsLayout = true;
                }
            }
        }

        /// <summary>
        /// Grows the surface vertically to whatever the document laid out to,
        /// so a panel is exactly as tall as its content.
        /// </summary>
        /// <remarks>
        /// The width still comes from <see cref="SetSize"/>; only the height is
        /// taken over. A host that positions the surface itself has to read
        /// <see cref="Texture"/>'s height back, because nothing here touches a
        /// RectTransform.
        /// </remarks>
        public bool AutoHeight
        {
            get => _autoHeight;
            set
            {
                if (_autoHeight != value)
                {
                    _autoHeight = value;
                    _needsLayout = true;
                }
            }
        }

        /// <summary>
        /// Resizes the surface. Layout re-runs, but the parsed document (and
        /// with it hover and active state) is kept.
        /// </summary>
        public void SetSize(int width, int height)
        {
            var wanted = new Vector2Int(Mathf.Max(1, width), Mathf.Max(1, height));
            if (wanted == _size)
            {
                return;
            }

            _size = wanted;
            _needsLayout = true;
        }

        private void LateUpdate()
        {
            // Newly-arrived images change layout, so watch the provider too.
            int providerVersion = _document?.Resources?.Version ?? 0;
            if (providerVersion != _resourceVersion)
            {
                // An image finished loading, so its box has a real size now.
                _resourceVersion = providerVersion;
                _needsLayout = true;

                // ...and its UVs may have moved, which relayout alone does NOT
                // cover. Native retains a display list keyed on geometry: if the
                // provider repacked its atlas without any box changing size, the
                // relayout below finds nothing dirty and the record replays
                // cached quads carrying the *old* UVs. A version bump is the
                // only thing that can tell native its image quads are stale, so
                // it has to drop the retained frame as well as re-lay-out.
                //
                // Deliberately unconditional. Distinguishing "an image loaded"
                // from "the atlas repacked" would need a second counter on the
                // provider interface; a version bump is rare (it means content
                // arrived), and the cost of being wrong is one full redraw.
                _document?.InvalidateDrawCache();
            }

            if (_size != _laidOutFor || !Mathf.Approximately(_deviceScale, _laidOutScale))
            {
                _needsLayout = true;
            }

            if (_needsReload)
            {
                RunReload();
            }

            if (_needsLayout)
            {
                RunLayout();
            }

            if (_needsRender)
            {
                RunRender();
            }
        }

        private void RunReload()
        {
            if (_document == null || !_document.IsValid)
            {
                return;
            }

            _needsReload = false;

            string html = CurrentHtml();
            string css = string.IsNullOrEmpty(_userCss) ? null : _userCss;

            // Identical markup produces an identical document, so skipping the
            // parse is not just faster — it also keeps hover, active and scroll
            // state that a fresh parse would throw away.
            if (!_forceReload && _loadedHtml != null && string.Equals(html, _loadedHtml, StringComparison.Ordinal) &&
                string.Equals(css, _loadedCss, StringComparison.Ordinal))
            {
                SkippedReloads++;
                ParseMs = 0f;
                return;
            }

            _forceReload = false;

            _stopwatch.Restart();
            bool ok = _document.LoadHtml(html, css);
            _stopwatch.Stop();
            ParseMs = (float)_stopwatch.Elapsed.TotalMilliseconds;

            ReloadCount++;
            ParseMsTotal += ParseMs;

            if (!ok)
            {
                Debug.LogError($"[Doctype] {_document.LastError}", this);
                return;
            }

            _loadedHtml = html;
            _loadedCss = css;

            _documentWasReparsed = true;
            _needsLayout = true;
        }

        private string CurrentHtml()
        {
            return _htmlAsset != null ? _htmlAsset.text : _html;
        }

        private void RunLayout()
        {
            if (_document == null || !_document.IsValid)
            {
                return;
            }

            _needsLayout = false;

            float width = _size.x / _deviceScale;
            float height = _size.y / _deviceScale;

            _document.SetDeviceScale(_deviceScale);
            _document.SetViewport(width, height);

            float documentHeight = MeasuredLayout(width);

            // Restoring the pointer can itself dirty layout: the re-applied
            // :hover may resize what it lands on. That has to be answered
            // before this frame renders, not next frame — the whole point of
            // the dirty contract is that stale geometry never reaches a draw.
            // Hit testing needs the laid-out tree, hence after the first pass.
            if (_documentWasReparsed)
            {
                _documentWasReparsed = false;

                if ((RestorePointerState() & HtmlDirty.Layout) != 0)
                {
                    documentHeight = MeasuredLayout(width);
                }
            }

            if (_autoHeight)
            {
                int wanted = Mathf.Max(1, Mathf.CeilToInt(documentHeight * _deviceScale));
                if (wanted != _size.y)
                {
                    _size.y = wanted;
                }
            }

            _laidOutFor = _size;
            _laidOutScale = _deviceScale;

            _needsRender = true;
        }

        /// <summary>
        /// Re-applies the pointer to a freshly parsed document, and reports what
        /// doing so dirtied.
        /// </summary>
        /// <remarks>
        /// Hit testing needs a laid-out tree, so this runs after layout. Sending
        /// the press again matters as much as the move: litehtml only reports a
        /// click when the release finds the same element still marked active.
        /// </remarks>
        private HtmlDirty RestorePointerState()
        {
            if (!_hasPointer)
            {
                return HtmlDirty.None;
            }

            HtmlDirty dirty = _document.MouseMove(_lastPointer);

            if (_pointerDown)
            {
                dirty |= _document.MouseDown(_lastPointer);
            }

            return dirty;
        }

        /// <summary>One layout pass, with the bookkeeping the benchmark reads.</summary>
        private float MeasuredLayout(float width)
        {
            _stopwatch.Restart();
            float documentHeight = _document.Layout(width);
            _stopwatch.Stop();
            LayoutMs = (float)_stopwatch.Elapsed.TotalMilliseconds;

            LayoutCount++;
            LayoutMsTotal += LayoutMs;

            return documentHeight;
        }

        /// <summary>
        /// Returns true when the surface was (re)created, because a fresh
        /// target holds garbage where the renderer expects the previous frame.
        /// </summary>
        private bool EnsureTarget()
        {
            if (_target != null && _target.width == _size.x && _target.height == _size.y)
            {
                return false;
            }

            if (_target != null)
            {
                _target.Release();
                DestroySafely(_target);
            }

            // sRGB so the GPU converts the shader's linear output back to the
            // byte values a browser would produce.
            _target = new RenderTexture(_size.x, _size.y, 0, RenderTextureFormat.ARGB32,
                                        RenderTextureReadWrite.sRGB)
            {
                name = $"Doctype {name}",
                hideFlags = HideFlags.HideAndDontSave,
                filterMode = FilterMode.Bilinear,
                wrapMode = TextureWrapMode.Clamp,
                useMipMap = false,
                autoGenerateMips = false,
            };

            _target.Create();
            return true;
        }

        private void RunRender()
        {
            if (_document == null || !_document.IsValid || _renderer == null)
            {
                return;
            }

            _needsRender = false;

            // Partial repaint leans on the target still holding the previous
            // frame. A recreated target holds garbage, a target the platform
            // discarded (Android after an app pause) holds less than that, and
            // a changed background falsifies every pixel outside the dirty
            // rect -- each of those forces one full repaint.
            bool forceFull = EnsureTarget();

            if (!_target.IsCreated())
            {
                _target.Create();
                forceFull = true;
            }

            if (_background != _lastBackground)
            {
                _lastBackground = _background;
                forceFull = true;
            }

            _stopwatch.Restart();

            NativeArray<HtmlQuad> quads = _document.Record(out HtmlFrame frame);

            // The document is laid out in CSS pixels and mapped onto the full
            // target, which is what makes deviceScale behave like a browser's
            // devicePixelRatio.
            var documentSize = new Vector2(_size.x / _deviceScale, _size.y / _deviceScale);

            _renderer.Render(quads, frame, _target, _background, documentSize,
                             _document.Resources?.ImageAtlas, forceFull);

            FontAtlasSize = new Vector2Int(frame.FontAtlasWidth, frame.FontAtlasHeight);

            _stopwatch.Stop();
            DrawMs = (float)_stopwatch.Elapsed.TotalMilliseconds;

            RenderCount++;
            DrawMsTotal += DrawMs;
        }

        // --- input -------------------------------------------------------------

        /// <summary>
        /// Id of the topmost element at a document point, or null. Changes no
        /// hover state, so it is safe to call every frame during a drag.
        /// </summary>
        public string ElementAt(Vector2 documentPoint)
        {
            return _document?.ElementAt(documentPoint);
        }

        /// <summary>
        /// Painted rectangle of the first element matching <paramref name="selector"/>,
        /// in CSS pixels, as of the last layout. Multiply by
        /// <see cref="DeviceScale"/> for surface pixels.
        /// </summary>
        public bool TryGetElementRect(string selector, out Rect rect)
        {
            rect = default;
            return _document != null && _document.TryGetElementRect(selector, out rect);
        }

        /// <summary>
        /// Converts a normalized point on the surface (0,0 bottom-left, as Unity
        /// UI reports it) into document space.
        /// </summary>
        public Vector2 NormalizedToDocument(Vector2 normalized)
        {
            float width = _size.x / _deviceScale;
            float height = _size.y / _deviceScale;

            // Document space has y growing downward.
            return new Vector2(normalized.x * width, (1f - normalized.y) * height);
        }

        /// <summary>
        /// Applies what an input event dirtied. Paint alone re-records and
        /// redraws; Layout also re-runs layout first, because native has just
        /// said the render tree's positions no longer match its styles — a
        /// :hover rule resized something. Treating that as Paint is how a
        /// button grows its hitbox but not its pixels.
        /// </summary>
        private void ApplyInputDirty(HtmlDirty dirty)
        {
            if (dirty == HtmlDirty.None)
            {
                return;
            }

            _needsRender = true;

            if ((dirty & HtmlDirty.Layout) != 0)
            {
                _needsLayout = true;
            }
        }

        public void PointerMove(Vector2 documentPoint)
        {
            _lastPointer = documentPoint;
            _hasPointer = true;

            if (_document != null)
            {
                ApplyInputDirty(_document.MouseMove(documentPoint));
            }
        }

        public void PointerDown(Vector2 documentPoint)
        {
            _lastPointer = documentPoint;
            _hasPointer = true;
            _pointerDown = true;

            if (_document != null)
            {
                ApplyInputDirty(_document.MouseDown(documentPoint));
            }
        }

        public void PointerUp(Vector2 documentPoint)
        {
            _lastPointer = documentPoint;
            _hasPointer = true;
            _pointerDown = false;

            if (_document != null)
            {
                ApplyInputDirty(_document.MouseUp(documentPoint));
            }
        }

        public void PointerExit()
        {
            _hasPointer = false;
            _pointerDown = false;

            if (_document != null)
            {
                ApplyInputDirty(_document.MouseLeave());
            }
        }

        /// <returns>True when an element inside the document consumed the scroll.</returns>
        public bool Scroll(Vector2 delta, Vector2 documentPoint)
        {
            if (_document == null)
            {
                return false;
            }

            int consumed = _document.Scroll(delta, documentPoint);
            if (consumed > 0)
            {
                _needsRender = true;
            }

            return consumed > 0;
        }

        /// <summary>
        /// Document language and culture ("tr", "tr-TR") for locale-aware
        /// text-transform. Set before loading the markup it should affect.
        /// </summary>
        public void SetLanguage(string language, string culture = "")
        {
            _document?.SetLanguage(language, culture);
        }

        // --- gamepad/keyboard focus ------------------------------------------

        /// <summary>
        /// Focuses the first element the selector matches (style it with
        /// :focus), or clears focus with null. Independent of the pointer:
        /// hover and :active stay whatever the pointer made them.
        /// </summary>
        public void SetFocus(string selector)
        {
            if (_document != null)
            {
                ApplyInputDirty(_document.SetFocus(selector));
            }
        }

        /// <summary>
        /// Moves focus in a direction across the document's tabindex-carrying
        /// elements: the author's data-nav-* override first, then the spatial
        /// metric. With nothing focused, enters at the top-left focusable.
        /// </summary>
        /// <returns>False when focus had nowhere to go and stayed put.</returns>
        public bool MoveFocus(HtmlNavDirection direction)
        {
            if (_document == null)
            {
                return false;
            }

            bool moved = _document.MoveFocus(direction, out HtmlDirty dirty);
            ApplyInputDirty(dirty);
            return moved;
        }

        /// <summary>
        /// Activates the focused element (or a selector's element): anchors
        /// raise <see cref="AnchorClicked"/> exactly like a real click.
        /// </summary>
        public bool Activate(string selector = null)
        {
            return _document != null && _document.Activate(selector);
        }

        /// <summary>
        /// Id of the focused element, "" when it has no id, null when nothing
        /// is focused.
        /// </summary>
        public string FocusedId => _document?.FocusedId;
    }
}
