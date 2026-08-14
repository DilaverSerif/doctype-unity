using System.IO;
using NUnit.Framework;
using Unity.Collections;
using UnityEngine;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// End-to-end GPU tests: HTML in, actual pixels out.
    /// </summary>
    /// <remarks>
    /// These are what prove the shader reconstructs the same shapes the native
    /// reference rasterizer produces. Everything is read back from a real
    /// RenderTexture, so a broken SDF, a flipped projection or a colour-space
    /// mistake all fail here.
    /// </remarks>
    public class LiteHtmlRenderTests
    {
        private const int Width = 300;
        private const int Height = 200;

        // Allow for sRGB round-tripping through a linear render target.
        private const int ColorTolerance = 3;

        private LiteHtmlDocument _doc;
        private LiteHtmlRenderer _renderer;
        private RenderTexture _target;
        private Texture2D _readback;

        [SetUp]
        public void SetUp()
        {
            _doc = LiteHtmlNativeTests.CreateDocument();
            _renderer = new LiteHtmlRenderer();

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

            if (_target != null)
            {
                _target.Release();
                Object.DestroyImmediate(_target);
            }

            if (_readback != null)
            {
                Object.DestroyImmediate(_readback);
            }
        }

        /// <summary>Lays out and draws the page, then reads the result back.</summary>
        private void Render(string html, Color? background = null)
        {
            Assert.IsTrue(_doc.LoadHtml(html), _doc.LastError);
            _doc.Layout(Width);

            NativeArray<LiteHtmlQuad> quads = _doc.Record(out LiteHtmlFrame frame);
            _renderer.Render(quads, frame, _target, background ?? Color.clear, new Vector2(Width, Height));

            Readback();
        }

        private void Readback()
        {
            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = _target;
            _readback.ReadPixels(new Rect(0, 0, Width, Height), 0, 0);
            _readback.Apply(false, false);
            RenderTexture.active = previous;
        }

        /// <summary>
        /// Samples using document coordinates (origin top-left, y down), which
        /// is the opposite of Unity's texture convention.
        /// </summary>
        private Color32 At(int documentX, int documentY)
        {
            return _readback.GetPixel(documentX, Height - 1 - documentY);
        }

        private void AssertColor(int x, int y, byte r, byte g, byte b, string because)
        {
            Color32 c = At(x, y);
            Assert.AreEqual(r, c.r, ColorTolerance, $"{because} (red) at ({x},{y}) -> {c}");
            Assert.AreEqual(g, c.g, ColorTolerance, $"{because} (green) at ({x},{y}) -> {c}");
            Assert.AreEqual(b, c.b, ColorTolerance, $"{because} (blue) at ({x},{y}) -> {c}");
        }

        [Test]
        public void ShaderIsPresent()
        {
            Assert.IsNotNull(Shader.Find("LiteHtmlUnity/Quad"), "the quad shader must be in the project");
        }

        [Test]
        public void SolidRectRendersAtTheRightPlace()
        {
            Render("<body style='margin:0'><div style='width:100px;height:50px;background:#ff0000'></div></body>");

            AssertColor(50, 25, 255, 0, 0, "inside the div is red");
            Assert.AreEqual(0, At(200, 100).a, "outside the div stays transparent");
        }

        [Test]
        public void DocumentOriginIsTopLeft()
        {
            // A y-flip is the single easiest mistake to make when driving a
            // RenderTexture by hand, so it gets its own test.
            Render("<body style='margin:0'>" +
                   "<div style='width:300px;height:50px;background:#ff0000'></div>" +
                   "<div style='width:300px;height:50px;background:#0000ff'></div>" +
                   "</body>");

            AssertColor(150, 25, 255, 0, 0, "the first block is at the top");
            AssertColor(150, 75, 0, 0, 255, "the second block is below it");
        }

        [Test]
        public void BorderEdgesGetTheirOwnColors()
        {
            // content-box: the border box ends up 124 x 76.
            Render("<body style='margin:0'><div style='width:100px;height:60px;" +
                   "border-top:6px solid #ff0000;border-right:10px solid #00ff00;" +
                   "border-bottom:14px solid #0000ff;border-left:18px solid #ffff00'></div></body>");

            AssertColor(60, 2, 255, 0, 0, "top edge");
            AssertColor(120, 40, 0, 255, 0, "right edge");
            AssertColor(60, 72, 0, 0, 255, "bottom edge");
            AssertColor(3, 40, 255, 255, 0, "left edge");

            Assert.AreEqual(0, At(60, 40).a, "the content area is not painted by the border");
        }

        [Test]
        public void CornersAreRounded()
        {
            Render("<body style='margin:0'><div style='width:100px;height:100px;" +
                   "border-radius:50px;background:#ff0000'></div></body>");

            AssertColor(50, 50, 255, 0, 0, "the middle of the circle is filled");
            Assert.AreEqual(0, At(2, 2).a, "the corner is cut away by the radius");
            Assert.AreEqual(0, At(97, 2).a, "the opposite corner too");
            AssertColor(50, 2, 255, 0, 0, "the top edge midpoint is still inside");
        }

        [Test]
        public void LinearGradientInterpolatesAcross()
        {
            Render("<body style='margin:0'><div style='width:200px;height:100px;" +
                   "background:linear-gradient(to right,#ff0000,#0000ff)'></div></body>");

            AssertColor(3, 50, 252, 0, 3, "left end is red");
            AssertColor(196, 50, 3, 0, 252, "right end is blue");

            Color32 middle = At(100, 50);
            Assert.Greater(middle.r, 100, "the middle blends both stops");
            Assert.Greater(middle.b, 100, "the middle blends both stops");
        }

        [Test]
        public void TextRendersFromTheGlyphAtlas()
        {
            if (!File.Exists(LiteHtmlNativeTests.AhemPath))
            {
                Assert.Ignore("ahem.ttf not available");
            }

            // Ahem glyphs are solid boxes, so their pixels are exactly the text
            // colour and their extent is exactly predictable.
            Render("<body style='margin:0'><div style=\"font:100px ahem;line-height:1;color:#00ff00\">A</div></body>");

            AssertColor(50, 40, 0, 255, 0, "the glyph body is opaque green");
            Assert.AreEqual(0, At(150, 40).a, "past the single glyph there is nothing");
        }

        [Test]
        public void OverflowHiddenClipsOnTheGpu()
        {
            Render("<body style='margin:0'>" +
                   "<div style='width:60px;height:60px;overflow:hidden'>" +
                   "<div style='width:200px;height:200px;background:#00ff00'></div>" +
                   "</div></body>");

            AssertColor(30, 30, 0, 255, 0, "inside the clip is painted");
            Assert.AreEqual(0, At(100, 100).a, "outside the clip is not");
            Assert.AreEqual(0, At(100, 30).a, "clipped horizontally too");
        }

        [Test]
        public void HoverRepaintsWithTheNewColor()
        {
            const string html =
                "<body style='margin:0'><style>" +
                "#b{width:100px;height:100px;background:#ff0000}" +
                "#b:hover{background:#0000ff}" +
                "</style><div id='b'></div></body>";

            Render(html);
            AssertColor(50, 50, 255, 0, 0, "unhovered");

            Assert.IsTrue(_doc.MouseMove(new Vector2(50f, 50f)));

            NativeArray<LiteHtmlQuad> quads = _doc.Record(out LiteHtmlFrame frame);
            _renderer.Render(quads, frame, _target, Color.clear, new Vector2(Width, Height));
            Readback();

            AssertColor(50, 50, 0, 0, 255, "hovered");
        }

        [Test]
        public void RootBackgroundFillsTheWholeSurface()
        {
            // Browsers propagate the root element's background across the whole
            // canvas, not just its own box. litehtml flags that layer as
            // is_root; ignoring it left the area below a short document unpainted.
            Render("<body style='margin:0;background:#123456'>" +
                   "<div style='height:10px'></div></body>");

            AssertColor(150, 5, 0x12, 0x34, 0x56, "painted at the top");
            AssertColor(150, Height - 5, 0x12, 0x34, 0x56, "painted well below the document box");
            AssertColor(Width - 5, Height - 5, 0x12, 0x34, 0x56, "painted into the far corner");
        }

        [Test]
        public void BackgroundClearColorIsHonored()
        {
            Render("<body style='margin:0'></body>", new Color32(20, 30, 40, 255));
            AssertColor(150, 100, 20, 30, 40, "the clear colour shows through an empty document");
        }

        [Test]
        public void WholePageDrawsInASingleBatch()
        {
            Render("<body style='margin:0;font-family:sans-serif'>" +
                   "<div style='padding:12px;border:2px solid #333;border-radius:8px;" +
                   "background:linear-gradient(#fff,#ddd)'>" +
                   "<h1>Title</h1><p>Some paragraph text that wraps onto a second line.</p>" +
                   "</div></body>");

            // Fills, borders, glyphs and gradients all end up in one mesh, so
            // quad count is the only thing that grows -- not draw calls.
            Assert.Greater(_renderer.LastQuadCount, 20, "the page produced a real quad stream");
        }

        // --- transparency ------------------------------------------------------
        //
        // What a see-through menu depends on. The quad shader blends RGB with
        // SrcAlpha OneMinusSrcAlpha, so colour reaches the surface already scaled
        // by its own alpha. Whatever composites that surface has to know: a uGUI
        // RawImage on the default material scales by alpha a second time, which
        // is the difference between a menu at the opacity it was authored with
        // and one at the square of it. These record the actual behaviour so a
        // change in blend state is noticed rather than discovered on a device.

        [Test]
        public void HalfTransparentFillReachesTheSurfacePremultiplied()
        {
            Render("<body style='margin:0'><div style='width:100px;height:100px;" +
                   "background:rgba(255,0,0,0.5)'></div></body>");

            Color32 c = At(50, 50);
            Assert.AreEqual(128, c.a, 6, $"alpha should survive at about half -> {c}");

            // 188, not 128 and not 255. Blending happens in linear space, so the
            // stored value is half of full red in linear, which sRGB-encodes to
            // about 0.735. Reading 255 here would mean straight alpha; reading
            // 128 would mean it had been premultiplied in sRGB space. Both would
            // be wrong, and both would composite differently.
            Assert.AreEqual(188, c.r, 6, $"red arrives premultiplied in linear, sRGB-encoded -> {c}");
        }

        [Test]
        public void OpaqueFillHidesThePremultiplication()
        {
            // Why this only shows up once something is made see-through.
            Render("<body style='margin:0'><div style='width:100px;height:100px;" +
                   "background:rgb(255,0,0)'></div></body>");

            Color32 c = At(50, 50);
            Assert.AreEqual(255, c.a, 2, $"{c}");
            Assert.AreEqual(255, c.r, ColorTolerance, $"{c}");
        }

        [Test]
        public void StackedTransparentLayersAccumulateAlphaCorrectly()
        {
            // 0.5 over 0.5 is 0.75, not 1.0 and not 0.5 — the alpha channel is
            // blended One OneMinusSrcAlpha precisely so this holds.
            Render("<body style='margin:0'><div style='width:100px;height:100px;" +
                   "background:rgba(0,0,255,0.5)'>" +
                   "<div style='width:100px;height:100px;background:rgba(0,0,255,0.5)'></div>" +
                   "</div></body>");

            Color32 c = At(50, 50);
            Assert.AreEqual(191, c.a, 8, $"two half-opaque layers should read three-quarters opaque -> {c}");
        }
    }
}
