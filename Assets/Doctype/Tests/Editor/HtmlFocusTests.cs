using NUnit.Framework;
using Unity.Collections;
using UnityEngine;

namespace Doctype.Tests
{
    /// <summary>
    /// The managed half of gamepad focus: the native metric and lifecycle are
    /// proven in the harness; these prove the plumbing from C# down and the
    /// pixels coming back up.
    /// </summary>
    public class HtmlFocusTests
    {
        private const int Width = 300;
        private const int Height = 200;

        private HtmlDocument _doc;
        private HtmlRenderer _renderer;
        private RenderTexture _target;
        private Texture2D _readback;

        [SetUp]
        public void SetUp()
        {
            _doc = HtmlNativeTests.CreateDocument();
            _renderer = new HtmlRenderer();
            _target = new RenderTexture(Width, Height, 0, RenderTextureFormat.ARGB32,
                                        RenderTextureReadWrite.sRGB);
            _target.Create();
            _readback = new Texture2D(Width, Height, TextureFormat.RGBA32, false, false);
        }

        [TearDown]
        public void TearDown()
        {
            _renderer?.Dispose();
            _doc?.Dispose();
            if (_target != null) { _target.Release(); Object.DestroyImmediate(_target); }
            if (_readback != null) Object.DestroyImmediate(_readback);
        }

        private void RenderOnce()
        {
            _doc.Layout(Width);
            NativeArray<HtmlQuad> quads = _doc.Record(out HtmlFrame frame);
            _renderer.Render(quads, frame, _target, Color.clear, new Vector2(Width, Height));

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = _target;
            _readback.ReadPixels(new Rect(0, 0, Width, Height), 0, 0);
            _readback.Apply(false, false);
            RenderTexture.active = previous;
        }

        private Color32 At(int x, int y) => _readback.GetPixel(x, Height - 1 - y);

        private const string Page =
            "<body style='margin:0'><style>" +
            ".b{position:absolute;width:60px;height:40px;background:#ff0000}" +
            ".b:focus{background:#0000ff}" +
            "</style>" +
            "<div id='left' class='b' tabindex='0' style='left:0;top:0'></div>" +
            "<div id='right' class='b' tabindex='0' style='left:100px;top:0'></div>" +
            "</body>";

        [Test]
        public void FocusRestylesThePixels()
        {
            Assert.IsTrue(_doc.LoadHtml(Page), _doc.LastError);
            RenderOnce();
            Assert.AreEqual(255, At(30, 20).r, 3, "unfocused is red");

            Assert.AreEqual(HtmlDirty.Paint, _doc.SetFocus("#left"));
            RenderOnce();
            Assert.AreEqual(255, At(30, 20).b, 3, "focused is blue");
            Assert.AreEqual("left", _doc.FocusedId);
        }

        [Test]
        public void MoveWalksAndTheOldElementLetsGo()
        {
            Assert.IsTrue(_doc.LoadHtml(Page), _doc.LastError);
            _doc.Layout(Width);

            _doc.SetFocus("#left");
            Assert.IsTrue(_doc.MoveFocus(HtmlNavDirection.Right, out HtmlDirty dirty));
            Assert.AreEqual(HtmlDirty.Paint, dirty, "two recolours are still paint only");
            Assert.AreEqual("right", _doc.FocusedId);

            RenderOnce();
            Assert.AreEqual(255, At(30, 20).r, 3, "the old element is red again");
            Assert.AreEqual(255, At(130, 20).b, 3, "the new one is blue");

            Assert.IsFalse(_doc.MoveFocus(HtmlNavDirection.Right, out _), "the page ends here");
            Assert.AreEqual("right", _doc.FocusedId, "and focus stays");
        }

        [Test]
        public void ActivateRaisesTheAnchorEventWithoutAPointer()
        {
            Assert.IsTrue(_doc.LoadHtml(
                "<body style='margin:0'><a id='go' href='page://next' tabindex='0'>ileri</a></body>"),
                _doc.LastError);
            _doc.Layout(Width);

            string clicked = null;
            _doc.AnchorClicked += url => clicked = url;

            _doc.SetFocus("#go");
            Assert.IsTrue(_doc.Activate());
            Assert.AreEqual("page://next", clicked);
        }
    }
}
