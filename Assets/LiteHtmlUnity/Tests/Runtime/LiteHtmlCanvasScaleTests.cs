using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using UnityEngine.UI;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// Covers the contract that lets one layout serve every screen: the surface
    /// follows the display's real pixel count, while the CSS viewport the page is
    /// laid out against stays fixed in canvas units.
    /// </summary>
    /// <remarks>
    /// Sizing the surface from <c>RectTransform.rect</c> alone renders below the
    /// display's resolution and stretches the result, which reads as soft text on
    /// a phone. These tests exist so that regression is caught in CI rather than
    /// on a device.
    /// </remarks>
    public class LiteHtmlCanvasScaleTests
    {
        const float RectWidth = 320f;
        const float RectHeight = 200f;

        GameObject _canvasGo;
        Canvas _canvas;
        LiteHtmlView _view;
        RectTransform _rect;

        [SetUp]
        public void SetUp()
        {
            _canvasGo = new GameObject("Canvas", typeof(Canvas));
            _canvas = _canvasGo.GetComponent<Canvas>();
            _canvas.renderMode = RenderMode.ScreenSpaceOverlay;

            // Deliberately no CanvasScaler: it drives scaleFactor from the screen
            // and would overwrite the value each test is trying to pin.
            _canvas.scaleFactor = 1f;

            var go = new GameObject("View", typeof(RectTransform), typeof(RawImage),
                                    typeof(LiteHtmlView), typeof(LiteHtmlRawImage));
            _rect = (RectTransform)go.transform;
            _rect.SetParent(_canvasGo.transform, false);
            _rect.sizeDelta = new Vector2(RectWidth, RectHeight);

            _view = go.GetComponent<LiteHtmlView>();
            _view.LoadHtml("<body style='margin:0'><div style='width:100px;height:20px'>x</div></body>");
        }

        [TearDown]
        public void TearDown()
        {
            if (_canvasGo != null)
            {
                Object.DestroyImmediate(_canvasGo);
            }
        }

        /// <summary>
        /// The view resizes in LateUpdate and a resize schedules another pass, so
        /// a single frame is not enough to observe the settled state.
        /// </summary>
        static IEnumerator Settle()
        {
            for (int i = 0; i < 4; i++)
            {
                yield return null;
            }
        }

        /// <summary>CSS viewport, read back through the input mapping.</summary>
        Vector2 CssViewport() => _view.NormalizedToDocument(new Vector2(1f, 0f));

        [UnityTest]
        public IEnumerator SurfaceFollowsCanvasScale()
        {
            _canvas.scaleFactor = 2f;
            yield return Settle();

            Assert.IsNotNull(_view.Texture, "the view should have produced a surface");
            Assert.AreEqual(Mathf.RoundToInt(RectWidth * 2f), _view.Texture.width,
                            "surface should be sized in real pixels, not canvas units");
            Assert.AreEqual(Mathf.RoundToInt(RectHeight * 2f), _view.Texture.height);
            Assert.AreEqual(2f, _view.DeviceScale, 0.001f, "device scale should track the canvas");
        }

        [UnityTest]
        public IEnumerator CssViewportIsUnchangedByResolution()
        {
            // This is the property that makes a HUD portable: an element pinned
            // 16 CSS px from the left edge is 16 canvas units from it on every
            // screen, however many physical pixels that turns out to be.
            _canvas.scaleFactor = 1f;
            yield return Settle();
            Vector2 atOne = CssViewport();

            _canvas.scaleFactor = 3f;
            yield return Settle();
            Vector2 atThree = CssViewport();

            Assert.AreEqual(RectWidth, atOne.x, 0.5f, "CSS width should equal the rect in canvas units");
            Assert.AreEqual(atOne.x, atThree.x, 0.5f, "tripling the resolution must not move the layout");
            Assert.AreEqual(atOne.y, atThree.y, 0.5f);

            // ...while the surface really did get denser.
            Assert.AreEqual(Mathf.RoundToInt(RectWidth * 3f), _view.Texture.width);
        }

        [UnityTest]
        public IEnumerator RectResizeStillDrivesTheDocument()
        {
            _canvas.scaleFactor = 2f;
            yield return Settle();

            _rect.sizeDelta = new Vector2(RectWidth * 0.5f, RectHeight);
            yield return Settle();

            Assert.AreEqual(Mathf.RoundToInt(RectWidth * 0.5f * 2f), _view.Texture.width,
                            "a narrower rect should still resize the surface");
            Assert.AreEqual(RectWidth * 0.5f, CssViewport().x, 0.5f,
                            "and the CSS viewport should follow it in canvas units");
        }
    }
}
