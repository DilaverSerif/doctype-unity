using System.Collections;
using Doctype.Samples;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using UnityEngine.UI;

namespace Doctype.Tests
{
    /// <summary>
    /// Runtime tests: the component actually running inside Play mode.
    /// </summary>
    /// <remarks>
    /// EditMode tests drive <see cref="HtmlDocument"/> and
    /// <see cref="HtmlRenderer"/> directly. These instead go through
    /// <see cref="HtmlView"/>'s MonoBehaviour lifecycle — OnEnable, the
    /// LateUpdate render loop, target recreation, teardown on destroy — which is
    /// where lifetime and per-frame bugs live.
    /// </remarks>
    public class HtmlPlayModeTests
    {
        private GameObject _go;
        private HtmlView _view;
        private Texture2D _readback;

        [SetUp]
        public void SetUp()
        {
            _go = new GameObject("HtmlView");
            _view = _go.AddComponent<HtmlView>();
        }

        [TearDown]
        public void TearDown()
        {
            if (_go != null)
            {
                Object.DestroyImmediate(_go);
            }

            if (_readback != null)
            {
                Object.DestroyImmediate(_readback);
            }
        }

        /// <summary>
        /// Waits until the view has produced a surface. The view renders in
        /// LateUpdate, so a single yield is not always enough — a resize
        /// schedules another pass.
        /// </summary>
        private IEnumerator WaitForRender(int frames = 3)
        {
            for (int i = 0; i < frames; i++)
            {
                yield return null;
            }
        }

        /// <summary>Samples the view's surface in document coordinates (y down).</summary>
        private Color32 Sample(int documentX, int documentY)
        {
            RenderTexture target = _view.Texture;
            Assert.IsNotNull(target, "the view has not produced a surface");

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

            return _readback.GetPixel(documentX, target.height - 1 - documentY);
        }

        [UnityTest]
        public IEnumerator ViewProducesASurfaceAndPaintsIt()
        {
            _view.SetSize(320, 240);
            _view.LoadHtml("<body style='margin:0'><div style='width:200px;height:100px;background:#ff0000'></div></body>");

            yield return WaitForRender();

            Assert.IsNotNull(_view.Texture, "a RenderTexture should exist after the first frames");
            Assert.AreEqual(320, _view.Texture.width);
            Assert.AreEqual(240, _view.Texture.height);
            Assert.Greater(_view.QuadCount, 0, "something should have been drawn");

            Color32 inside = Sample(100, 50);
            Assert.AreEqual(255, inside.r, 3, "the red div should be painted");
            Assert.AreEqual(0, inside.g, 3);
        }

        [UnityTest]
        public IEnumerator ContentChangesRepaintOnTheNextFrame()
        {
            _view.SetSize(200, 200);
            _view.LoadHtml("<body style='margin:0'><div style='width:200px;height:200px;background:#ff0000'></div></body>");
            yield return WaitForRender();
            Assert.AreEqual(255, Sample(100, 100).r, 3);

            _view.LoadHtml("<body style='margin:0'><div style='width:200px;height:200px;background:#00ff00'></div></body>");
            yield return WaitForRender();

            Color32 after = Sample(100, 100);
            Assert.AreEqual(255, after.g, 3, "the new content should be on screen");
            Assert.AreEqual(0, after.r, 3, "and the old content gone");
        }

        [UnityTest]
        public IEnumerator HoverRepaintsWithoutReparsing()
        {
            _view.SetSize(200, 200);
            _view.LoadHtml(
                "<body style='margin:0'><style>" +
                "#b{width:200px;height:200px;background:#ff0000}" +
                "#b:hover{background:#0000ff}" +
                "</style><div id='b'></div></body>");

            yield return WaitForRender();
            Assert.AreEqual(255, Sample(100, 100).r, 3, "starts red");

            _view.PointerMove(new Vector2(100f, 100f));
            yield return WaitForRender();

            Assert.AreEqual(255, Sample(100, 100).b, 3, "turns blue under the pointer");

            _view.PointerExit();
            yield return WaitForRender();

            Assert.AreEqual(255, Sample(100, 100).r, 3, "returns to red when the pointer leaves");
        }

        [UnityTest]
        public IEnumerator ResizeKeepsHoverState()
        {
            // A resize used to re-parse the document, which silently dropped
            // :hover. This is the regression test for that.
            _view.SetSize(200, 200);
            _view.LoadHtml(
                "<body style='margin:0'><style>" +
                "#b{width:100%;height:100%;background:#ff0000}" +
                "#b:hover{background:#0000ff}" +
                "</style><div id='b'></div></body>");

            yield return WaitForRender();

            _view.PointerMove(new Vector2(100f, 100f));
            yield return WaitForRender();
            Assert.AreEqual(255, Sample(100, 100).b, 3, "hovered before the resize");

            _view.SetSize(300, 300);
            yield return WaitForRender();

            Assert.AreEqual(300, _view.Texture.width, "the surface followed the new size");
            Assert.AreEqual(255, Sample(150, 150).b, 3, "still hovered after the resize");
        }

