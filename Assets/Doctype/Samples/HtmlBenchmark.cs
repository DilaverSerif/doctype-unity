using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using UnityEngine;
using UnityEngine.UI;

namespace Doctype.Samples
{
    /// <summary>
    /// Measures what a litehtml HUD costs a running game, on the device.
    /// </summary>
    /// <remarks>
    /// The question this answers is not "how fast is the renderer" — the native
    /// benchmark already answers that, and it answers it without a GPU. This one
    /// answers "how much does putting this on screen take away from the game",
    /// which is a different number and can only be measured with both running at
    /// once.
    /// <para>
    /// So every scenario runs over the same spinning-cube load, and every result
    /// is reported as a delta against a <c>game-only</c> pass with no surfaces at
    /// all. The absolute frame time depends on the device, the resolution and how
    /// hot the phone is; the delta is the part that belongs to the HUD.
    /// </para>
    /// <para>
    /// The scenarios are ordered by how much work they force, from a HUD that
    /// changes nothing to one that re-parses its whole document every frame. Real
    /// UI lives near the top of that list; the bottom is there to bound the worst
    /// case and to show what the caches are actually saving.
    /// </para>
    /// <para>
    /// VSync and the frame cap are turned off for the duration: capped at 60 fps
    /// every scenario measures 16.7 ms and the benchmark says nothing at all.
    /// </para>
    /// </remarks>
    [AddComponentMenu("Doctype/Samples/Benchmark")]
    public class HtmlBenchmark : MonoBehaviour
    {
        [Tooltip("Frames discarded before each scenario is measured, so shader warmup, " +
                 "the first layout and the atlas filling up do not land in the samples.")]
        [SerializeField] private int _warmupFrames = 90;

        [Tooltip("Frames measured per scenario.")]
        [SerializeField] private int _measureFrames = 300;

        [Tooltip("Slots per panel. The knob that decides how big a document each panel is.")]
        [SerializeField] private int _slotsPerPanel = 40;

        // --- scenarios ---------------------------------------------------------

        enum Churn
        {
            None,       // the HUD is up and nothing about it changes
            SetText,    // one text node rewritten per frame — a score, a timer
            SetStyle,   // one inline style rewritten per frame — a hover, a cooldown
            Scroll,     // the list is dragged
            Relayout,   // the surface resizes, so layout re-runs but the DOM stands
            Reload,     // the whole document is rebuilt from a string
        }

        readonly struct Scenario
        {
            public Scenario(string name, int panels, Churn churn, string note, int slots = 0,
                            float renderScale = 1f)
            {
                Name = name;
                Panels = panels;
                Churn = churn;
                Note = note;
                Slots = slots;
                RenderScale = renderScale;
            }

            public string Name { get; }
            public int Panels { get; }
            public Churn Churn { get; }
            public string Note { get; }

            /// <summary>Rows in the page, or 0 for the component's default.</summary>
            public int Slots { get; }

            /// <summary>Surface resolution multiplier; the layout is unaffected.</summary>
            public float RenderScale { get; }
        }

        static readonly Scenario[] Scenarios =
        {
            new Scenario("game-only", 0, Churn.None, "baseline, no surfaces"),
            new Scenario("hud-idle", 1, Churn.None, "one panel, nothing changes"),
            new Scenario("hud-settext", 1, Churn.SetText, "one text node per frame"),
            new Scenario("hud-setstyle", 1, Churn.SetStyle, "one inline style per frame"),
            new Scenario("hud-scroll", 1, Churn.Scroll, "scrolled every frame"),
            new Scenario("hud-relayout", 1, Churn.Relayout, "resized every frame"),
            new Scenario("hud-reload", 1, Churn.Reload, "re-parsed every frame"),
            new Scenario("hud-idle-x4", 4, Churn.None, "four panels, nothing changes"),

            // Two and three panels are not padding. The first run showed one
            // re-rendering panel costing 21 ms of GPU and four costing 32 --
            // nothing like four times as much -- which says most of that is a
            // fixed price paid once per frame rather than per panel. These rows
            // are what turns that reading into a shape.
            new Scenario("hud-settext-x2", 2, Churn.SetText, "two panels, one text node each"),
            new Scenario("hud-settext-x3", 3, Churn.SetText, "three panels, one text node each"),
            new Scenario("hud-settext-x4", 4, Churn.SetText, "four panels, one text node each"),

            // Same surface, same one-text-node change, almost nothing in the
            // document. If this still costs what the full page costs, the price
            // is the render-target switch and not the drawing -- and every
            // optimisation aimed at the quad stream would be aimed at the wrong
            // thing.
            new Scenario("hud-settext-tiny", 1, Churn.SetText, "one panel, near-empty document", slots: 2),

            // The same page at a quarter of the pixels. Quad count and fill move
            // together in every other row, so neither can be blamed for the
            // cost; this holds the quads and the vertex bytes exactly fixed and
            // cuts only the pixels. If the GPU time falls, the price is fill and
            // the fragment shader; if it holds, it is geometry.
            new Scenario("hud-settext-halfres", 1, Churn.SetText, "one panel at 0.5x resolution",
                         renderScale: 0.5f),

            new Scenario("hud-reload-x4", 4, Churn.Reload, "four panels re-parsed every frame"),

            // The baseline again, two minutes later. A phone that has been
            // rendering flat out for the whole run is not the phone that started
            // it: if this row does not match the first one, the run drifted --
            // thermal throttling, a governor step, another app waking up -- and
            // every delta in between inherits that drift.
            new Scenario("game-only-end", 0, Churn.None, "baseline again, to show drift"),
        };

