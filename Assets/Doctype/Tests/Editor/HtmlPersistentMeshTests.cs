using NUnit.Framework;
using Unity.Collections;
using UnityEngine;

namespace Doctype.Tests
{
    /// <summary>
    /// The persistent mesh must be invisible: a renderer that keeps its vertex
    /// buffer alive and rewrites only the span the frame's stable range leaves
    /// open has to produce, pixel for pixel, what a renderer that rebuilds the
    /// whole mesh from scratch produces.
    /// </summary>
    /// <remarks>
    /// One document is driven through a mutation script. Each step records
    /// once and hands the same quads and frame to two renderers: a long-lived
    /// one, whose builder takes the ranged path, and a freshly created one,
    /// whose builder has no previous frame and must rebuild fully. Their
    /// targets are read back and compared byte for byte. The script includes
    /// the shapes that stress the range bookkeeping: a same-count change (pure
    /// middle rewrite), a count change (the tail slides), a change at the very
    /// start and very end of the page, and a frame with no change at all.
    /// </remarks>
    public class HtmlPersistentMeshTests
    {
        private const int Width = 300;
        private const int Height = 240;

        private HtmlDocument _doc;
        private HtmlRenderer _persistent;
        private RenderTexture _persistentTarget;
        private RenderTexture _freshTarget;
        private Texture2D _readbackA;
        private Texture2D _readbackB;

        [SetUp]
        public void SetUp()
        {
            _doc = HtmlNativeTests.CreateDocument();
            _persistent = new HtmlRenderer();

            _persistentTarget = NewTarget();
            _freshTarget = NewTarget();

            _readbackA = new Texture2D(Width, Height, TextureFormat.RGBA32, false, false);
            _readbackB = new Texture2D(Width, Height, TextureFormat.RGBA32, false, false);
        }

        private static RenderTexture NewTarget()
        {
            var target = new RenderTexture(Width, Height, 0, RenderTextureFormat.ARGB32,
                                           RenderTextureReadWrite.sRGB);
            target.Create();
            return target;
        }

        [TearDown]
        public void TearDown()
        {
            _persistent?.Dispose();
            _doc?.Dispose();

            ReleaseTarget(_persistentTarget);
            ReleaseTarget(_freshTarget);

            Object.DestroyImmediate(_readbackA);
            Object.DestroyImmediate(_readbackB);
        }

        private static void ReleaseTarget(RenderTexture target)
        {
            if (target != null)
            {
                target.Release();
                Object.DestroyImmediate(target);
            }
        }

        private static void Readback(RenderTexture target, Texture2D into)
        {
            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = target;
            into.ReadPixels(new Rect(0, 0, Width, Height), 0, 0);
            into.Apply(false, false);
            RenderTexture.active = previous;
        }

        /// <summary>
        /// Records once, renders through both paths, and demands identical
        /// pixels. Returns the frame so callers can assert on its claims.
        /// </summary>
        private HtmlFrame Step(string what)
        {
            _doc.Layout(Width);
            NativeArray<HtmlQuad> quads = _doc.Record(out HtmlFrame frame);

            var size = new Vector2(Width, Height);

            _persistent.Render(quads, frame, _persistentTarget, Color.clear, size);

            // A renderer born this instant has no mesh to keep; its builder
            // ignores the frame's stable range and rebuilds everything.
            var fresh = new HtmlRenderer();
            try
            {
                fresh.Render(quads, frame, _freshTarget, Color.clear, size);
            }
            finally
            {
                fresh.Dispose();
            }

            Readback(_persistentTarget, _readbackA);
            Readback(_freshTarget, _readbackB);

            var a = _readbackA.GetRawTextureData<byte>();
            var b = _readbackB.GetRawTextureData<byte>();

            Assert.AreEqual(b.Length, a.Length, what);

            for (int i = 0; i < a.Length; i++)
            {
                if (a[i] != b[i])
                {
                    int pixel = i / 4;
                    Assert.Fail($"{what}: pixel ({pixel % Width},{pixel / Width}) byte {i % 4} " +
                                $"differs, persistent={a[i]} fresh={b[i]}");
                }
            }

            return frame;
        }

        private const string Page =
            "<body style='margin:0;font-family:sans-serif;background:#101418;color:#e6e9f0'>" +
            "<div id='head' style='padding:6px;font-size:18px'>Baslik</div>" +
            "<div id='num' style='padding:4px;font-size:15px'>Skor 100</div>" +
            "<div id='box' style='margin:6px;padding:6px;border-radius:8px;background:#233056'>Kutu</div>" +
            "<div id='mark' style='padding:4px;font-size:14px'>Esya kisa</div>" +
            "<div id='tail' style='padding:6px;color:#8e97b3'>alt satir</div>" +
            "</body>";

        [Test]
        public void RangedRebuildsMatchFullRebuildsPixelForPixel()
        {
            Assert.IsTrue(_doc.LoadHtml(Page), _doc.LastError);
            Step("first frame");

            // Same glyph count in the middle: the purest ranged rewrite.
            _doc.SetText("#num", "Skor 101");
            HtmlFrame frame = Step("same-shape text");

            Assert.Greater(frame.StablePrefix, 0, "a middle change should keep a stable prefix");
            Assert.Greater(frame.StableSuffix, 0, "and a stable suffix");

            // Glyph count changes: the tail slides through the buffer.
            _doc.SetText("#mark", "Esya cok daha uzun bir etiket oldu");
            Step("reshaping text");

            // The very first element: prefix is empty, everything after rides.
            _doc.SetText("#head", "Yeni Baslik");
            Step("change at the top");

            // The very last element: the suffix is empty this time.
            _doc.SetText("#tail", "yepyeni alt satir");
            Step("change at the bottom");

            // A style change through the other mutation entry point.
            _doc.SetStyle("#box", "margin:6px;padding:6px;border-radius:8px;background:#6a2030");
            Step("style change");

            // No mutation at all: the frame reports None and neither renderer
            // may touch its target.
            Step("idle frame");

            // And back again, which must not smear anything left over.
            _doc.SetText("#num", "Skor 100");
            Step("return to an earlier value");
        }

        [Test]
        public void RangedPathUploadsLessThanThePage()
        {
            Assert.IsTrue(_doc.LoadHtml(Page), _doc.LastError);
            Step("first frame");

            int fullUpload = _persistent.LastUploadedVertices;
            Assert.Greater(fullUpload, 0, "the first build uploads the page");

            _doc.SetText("#num", "Skor 999");
            Step("same-shape text");

            // The point of the whole exercise: a one-element change must not
            // re-upload the page. Half is a generous bound; the real span is a
            // handful of glyph quads.
            Assert.Greater(_persistent.LastUploadedVertices, 0, "something changed, something uploads");
            Assert.Less(_persistent.LastUploadedVertices, fullUpload / 2,
                        "a one-element change uploads a span, not the page");
        }
    }
}