        [UnityTest]
        public IEnumerator ClickingAnAnchorRaisesTheEvent()
        {
            string clicked = null;
            _view.AnchorClicked += url => clicked = url;

            _view.SetSize(300, 200);
            _view.LoadHtml(
                "<body style='margin:0;font-family:sans-serif'>" +
                "<a href='litehtml://ok' style='display:block;width:300px;height:200px'>tikla</a></body>");

            yield return WaitForRender();

            // litehtml only treats it as a click when press and release land on
            // the same element, so both have to be delivered.
            _view.PointerMove(new Vector2(150f, 100f));
            _view.PointerDown(new Vector2(150f, 100f));
            _view.PointerUp(new Vector2(150f, 100f));

            yield return WaitForRender();

            Assert.AreEqual("litehtml://ok", clicked, "the anchor URL should reach the C# event");
        }

        [UnityTest]
        public IEnumerator RebuildingEveryFrameStaysStable()
        {
            // The atlas grows and the mesh buffers are reused across frames;
            // churning content is what shakes out stale UVs and capacity bugs.
            _view.SetSize(400, 300);

            int lastQuads = 0;

            for (int frame = 0; frame < 30; frame++)
            {
                _view.LoadHtml(
                    "<body style='margin:0;font-family:sans-serif;font-size:14px'>" +
                    $"<div style='padding:8px;border:1px solid #888;border-radius:6px'>frame {frame} " +
                    $"<span style='color:#c00'>{frame * 7919}</span></div></body>");

                yield return null;
                yield return null;

                Assert.Greater(_view.QuadCount, 0, $"frame {frame} drew nothing");
                lastQuads = _view.QuadCount;
            }

            Assert.Greater(lastQuads, 0);
            Assert.IsNotNull(_view.Texture);
        }

        [UnityTest]
        public IEnumerator DestroyAndRecreateDoesNotThrow()
        {
            for (int i = 0; i < 3; i++)
            {
                _view.SetSize(160, 120);
                _view.LoadHtml($"<body style='margin:0'><div style='width:99px;height:99px;background:#0f0'>{i}</div></body>");
                yield return WaitForRender();

                Assert.IsNotNull(_view.Texture);

                Object.DestroyImmediate(_go);
                yield return null;

                _go = new GameObject("HtmlView");
                _view = _go.AddComponent<HtmlView>();
            }
        }

        [UnityTest]
        public IEnumerator DemoSceneRunsAndKeepsRebuilding()
        {
            // Exercises the shipped scene end to end: Canvas + RawImage +
            // HtmlRawImage + HtmlView + the demo controller.
            Object.DestroyImmediate(_go);
            _go = null;

            Scene scene = SceneManager.GetActiveScene();
            Assert.IsTrue(scene.IsValid());

            var canvasGo = new GameObject("Canvas", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            canvasGo.GetComponent<Canvas>().renderMode = RenderMode.ScreenSpaceOverlay;

            var panel = new GameObject("Panel", typeof(RectTransform), typeof(RawImage));
            panel.transform.SetParent(canvasGo.transform, false);
            ((RectTransform)panel.transform).sizeDelta = new Vector2(640f, 420f);

            var view = panel.AddComponent<HtmlView>();
            panel.AddComponent<HtmlRawImage>();
            var controller = panel.AddComponent<HtmlDemoController>();

            // The controller refreshes on a 0.25s timer; give it a few turns.
            float deadline = Time.unscaledTime + 3f;
            while (Time.unscaledTime < deadline && controller.Rebuilds < 3)
            {
                yield return null;
            }

            Assert.GreaterOrEqual(controller.Rebuilds, 3,
                                  "the demo controller should have rebuilt the page several times");
            Assert.IsNotNull(view.Texture, "the view produced a surface inside the scene");
            Assert.Greater(view.QuadCount, 20, "the live stats page is non-trivial");

            RawImage raw = panel.GetComponent<RawImage>();
            Assert.AreSame(view.Texture, raw.texture, "the RawImage is bound to the current surface");

            Object.DestroyImmediate(canvasGo);
        }

        /// <summary>
        /// A :hover rule that resizes must resize on screen, not just in the
        /// styles. This is the input/layout contract end to end: PointerMove
        /// reports Layout, the view re-runs layout before the next record, and
        /// the rect the game reads matches what the player sees.
        /// </summary>
        [UnityTest]
        public IEnumerator HoverThatResizesRelayouts()
        {
            _view.SetSize(400, 300);
            _view.LoadHtml("<body style='margin:0'>" +
                           "<style>#grow { width:100px; height:40px; background:#333; }" +
                           " #grow:hover { width:200px; }</style>" +
                           "<div id='grow'></div></body>");
            yield return WaitForRender();

            Assert.IsTrue(_view.TryGetElementRect("#grow", out Rect before));
            Assert.AreEqual(100f, before.width, 0.5f, "starts at its resting width");

            _view.PointerMove(new Vector2(50f, 20f));
            yield return WaitForRender();

            Assert.IsTrue(_view.TryGetElementRect("#grow", out Rect hovered));
            Assert.AreEqual(200f, hovered.width, 0.5f,
                            "the rect the game reads matches the hover style, not the stale layout");

            _view.PointerExit();
            yield return WaitForRender();

            Assert.IsTrue(_view.TryGetElementRect("#grow", out Rect after));
            Assert.AreEqual(100f, after.width, 0.5f, "and it snaps back on exit");
        }

    }
}