        // --- one scenario's numbers -------------------------------------------

        class Result
        {
            public string Name;
            public string Note;
            public int Panels;
            public int Frames;

            public float FrameMsMedian;
            public float FrameMsP95;
            public float GpuMsMedian;
            public float CpuMsMedian;

            // Per measured frame, from the view's lifetime counters -- not a
            // median of "the last time it ran", which never goes back to zero.
            public float ParseMsPerFrame;
            public float LayoutMsPerFrame;
            public float DrawMsPerFrame;
            public float CpuMsPerFrame;

            // How often the work actually happened, per frame. An idle HUD
            // should be at zero, and if it is not, that is the finding.
            public float ReloadsPerFrame;
            public float LayoutsPerFrame;
            public float RendersPerFrame;

            public int QuadsMedian;

            // Measured vertex upload, from the builder's own byte counter.
            public float VertexKbPerUpload;
            public float VertexKbPerFrame;

            public float HeapKbPerFrame;
            public int Gen0Collections;

            public long CacheFramesFast;
            public long CacheFramesPartial;
            public long CacheFramesRebuild;
            public long CacheQuadsReplayed;
            public long CacheQuadsEmitted;
        }

        HtmlBenchmarkLoad _load;
        HtmlView[] _panels;
        HtmlRawImage[] _panelRaws;
        RectTransform[] _panelRects;
        Vector2[] _panelSizes;

        readonly List<Result> _results = new List<Result>();

        // Preallocated: a benchmark that allocates while sampling measures itself.
        float[] _frameMs;
        float[] _gpuMs;
        float[] _cpuMs;
        float[] _quads;
        float[] _scratch;

        readonly StringBuilder _sb = new StringBuilder(8192);
        WaitForEndOfFrame _endOfFrame;

        // Time.unscaledDeltaTime is clamped by Time.maximumDeltaTime and is a
        // frame-loop concept; this is a clock. For a benchmark that difference
        // is the whole point of the number.
        double _lastRealtime;

        string _reportPath;

        void Awake()
        {
            _load = FindAnyObjectByType<HtmlBenchmarkLoad>();
            _panels = GetComponentsInChildren<HtmlView>(true);

            _panelRects = new RectTransform[_panels.Length];
            _panelSizes = new Vector2[_panels.Length];
            _panelRaws = new HtmlRawImage[_panels.Length];

            for (int i = 0; i < _panels.Length; i++)
            {
                _panelRaws[i] = _panels[i].GetComponent<HtmlRawImage>();
                _panelRects[i] = (RectTransform)_panels[i].transform;
                _panelSizes[i] = _panelRects[i].sizeDelta;
                _panels[i].gameObject.SetActive(false);
            }

            Allocate(_measureFrames);

            // Both, or the cap wins and every scenario reports the same number.
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = -1;

            _endOfFrame = new WaitForEndOfFrame();
            _reportPath = Path.Combine(Application.persistentDataPath, "doctype-bench.csv");
        }

        void Allocate(int n)
        {
            _frameMs = new float[n];
            _gpuMs = new float[n];
            _cpuMs = new float[n];
            _quads = new float[n];
            _scratch = new float[n];
        }

