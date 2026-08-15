using System.IO;
using Doctype;
using NUnit.Framework;
using Unity.Collections;
using UnityEngine;

namespace Doctype.Tests
{
    /// <summary>
    /// Exercises the native layer through the same P/Invoke surface the runtime
    /// uses. These assertions mirror Native/tests/harness.cpp, so a failure here
    /// that passes there points at the marshalling boundary rather than at
    /// litehtml.
    /// </summary>
    public class HtmlNativeTests
    {
        /// <summary>
        /// Ahem is a test typeface where every glyph is exactly 1em wide and
        /// fills the box from 0.8em above to 0.2em below the baseline. That
        /// makes text layout assertions exact instead of approximate.
        /// </summary>
        internal static string AhemPath =>
            Path.GetFullPath(Path.Combine(Application.dataPath,
                "../Native/third_party/litehtml/containers/test/fonts/ahem.ttf"));

        internal static HtmlDocument CreateDocument(bool ahem = true)
        {
            var doc = new HtmlDocument();

            if (File.Exists(AhemPath))
            {
                Assert.IsTrue(doc.RegisterFont("ahem", File.ReadAllBytes(AhemPath)), "ahem.ttf should parse");
            }

            foreach (HtmlFontEntry entry in HtmlSystemFonts.Discover())
            {
                byte[] data = entry.Resolve();
                if (data != null)
                {
                    doc.RegisterFont(entry.Family, data, entry.Weight, entry.Italic);
                }
            }

            doc.SetDefaultFont(ahem ? "ahem" : "sans-serif", 16f);
            doc.SetViewport(400f, 300f);
            return doc;
        }

        [Test]
        public void PluginLoadsAndReportsVersion()
        {
            Assert.IsNotEmpty(HtmlNative.Version(), "native plugin should report a version");
        }

        [Test]
        public void QuadLayoutMatchesNative()
        {
            // A silent drift here would corrupt every quad, so it is asserted
            // rather than trusted.
            Assert.DoesNotThrow(HtmlNative.AssertLayout);
            Assert.AreEqual(144, HtmlNative.lhu_quad_size(), "LhuQuad is expected to be 144 bytes");
        }

        [Test]
        public void SystemFontsAreDiscoverable()
        {
            var fonts = HtmlSystemFonts.Discover();
            Assert.IsNotEmpty(fonts, "the test platform should expose at least one usable system font");
        }

        [Test]
        public void SolidDivProducesOneExactRect()
        {
            using var doc = CreateDocument();

            Assert.IsTrue(doc.LoadHtml(
                "<body style='margin:0'><div style='width:100px;height:50px;background:#ff0000'></div></body>"));

            doc.Layout(400f);

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);
            Assert.Greater(frame.QuadCount, 0, "the document should record at least one quad");

