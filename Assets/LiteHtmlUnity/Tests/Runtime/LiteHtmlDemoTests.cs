using System.Collections;
using LiteHtmlUnity.Samples;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using UnityEngine.UI;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// Tests for the demo's navigation, animation and frame-rate cap.
    /// </summary>
    /// <remarks>
    /// These matter beyond the demo: navigation is the only way this system can
    /// have multiple screens (there is no JavaScript), and animation means
    /// re-parsing and re-laying-out every frame — so if either is broken or
    /// slow, the architecture itself is in question.
    /// </remarks>
    public class LiteHtmlDemoTests
    {
        private GameObject _root;
        private LiteHtmlView _view;
        private LiteHtmlDemoController _demo;
        private FrameRateLimiter _limiter;
        private Texture2D _readback;

        [SetUp]
        public void SetUp()
        {
            var canvasGo = new GameObject("Canvas", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            canvasGo.GetComponent<Canvas>().renderMode = RenderMode.ScreenSpaceOverlay;
            _root = canvasGo;

            var panel = new GameObject("Panel", typeof(RectTransform), typeof(RawImage));
            panel.transform.SetParent(canvasGo.transform, false);
            ((RectTransform)panel.transform).sizeDelta = new Vector2(680f, 460f);

            _view = panel.AddComponent<LiteHtmlView>();
            panel.AddComponent<LiteHtmlRawImage>();
            _limiter = panel.AddComponent<FrameRateLimiter>();
            _demo = panel.AddComponent<LiteHtmlDemoController>();
        }

        [TearDown]
        public void TearDown()
        {
            if (_root != null)
            {
                Object.DestroyImmediate(_root);
            }

            if (_readback != null)
            {
                Object.DestroyImmediate(_readback);
            }

            Application.targetFrameRate = -1;
        }

        private IEnumerator Settle(int frames = 4)
        {
            for (int i = 0; i < frames; i++)
            {
                yield return null;
            }
        }

        /// <summary>Reads the whole surface back so frames can be compared.</summary>
        private Color32[] Grab()
        {
            RenderTexture target = _view.Texture;
            Assert.IsNotNull(target, "no surface yet");

            if (_readback == null || _readback.width != target.width || _readback.height != target.height)
            {
                if (_readback != null)
                {
                    Object.DestroyImmediate(_readback);
                }

                _readback = new Texture2D(target.width, target.height, TextureFormat.RGBA32, false, false);
            }

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = target;
            _readback.ReadPixels(new Rect(0, 0, target.width, target.height), 0, 0);
            _readback.Apply(false, false);
            RenderTexture.active = previous;

            return _readback.GetPixels32();
        }

        private static int DifferingPixels(Color32[] a, Color32[] b)
        {
            Assert.AreEqual(a.Length, b.Length);

            int n = 0;
            for (int i = 0; i < a.Length; i++)
            {
                if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b)
                {
                    n++;
                }
            }

            return n;
        }

        [UnityTest]
        public IEnumerator NavigatingByLinkChangesPage()
        {
            yield return Settle();
            Assert.AreEqual(LiteHtmlDemoController.Page.Overview, _demo.CurrentPage, "starts on Overview");

            Color32[] overview = Grab();

            // Go through the public API rather than hunting for the link's
            // pixel position; the click path itself is covered separately.
            _demo.GoTo(LiteHtmlDemoController.Page.Typography);
            yield return Settle(8);

            Assert.AreEqual(LiteHtmlDemoController.Page.Typography, _demo.CurrentPage);
            Assert.Greater(DifferingPixels(overview, Grab()), 5000, "the page should look different");
        }

        [UnityTest]
        public IEnumerator ClickingANavLinkNavigates()
        {
            yield return Settle();

            // The nav bar sits at the top-left; the second entry is "Animasyon".
            // Coordinates come from the stylesheet: 14px shell margin, pills
            // roughly 60px wide with 5px gaps.
            var point = new Vector2(120f, 26f);

            _view.PointerMove(point);
            _view.PointerDown(point);
            _view.PointerUp(point);

            yield return Settle(6);

            Assert.AreNotEqual(LiteHtmlDemoController.Page.Overview, _demo.CurrentPage,
                               $"clicking the nav bar should navigate (last link: {_demo.LastClickedLink})");
            Assert.IsTrue(_demo.LastClickedLink.StartsWith("page://"),
                          $"expected a page link, got '{_demo.LastClickedLink}'");
        }

        [UnityTest]
        public IEnumerator AnimationChangesPixelsEveryFrame()
        {
            _demo.GoTo(LiteHtmlDemoController.Page.Animation);
            _demo.Animate = true;

            // Let the enter transition finish, so movement afterwards is the
            // animation itself and not the page sliding in.
            yield return Settle(20);

            Color32[] first = Grab();
            yield return null;
            yield return null;
            Color32[] second = Grab();

            Assert.Greater(DifferingPixels(first, second), 200,
                           "the animated page should differ between frames");
            Assert.Greater(_demo.Rebuilds, 15, "animation rebuilds the page every frame");
        }

        [UnityTest]
        public IEnumerator AnimationCanBeTurnedOff()
        {
            _demo.GoTo(LiteHtmlDemoController.Page.Animation);
            _demo.Animate = false;
            yield return Settle(10);

            int before = _demo.Rebuilds;
            Color32[] first = Grab();

            yield return null;
            yield return null;

            Assert.AreEqual(before, _demo.Rebuilds, "no rebuild should happen inside the idle interval");
            Assert.AreEqual(0, DifferingPixels(first, Grab()), "a still page should be pixel-identical");
        }

        [UnityTest]
        public IEnumerator FrameRateCapIsApplied()
        {
            yield return Settle();

            _limiter.SetTargetFrameRate(61);
            Assert.AreEqual(61, Application.targetFrameRate, "the cap reaches Unity");
            Assert.AreEqual(0, QualitySettings.vSyncCount,
                            "VSync must be off or targetFrameRate is ignored");
            Assert.IsTrue(_limiter.IsLimited);

            _limiter.SetLimited(false);
            Assert.AreEqual(-1, Application.targetFrameRate, "uncapped");
            Assert.IsFalse(_limiter.IsLimited);
        }

        [UnityTest]
        public IEnumerator FrameRateCanBeChangedFromInsideTheDocument()
        {
            _demo.GoTo(LiteHtmlDemoController.Page.Performance);
            yield return Settle(10);

            _limiter.SetTargetFrameRate(120);
            Assert.AreEqual(120, Application.targetFrameRate);

            // Stop the controller rebuilding the page every frame, or it
            // overwrites the markup below before the click can land. The
            // component stays enabled so its AnchorClicked handler — the thing
            // under test — is still subscribed.
            _demo.Animate = false;
            yield return Settle(2);

            // The real "30 fps" pill's pixel position depends on the whole
            // stylesheet; a full-surface link exercises the same chain
            // (litehtml hit test -> C ABI callback -> C# event -> handler ->
            // limiter) without encoding a brittle coordinate. Clicking the real
            // markup is covered by ClickingANavLinkNavigates.
            _view.LoadHtml("<body style='margin:0'><a href='fps://30' " +
                           "style='display:block;width:680px;height:460px'>x</a></body>");
            yield return Settle(3);

            _view.PointerMove(new Vector2(300f, 200f));
            _view.PointerDown(new Vector2(300f, 200f));
            _view.PointerUp(new Vector2(300f, 200f));
            yield return null;

            Assert.AreEqual(30, Application.targetFrameRate,
                            "an fps:// link inside the page should retarget the limiter");
        }

        [UnityTest]
        public IEnumerator ClickingADemoButtonReachesCSharp()
        {
            // The Overview page's button card sits below the stats card. Rather
            // than hard-code a pixel, walk down the left edge until a press
            // lands on one of the bound buttons -- that also proves the buttons
            // are actually reachable at a plausible position.
            _demo.GoTo(LiteHtmlDemoController.Page.Overview);
            yield return Settle(10);

            int before = _demo.ButtonClicks;

            for (int y = 240; y < 440 && _demo.ButtonClicks == before; y += 6)
            {
                var p = new Vector2(60f, y);
                _view.PointerMove(p);
                _view.PointerDown(p);
                _view.PointerUp(p);
                yield return null;
            }

            Assert.Greater(_demo.ButtonClicks, before,
                           "a click somewhere down the button card should have hit a bound button");
            Assert.AreNotEqual("-", _demo.LastButtonAction, "the handler should have recorded a data-action");
        }

        [UnityTest]
        public IEnumerator DemoButtonSurvivesTheAnimatedRebuildLoop()
        {
            // The reported bug in its original setting: the demo rebuilds the
            // page every frame, and a click takes several frames.
            _demo.GoTo(LiteHtmlDemoController.Page.Overview);
            _demo.Animate = true;
            yield return Settle(10);

            int before = _demo.ButtonClicks;
            bool clicked = false;

            for (int y = 240; y < 440 && !clicked; y += 6)
            {
                var p = new Vector2(60f, y);

                _view.PointerMove(p);
                yield return null;

                _view.PointerDown(p);
                // Several rebuilds happen between press and release.
                yield return null;
                yield return null;
                yield return null;

                _view.PointerUp(p);
                yield return null;

                clicked = _demo.ButtonClicks > before;
            }

            Assert.IsTrue(clicked, "a multi-frame click must survive the rebuild loop");
        }

        [UnityTest]
        public IEnumerator TextSurvivesALongRebuildLoop()
        {
            // Reported failure: after running for a while the page kept its
            // boxes but lost every glyph. The cause was the glyph cache living
            // on the Font object -- litehtml recreates fonts on every re-parse,
            // so the same characters were re-packed into fresh atlas space each
            // frame until the atlas hit its ceiling and stopped accepting any.
            _demo.GoTo(LiteHtmlDemoController.Page.Overview);
            _demo.Animate = true;
            yield return Settle(20);

            Vector2Int atlasAtStart = _view.FontAtlasSize;
            Color32[] early = Grab();

            for (int i = 0; i < 240; i++)
            {
                yield return null;
            }

            Color32[] late = Grab();

            Assert.AreEqual(atlasAtStart, _view.FontAtlasSize,
                            $"the atlas grew from {atlasAtStart} to {_view.FontAtlasSize} without new characters");

            // Losing the text would leave the boxes behind, so a huge share of
            // pixels turning uniform is the signature to catch.
            int differing = DifferingPixels(early, late);
            Assert.Less(differing, early.Length / 2,
                        "most of the surface changed, which is what losing all glyphs looks like");

            Assert.Greater(_view.QuadCount, 100, "glyph quads should still be produced");
        }

        [UnityTest]
        public IEnumerator UnchangedMarkupIsNotReparsed()
        {
            // Re-parsing is ~95% of a rebuild's CPU cost, so a UI that only
            // changes when something happens must not pay it every frame.
            _demo.enabled = false;
            yield return Settle(4);

            _view.LoadHtml("<body style='margin:0'><div style='width:100px;height:50px;background:#0a0'>x</div></body>");
            yield return Settle(4);

            int skippedBefore = _view.SkippedReloads;

            // Same markup, asked for repeatedly.
            for (int i = 0; i < 10; i++)
            {
                _view.LoadHtml("<body style='margin:0'><div style='width:100px;height:50px;background:#0a0'>x</div></body>");
                yield return null;
            }

            Assert.Greater(_view.SkippedReloads, skippedBefore, "identical markup should skip the parse");
            Assert.AreEqual(0f, _view.ParseMs, "a skipped reload costs no parse time");

            // Different markup must still go through.
            int skippedAfter = _view.SkippedReloads;
            _view.LoadHtml("<body style='margin:0'><div style='width:100px;height:50px;background:#00a'>y</div></body>");
            yield return Settle(4);

            Assert.AreEqual(skippedAfter, _view.SkippedReloads, "changed markup must be parsed");
            Assert.Greater(_view.ParseMs, 0f, "a real reload records parse time");
        }

        [UnityTest]
        public IEnumerator RebuildCostStaysWithinAFrameBudget()
        {
            _demo.GoTo(LiteHtmlDemoController.Page.Animation);
            _demo.Animate = true;
            yield return Settle(30);

            // Sample rather than trust a single frame: the first rebuilds pay
            // for glyph rasterization that later ones reuse.
            float worst = 0f;
            float total = 0f;
            const int samples = 30;

            for (int i = 0; i < samples; i++)
            {
                yield return null;
                worst = Mathf.Max(worst, _view.TotalMs);
                total += _view.TotalMs;
            }

            float mean = total / samples;
            Debug.Log($"[LiteHtml] animated rebuild: mean {mean:0.00} ms, worst {worst:0.00} ms, " +
                      $"parse {_view.ParseMs:0.00} / layout {_view.LayoutMs:0.00} / draw {_view.DrawMs:0.00}, " +
                      $"{_view.QuadCount} quads");

            // 16.6 ms is one frame at 60 fps. A full re-parse plus re-layout has
            // to leave room for the rest of the game, so hold it to a third.
            Assert.Less(mean, 5.5f, $"mean rebuild {mean:0.00} ms is too expensive to run every frame");
        }
    }
}