        IEnumerator Start()
        {
            Debug.Log($"[bench] device={SystemInfo.deviceModel} gpu={SystemInfo.graphicsDeviceName} " +
                      $"api={SystemInfo.graphicsDeviceType} screen={Screen.width}x{Screen.height} " +
                      $"load={(_load != null ? _load.Cubes : 0)} cubes");

            // FrameTimingManager has to be running before the first scenario is
            // measured, not during it. Its per-scenario warmup is not enough:
            // the very first scenario is also the first thing the process ever
            // renders, and the baseline coming back "n/a" costs every GPU delta
            // in the report, since they are all differences against it.
            for (int i = 0; i < 120; i++)
            {
                GpuTiming(out _, out _);
                yield return _endOfFrame;
            }

            foreach (Scenario scenario in Scenarios)
            {
                yield return RunScenario(scenario);
            }

            WriteReport();
            ShowReport();
        }

        IEnumerator RunScenario(Scenario scenario)
        {
            Activate(scenario);

            // The first frames after a surface appears pay for shader variants,
            // the render texture and the glyph atlas. None of that repeats.
            for (int i = 0; i < _warmupFrames; i++)
            {
                ApplyChurn(scenario, i);
                yield return _endOfFrame;
            }

            ResetCacheCounters(scenario.Panels);
            SnapshotWork(scenario.Panels);

            // GetTotalAllocatedBytes is not in Unity's scripting profile, so the
            // heap is sampled instead: a net figure, since a collection inside
            // the window gives some of it back. The gen-0 count next to it is
            // what says whether that happened.
            long heapBefore = GC.GetTotalMemory(false);
            int gen0Before = GC.CollectionCount(0);

            _lastRealtime = Time.realtimeSinceStartupAsDouble;

            for (int i = 0; i < _measureFrames; i++)
            {
                ApplyChurn(scenario, i);
                yield return _endOfFrame;
                Sample(i, scenario.Panels);
            }

            _results.Add(Summarise(scenario, GC.GetTotalMemory(false) - heapBefore,
                                   GC.CollectionCount(0) - gen0Before));
            Debug.Log($"[bench] {scenario.Name}: {_results[_results.Count - 1].FrameMsMedian:0.00} ms/frame");
        }

        // --- the work under test ----------------------------------------------

        void Activate(Scenario scenario)
        {
            for (int i = 0; i < _panels.Length; i++)
            {
                bool on = i < scenario.Panels;

                if (_panels[i].gameObject.activeSelf != on)
                {
                    _panels[i].gameObject.SetActive(on);
                }

                if (on)
                {
                    // Back to the authored size: the relayout scenario leaves it
                    // wherever the last wobble put it.
                    _panelRects[i].sizeDelta = _panelSizes[i];

                    if (_panelRaws[i] != null)
                    {
                        _panelRaws[i].RenderScale = scenario.RenderScale;
                    }

                    _panels[i].LoadHtml(Page(i, 0, Slots(scenario)));
                }
            }
        }

        void ApplyChurn(Scenario scenario, int frame)
        {
            for (int i = 0; i < scenario.Panels; i++)
            {
                HtmlView view = _panels[i];

                switch (scenario.Churn)
                {
                    case Churn.None:
                        break;

                    case Churn.SetText:
                        view.SetText("#status", Counter(frame));
                        break;

                    case Churn.SetStyle:
                        // Walk the highlight so a different element is touched
                        // every frame; parking it on one would let the cache
                        // settle into a state real hover never reaches.
                        // The sweep, not the frame, decides the colour. Keying
                        // it off `frame & 1` looks right and is not: the slot
                        // count is even, so each slot is revisited on the same
                        // parity every time, SetStyle reports "already that" and
                        // the scenario measures nothing at all.
                        view.SetStyle("#slot" + (frame % Slots(scenario)),
                                      ((frame / Slots(scenario)) & 1) == 0
                                          ? "background:#16203a"
                                          : "background:#0d1322");
                        break;

                    case Churn.Scroll:
                        // Aimed at the scroll box by asking where it is, rather
                        // than at a plausible-looking point. A guessed point that
                        // lands on the header is taken by nothing, and the
                        // scenario then measures an idle HUD while reporting
                        // itself as scrolling — which is worse than failing.
                        if (view.TryGetElementRect("#scroller", out Rect box))
                        {
                            view.Scroll(new Vector2(0f, (frame & 31) < 16 ? 6f : -6f), box.center);
                        }
                        break;

                    case Churn.Relayout:
                        // Eight canvas units, not one: the surface is sized in
                        // device pixels, and on a small screen one canvas unit
                        // rounds away to nothing so the size never changes.
                        _panelRects[i].sizeDelta = _panelSizes[i] + new Vector2((frame & 1) * 8f, 0f);
                        break;

                    case Churn.Reload:
                        view.LoadHtml(Page(i, frame, Slots(scenario)));
                        break;
                }
            }
        }