            HtmlQuad? found = null;
            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type == HtmlQuadType.Rect)
                {
                    found = quads[i];
                    break;
                }
            }

            Assert.IsTrue(found.HasValue, "expected a Rect quad");
            HtmlQuad quad = found.Value;

            Assert.AreEqual(0f, quad.X, 0.01f);
            Assert.AreEqual(0f, quad.Y, 0.01f);
            Assert.AreEqual(100f, quad.W, 0.01f);
            Assert.AreEqual(50f, quad.H, 0.01f);

            // Packed as R | G<<8 | B<<16 | A<<24.
            Assert.AreEqual(0xFF0000FFu, quad.Color, "opaque red");
        }

        [Test]
        public void AhemMetricsAreExact()
        {
            using var doc = CreateDocument();

            Assert.IsTrue(doc.LoadHtml(
                "<body style='margin:0'><div style=\"font:100px ahem;line-height:1\">AA</div></body>"));

            doc.Layout(400f);

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

            var glyphs = new System.Collections.Generic.List<HtmlQuad>();
            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type == HtmlQuadType.Glyph)
                {
                    glyphs.Add(quads[i]);
                }
            }

            Assert.AreEqual(2, glyphs.Count, "two glyph quads");
            Assert.AreEqual(0f, glyphs[0].X, 1f, "first glyph starts at the content edge");
            Assert.AreEqual(100f, glyphs[1].X - glyphs[0].X, 1f, "advance is exactly 1em");
            Assert.AreEqual(100f, glyphs[0].W, 1f, "glyph box is 1em wide");
        }

        [Test]
        public void BordersSplitIntoFourEdges()
        {
            using var doc = CreateDocument();

            Assert.IsTrue(doc.LoadHtml(
                "<body style='margin:0'><div style='width:100px;height:60px;" +
                "border-top:4px solid #ff0000;border-right:8px solid #00ff00;" +
                "border-bottom:12px solid #0000ff;border-left:16px solid #ffff00'></div></body>"));

            doc.Layout(400f);

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

            var edges = new bool[4];
            int borderQuads = 0;

            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type != HtmlQuadType.Border)
                {
                    continue;
                }

                borderQuads++;
                int edge = Mathf.RoundToInt(quads[i].P0);
                if (edge >= 0 && edge < 4)
                {
                    edges[edge] = true;
                }

                // Every edge quad carries all four widths so the shader can
                // derive the inner rounded rect.
                Assert.AreEqual(16f, quads[i].BorderL, 0.01f);
                Assert.AreEqual(4f, quads[i].BorderT, 0.01f);
            }

            Assert.AreEqual(4, borderQuads, "one quad per visible edge");
            CollectionAssert.AreEqual(new[] { true, true, true, true }, edges, "all four edges present");
        }

        [Test]
        public void HoverChangesRecordedColor()
        {
            using var doc = CreateDocument();

            Assert.IsTrue(doc.LoadHtml(
                "<body style='margin:0'><style>" +
                "#b{width:100px;height:100px;background:#ff0000}" +
                "#b:hover{background:#0000ff}" +
                "</style><div id='b'></div></body>"));

            doc.Layout(400f);
            Assert.AreEqual(0xFF0000FFu, FirstRectColor(doc), "starts red");

            Assert.AreNotEqual(HtmlDirty.None, doc.MouseMove(new Vector2(50f, 50f)),
                               "moving over the element reports a change");
            Assert.AreEqual(0xFFFF0000u, FirstRectColor(doc), "turns blue while hovered");

            doc.MouseMove(new Vector2(350f, 250f));
            Assert.AreEqual(0xFF0000FFu, FirstRectColor(doc), "returns to red after leaving");
        }

        [Test]
        public void OverflowHiddenAttachesClipRect()
        {
            using var doc = CreateDocument();

            Assert.IsTrue(doc.LoadHtml(
                "<body style='margin:0'>" +
                "<div style='width:60px;height:60px;overflow:hidden'>" +
                "<div style='width:200px;height:200px;background:#00ff00'></div>" +
                "</div></body>"));

            doc.Layout(400f);

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

            bool clipped = false;
            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type == HtmlQuadType.Rect && quads[i].IsClipped)
                {
                    clipped = true;
                    Assert.AreEqual(60f, quads[i].ClipW, 0.01f, "clip follows the overflow box");
                }
            }

            Assert.IsTrue(clipped, "the oversized fill should carry a clip rect");
        }

        [Test]
        public void GradientBakesLutRow()
        {
            using var doc = CreateDocument();

            Assert.IsTrue(doc.LoadHtml(
                "<body style='margin:0'><div style='width:200px;height:100px;" +
                "background:linear-gradient(to right,#ff0000,#0000ff)'></div></body>"));

            doc.Layout(400f);

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

            int gradients = 0;
            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type == HtmlQuadType.LinearGradient)
                {
                    gradients++;
                    Assert.AreEqual(0, quads[i].GradRow);
                }
            }

            Assert.AreEqual(1, gradients);
            Assert.GreaterOrEqual(frame.GradLutRows, 1, "a LUT row should have been baked");
            Assert.AreEqual(256, frame.GradLutWidth);
        }

        [Test]
        public void LoadingWithoutFontsFailsCleanly()
        {
            // Reaching layout with no font would divide by a zero line height
            // deep inside litehtml, so the native side refuses early.
            using var doc = new HtmlDocument();
            Assert.IsFalse(doc.LoadHtml("<p>hello</p>"));
            Assert.IsNotEmpty(doc.LastError);
        }

        private static uint FirstRectColor(HtmlDocument doc)
        {
            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type == HtmlQuadType.Rect)
                {
                    return quads[i].Color;
                }
            }

            return 0;
        }
    }
}