        /// <summary>
        /// A page shaped like a real HUD panel: a header, a status line and a
        /// scrollable list of slots, each with an id so it can be addressed.
        /// </summary>
        int Slots(Scenario scenario) => scenario.Slots > 0 ? scenario.Slots : _slotsPerPanel;

        string Page(int panel, int frame, int slots)
        {
            _sb.Clear();
            _sb.Append(@"<style>
  html,body { margin:0; background:transparent; color:#e8ecf6;
              font:15px -apple-system,'Helvetica Neue',Arial,sans-serif; }
  .panel { background:#111726; border:2px solid #222b45; border-radius:12px; padding:12px; }
  h1 { font-size:20px; margin:0 0 2px 0; }
  #status { color:#7f8aa8; font-size:13px; margin:0 0 10px 0; }
  /* The scroll box is the panel itself, not a strip inside it: that keeps the
     page the size a real HUD panel is (everything that fits is laid out and
     drawn) while still giving the scroll scenario something to move. vh, not %,
     because the parent's height is auto and a percentage would resolve to auto
     and never clip. */
  #scroller { height:94vh; overflow:auto; }
  .row { border-radius:8px; background:#0d1322; border:1px solid #232d49;
         padding:8px 10px; margin-bottom:6px; }
  .k { color:#8e97b3; font-size:12px; }
  .v { color:#9fb4e8; }
</style>");

            _sb.Append("<div class=\"panel\">");
            _sb.Append("<h1>Panel ").Append(panel + 1).Append("</h1>");
            _sb.Append("<p id=\"status\">").Append(Counter(frame)).Append("</p>");
            _sb.Append("<div id=\"scroller\">");

            for (int i = 0; i < slots; i++)
            {
                _sb.Append("<div id=\"slot").Append(i).Append("\" class=\"row\">")
                   .Append("<span class=\"k\">slot ").Append(i).Append("</span> ")
                   .Append("<span class=\"v\">").Append((i * 37 + frame) % 1000).Append("</span>")
                   .Append("</div>");
            }

            _sb.Append("</div></div>");
            return _sb.ToString();
        }

        static string Counter(int frame) =>
            "frame " + frame.ToString(CultureInfo.InvariantCulture);

        // --- sampling ----------------------------------------------------------

        void Sample(int i, int panels)
        {
            double now = Time.realtimeSinceStartupAsDouble;
            _frameMs[i] = (float)((now - _lastRealtime) * 1000d);
            _lastRealtime = now;

            GpuTiming(out float cpu, out float gpu);
            _cpuMs[i] = cpu;
            _gpuMs[i] = gpu;

            float quads = 0f;

            for (int p = 0; p < panels; p++)
            {
                quads += _panels[p].QuadCount;
            }

            _quads[i] = quads;
        }

        /// <summary>
        /// Frame timings straight from the driver, in milliseconds, or -1 where
        /// the platform does not report them.
        /// </summary>
        /// <remarks>
        /// This is the only source here that can separate GPU cost from CPU cost.
        /// It needs a graphics API that exposes timer queries — Vulkan does,
        /// GLES3 often does not — so a -1 in the report is "the device would not
        /// say", not "zero".
        /// </remarks>
        static readonly FrameTiming[] Timings = new FrameTiming[1];

        static void GpuTiming(out float cpuMs, out float gpuMs)
        {
            cpuMs = -1f;
            gpuMs = -1f;

            FrameTimingManager.CaptureFrameTimings();

            if (FrameTimingManager.GetLatestTimings(1, Timings) < 1)
            {
                return;
            }

            cpuMs = Plausible((float)Timings[0].cpuFrameTime);
            gpuMs = Plausible((float)Timings[0].gpuFrameTime);
        }

        /// <summary>
        /// A frame time, or -1 for the values FrameTimingManager hands back
        /// before it has anything real to say.
        /// </summary>
        /// <remarks>
        /// It reports before its first timer query lands, and what it reports
        /// then is not a small error — the first scenario of a run came back
        /// with a median of 40,651,860 ms. A single outlier would have washed
        /// out of a 300-frame median; a majority of them does not, so they have
        /// to be dropped rather than averaged.
        /// </remarks>
        static float Plausible(float ms) => ms > 0f && ms < 1000f ? ms : -1f;

        // Lifetime totals at the start of the measured window; everything the
        // report says about CPU is the difference between these and the same
        // numbers at the end, divided by the frames in between.
        double _parseMs0, _layoutMs0, _drawMs0, _uploadKb0;
        int _reloads0, _layouts0, _renders0;

        void SnapshotWork(int panels)
        {
            _parseMs0 = _layoutMs0 = _drawMs0 = _uploadKb0 = 0d;
            _reloads0 = _layouts0 = _renders0 = 0;

            for (int p = 0; p < panels; p++)
            {
                _parseMs0 += _panels[p].ParseMsTotal;
                _layoutMs0 += _panels[p].LayoutMsTotal;
                _drawMs0 += _panels[p].DrawMsTotal;
                _uploadKb0 += _panels[p].UploadedKbTotal;
                _reloads0 += _panels[p].ReloadCount;
                _layouts0 += _panels[p].LayoutCount;
                _renders0 += _panels[p].RenderCount;
            }
        }

        void ResetCacheCounters(int panels)
        {
            for (int p = 0; p < panels; p++)
            {
                _panels[p].QuadCacheStat(HtmlNative.QuadCacheStat.Reset);
            }
        }

        Result Summarise(Scenario scenario, long heapBytes, int gen0)
        {
            var r = new Result
            {
                Name = scenario.Name,
                Note = scenario.Note,
                Panels = scenario.Panels,
                Frames = _measureFrames,

                FrameMsMedian = Percentile(_frameMs, 0.5f),
                FrameMsP95 = Percentile(_frameMs, 0.95f),
                GpuMsMedian = Percentile(_gpuMs, 0.5f),
                CpuMsMedian = Percentile(_cpuMs, 0.5f),


                QuadsMedian = Mathf.RoundToInt(Percentile(_quads, 0.5f)),
                HeapKbPerFrame = heapBytes / 1024f / _measureFrames,
                Gen0Collections = gen0,
            };

            double parse = 0d, layout = 0d, draw = 0d, uploadKb = 0d;
            int reloads = 0, layouts = 0, renders = 0;

            for (int p = 0; p < scenario.Panels; p++)
            {
                parse += _panels[p].ParseMsTotal;
                layout += _panels[p].LayoutMsTotal;
                draw += _panels[p].DrawMsTotal;
                uploadKb += _panels[p].UploadedKbTotal;
                reloads += _panels[p].ReloadCount;
                layouts += _panels[p].LayoutCount;
                renders += _panels[p].RenderCount;
            }

            float frames = _measureFrames;

            // Measured, not derived: the persistent mesh uploads the changed
            // span, so the page's full size no longer predicts the bytes.
            r.VertexKbPerFrame = (float)((uploadKb - _uploadKb0) / frames);
            r.VertexKbPerUpload = renders > _renders0
                ? (float)((uploadKb - _uploadKb0) / (renders - _renders0))
                : 0f;

            r.ParseMsPerFrame = (float)((parse - _parseMs0) / frames);
            r.LayoutMsPerFrame = (float)((layout - _layoutMs0) / frames);
            r.DrawMsPerFrame = (float)((draw - _drawMs0) / frames);
            r.CpuMsPerFrame = r.ParseMsPerFrame + r.LayoutMsPerFrame + r.DrawMsPerFrame;

            r.ReloadsPerFrame = (reloads - _reloads0) / frames;
            r.LayoutsPerFrame = (layouts - _layouts0) / frames;
            r.RendersPerFrame = (renders - _renders0) / frames;

            for (int p = 0; p < scenario.Panels; p++)
            {
                r.CacheFramesFast += _panels[p].QuadCacheStat(HtmlNative.QuadCacheStat.FramesFast);
                r.CacheFramesPartial += _panels[p].QuadCacheStat(HtmlNative.QuadCacheStat.FramesPartial);
                r.CacheFramesRebuild += _panels[p].QuadCacheStat(HtmlNative.QuadCacheStat.FramesRebuild);
                r.CacheQuadsReplayed += _panels[p].QuadCacheStat(HtmlNative.QuadCacheStat.QuadsReplayed);
                r.CacheQuadsEmitted += _panels[p].QuadCacheStat(HtmlNative.QuadCacheStat.QuadsEmitted);
            }

            return r;
        }

        /// <summary>
        /// A percentile over the samples that are real. Anything negative is a
        /// measurement that did not happen, and sorting those in would drag the
        /// median toward a number nothing ever reported.
        /// </summary>
        float Percentile(float[] samples, float p)
        {
            int n = 0;

            for (int i = 0; i < _measureFrames; i++)
            {
                if (samples[i] >= 0f)
                {
                    _scratch[n++] = samples[i];
                }
            }

            if (n == 0)
            {
                return -1f;
            }

            Array.Sort(_scratch, 0, n);
            return _scratch[Mathf.Clamp(Mathf.RoundToInt(p * (n - 1)), 0, n - 1)];
        }

        // --- reporting ---------------------------------------------------------

        /// <summary>
        /// The no-HUD frame time every other row is measured against. Found by
        /// name rather than by position, because there are two baselines and the
        /// second one is only there to be compared with the first.
        /// </summary>
        float Baseline => Find("game-only")?.FrameMsMedian ?? 0f;

        Result Find(string name)
        {
            foreach (Result r in _results)
            {
                if (r.Name == name)
                {
                    return r;
                }
            }

            return null;
        }

        /// <summary>
        /// True when the half-resolution row failed to hold the layout fixed,
        /// which makes it useless for the thing it exists to separate.
        /// </summary>
        /// <remarks>
        /// The row only means something if the page laid out identically and
        /// only the pixel count changed. DeviceScale bottoms out at 0.5, so on a
        /// display where the canvas already scales below 1 the multiplier gets
        /// clamped, the CSS viewport shrinks instead of the resolution, and the
        /// page reflows — same row, completely different measurement. Quad count
        /// is the check: it is exactly what a reflow changes.
        /// </remarks>
        bool RenderScaleRowIsInvalid()
        {
            Result full = Find("hud-settext"), half = Find("hud-settext-halfres");

            return full != null && half != null &&
                   Mathf.Abs(half.QuadsMedian - full.QuadsMedian) > full.QuadsMedian * 0.02f;
        }

        /// <summary>How far the machine moved under us between the two baselines.</summary>
        float Drift()
        {
            Result first = Find("game-only"), last = Find("game-only-end");
            return first != null && last != null ? last.FrameMsMedian - first.FrameMsMedian : 0f;
        }

        /// <summary>
        /// True when this scenario did real work that the frame time could not
        /// see, because the frame had budget to spare.
        /// </summary>
        /// <remarks>
        /// Worth saying out loud per row rather than leaving it in the numbers.
        /// A frame paced to 33 ms absorbs anything under 33 ms without moving,
        /// so a HUD that costs 6 ms of CPU reports "+0.01 ms" — and a zero that
        /// means "we could not tell" reads exactly like a zero that means "it
        /// was free". The CPU columns next to it are still honest; they come
        /// from the engine's own stopwatches, not from frame pacing.
        /// <para>
        /// Checked per scenario, not across all of them: a run where nine rows
        /// are absorbed and the tenth finally blows the budget is the normal
        /// shape of this, and a whole-run check would call that "not locked".
        /// </para>
        /// </remarks>
        bool Absorbed(Result r) =>
            r.CpuMsPerFrame > 0.5f && r.FrameMsMedian - Baseline < 0.05f;

        int AbsorbedCount()
        {
            int n = 0;

            foreach (Result r in _results)
            {
                if (Absorbed(r))
                {
                    n++;
                }
            }

            return n;
        }

        void WriteReport()
        {
            var csv = new StringBuilder(4096);

            csv.Append("# device,").Append(SystemInfo.deviceModel)
               .Append("\n# gpu,").Append(SystemInfo.graphicsDeviceName)
               .Append("\n# api,").Append(SystemInfo.graphicsDeviceType)
               .Append("\n# screen,").Append(Screen.width).Append('x').Append(Screen.height)
               .Append("\n# load_cubes,").Append(_load != null ? _load.Cubes : 0)
               .Append("\n# frames_per_scenario,").Append(_measureFrames)
               .Append("\n# rows_absorbed_by_frame_budget,").Append(AbsorbedCount())
               .Append("\n# baseline_drift_ms,").Append(D(Drift()))
               .Append("\n# halfres_row_valid,").Append(RenderScaleRowIsInvalid() ? "no (reflowed)" : "yes")
               .Append("\n# slots_per_panel,").Append(_slotsPerPanel)
               .Append('\n');

            csv.Append("scenario,panels,frame_ms_p50,frame_ms_p95,hud_cost_ms,gpu_ms_p50,cpu_ms_p50,")
               .Append("litehtml_cpu_ms_per_frame,parse_ms_per_frame,layout_ms_per_frame,draw_ms_per_frame,")
               .Append("reloads_per_frame,layouts_per_frame,renders_per_frame,")
               .Append("quads,vertex_kb_per_upload,vertex_kb_per_frame,heap_kb_per_frame,gc0,absorbed,")
               .Append("cache_fast,cache_partial,cache_rebuild,quads_replayed,quads_emitted\n");

            foreach (Result r in _results)
            {
                csv.Append(r.Name).Append(',').Append(r.Panels).Append(',')
                   .Append(F(r.FrameMsMedian)).Append(',').Append(F(r.FrameMsP95)).Append(',')
                   .Append(D(r.FrameMsMedian - Baseline)).Append(',')
                   .Append(F(r.GpuMsMedian)).Append(',').Append(F(r.CpuMsMedian)).Append(',')
                   .Append(F(r.CpuMsPerFrame)).Append(',')
                   .Append(F(r.ParseMsPerFrame)).Append(',').Append(F(r.LayoutMsPerFrame)).Append(',')
                   .Append(F(r.DrawMsPerFrame)).Append(',')
                   .Append(F(r.ReloadsPerFrame)).Append(',').Append(F(r.LayoutsPerFrame)).Append(',')
                   .Append(F(r.RendersPerFrame)).Append(',')
                   .Append(r.QuadsMedian).Append(',').Append(F(r.VertexKbPerUpload)).Append(',')
                   .Append(F(r.VertexKbPerFrame)).Append(',')
                   .Append(F(r.HeapKbPerFrame)).Append(',').Append(r.Gen0Collections).Append(',')
                   .Append(Absorbed(r) ? "yes" : "no").Append(',')
                   .Append(r.CacheFramesFast).Append(',').Append(r.CacheFramesPartial).Append(',')
                   .Append(r.CacheFramesRebuild).Append(',')
                   .Append(r.CacheQuadsReplayed).Append(',').Append(r.CacheQuadsEmitted)
                   .Append('\n');
            }

            try
            {
                File.WriteAllText(_reportPath, csv.ToString());
                Debug.Log($"[bench] report written to {_reportPath}");
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[bench] could not write {_reportPath}: {e.Message}");
            }

            // Also to the log, so a device with no reachable filesystem still
            // reports. One line per scenario, prefixed for grepping.
            if (RenderScaleRowIsInvalid())
            {
                Result full = Find("hud-settext"), half = Find("hud-settext-halfres");

                Debug.LogWarning($"[bench] hud-settext-halfres reflowed ({half.QuadsMedian} quads vs " +
                                 $"{full.QuadsMedian}) instead of only losing resolution — DeviceScale " +
                                 "clamped at 0.5. That row does not separate fill from geometry on this " +
                                 "display; ignore it.");
            }

            float drift = Drift();

            if (Mathf.Abs(drift) > 0.5f)
            {
                Debug.LogWarning($"[bench] the two baselines differ by {D(drift)} ms, so the device changed " +
                                 "speed during the run. Deltas measured in between carry that drift.");
            }

            int absorbed = AbsorbedCount();

            if (absorbed > 0)
            {
                Debug.LogWarning($"[bench] {absorbed} scenario(s) did measurable CPU work that the frame time " +
                                 "did not move for: the frame had budget to spare and absorbed it. Their " +
                                 "hud_cost_ms is a floor, not a cost. The cpu columns are unaffected.");
            }

            foreach (Result r in _results)
            {
                Debug.Log($"[bench-row] {r.Name,-16} frame {F(r.FrameMsMedian),7} ({D(r.FrameMsMedian - Baseline),6})  " +
                          $"gpu {F(r.GpuMsMedian),6}  cpu {F(r.CpuMsPerFrame),6} = " +
                          $"parse {F(r.ParseMsPerFrame),5} + layout {F(r.LayoutMsPerFrame),5} + " +
                          $"draw {F(r.DrawMsPerFrame),5}  runs/frame {F(r.ReloadsPerFrame)}/" +
                          $"{F(r.LayoutsPerFrame)}/{F(r.RendersPerFrame)}  quads {r.QuadsMedian,5}  " +
                          $"vtx {F(r.VertexKbPerFrame),6} KB/frame  " +
                          $"gc0 {r.Gen0Collections}");
            }
        }

        static string F(float v) =>
            v < 0f ? "n/a" : v.ToString("0.00", CultureInfo.InvariantCulture);

        /// <summary>
        /// A delta, which is allowed to be negative. F() reserves negatives for
        /// "the device would not say", so running a difference through it turns
        /// a hair of measurement noise into a missing measurement.
        /// </summary>
        static string D(float v) =>
            (v >= 0f ? "+" : "") + v.ToString("0.00", CultureInfo.InvariantCulture);

        /// <summary>
        /// Puts the table on screen, drawn by the thing it is a table about.
        /// </summary>
        void ShowReport()
        {
            var canvas = GetComponentInParent<Canvas>();
            if (canvas == null)
            {
                return;
            }

            for (int i = 0; i < _panels.Length; i++)
            {
                _panels[i].gameObject.SetActive(false);
            }

            var go = new GameObject("Report", typeof(RectTransform), typeof(RawImage),
                                    typeof(HtmlView), typeof(HtmlRawImage));
            go.transform.SetParent(canvas.transform, false);
            go.transform.SetAsLastSibling();

            var rect = (RectTransform)go.transform;
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = Vector2.zero;
            rect.offsetMax = Vector2.zero;

            go.GetComponent<HtmlView>().LoadHtml(ReportPage());
        }

        string ReportPage()
        {
            _sb.Clear();
            _sb.Append(@"<style>
  html,body { margin:0; background:#0b0f1a; color:#e8ecf6;
              font:13px -apple-system,'Helvetica Neue',Arial,sans-serif; }
  .wrap { padding:14px 12px; }
  h1 { font-size:19px; margin:0 0 2px 0; }
  .sub { color:#8e97b3; font-size:12px; margin:0 0 12px 0; }
  .row { border-bottom:1px solid #1b2440; padding:5px 0; }
  .name { color:#e8ecf6; }
  .cost { color:#9fb4e8; }
  .dim { color:#7f8aa8; font-size:11px; }
  .warn { color:#f0b429; font-size:12px; margin:10px 0 0 0; }
</style>");

            _sb.Append("<div class=\"wrap\"><h1>HUD maliyeti</h1>");
            _sb.Append("<p class=\"sub\">").Append(SystemInfo.deviceModel).Append(" &middot; ")
               .Append(Screen.width).Append('x').Append(Screen.height).Append(" &middot; ")
               .Append(_load != null ? _load.Cubes : 0).Append(" kup yuk &middot; ")
               .Append(_measureFrames).Append(" kare/senaryo</p>");

            foreach (Result r in _results)
            {
                _sb.Append("<div class=\"row\"><span class=\"name\">").Append(r.Name)
                   .Append(Absorbed(r) ? " &#9888;" : "").Append("</span> ")
                   .Append("<span class=\"cost\">").Append(F(r.FrameMsMedian)).Append(" ms")
                   .Append(r.Name == "game-only" ? "" : " (" + D(r.FrameMsMedian - Baseline) + ")")
                   .Append("</span><div class=\"dim\">cpu ").Append(F(r.CpuMsPerFrame))
                   .Append(" (p ").Append(F(r.ParseMsPerFrame))
                   .Append(" / l ").Append(F(r.LayoutMsPerFrame))
                   .Append(" / d ").Append(F(r.DrawMsPerFrame))
                   .Append(") &middot; gpu ").Append(F(r.GpuMsMedian))
                   .Append(" &middot; ").Append(r.QuadsMedian).Append(" quad / ")
                   .Append(F(r.VertexKbPerFrame)).Append(" KB/kare")
                   .Append("</div></div>");
            }

            if (RenderScaleRowIsInvalid())
            {
                _sb.Append("<p class=\"warn\">halfres satiri yeniden akmis (quad sayisi degisti), " +
                           "yani cozunurluk degil layout degismis — o satiri okuma.</p>");
            }

            if (AbsorbedCount() > 0)
            {
                _sb.Append("<p class=\"warn\">&#9888; isaretli satirlarda is olculdu ama kare suresi " +
                           "kimildamadi: kare butcesi yuttu. Onlarin artisi bir taban, maliyet degil.</p>");
            }

            _sb.Append("<p class=\"dim\">").Append(_reportPath).Append("</p>");
            _sb.Append("</div>");
            return _sb.ToString();
        }
    }
}
