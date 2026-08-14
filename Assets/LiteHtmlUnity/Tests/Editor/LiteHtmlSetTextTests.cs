using System.Collections.Generic;
using LiteHtmlUnity;
using NUnit.Framework;
using Unity.Collections;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// Covers <see cref="LiteHtmlDocument.SetText"/>, the path that updates a
    /// counter or a label without re-parsing the document.
    /// </summary>
    /// <remarks>
    /// Native/tests/bench.cpp already proves byte-for-byte that a mutation
    /// equals a full re-parse. What that cannot reach is the marshalling
    /// boundary: the selector and the replacement text both cross as UTF-8, and
    /// the return value is an int the wrapper turns into a bool. These tests
    /// exist for that boundary, so they assert geometry rather than trusting it.
    /// </remarks>
    public class LiteHtmlSetTextTests
    {
        const string Page =
            "<body style='margin:0'><div id='label' style='font:16px ahem'>AB</div></body>";

        /// <summary>
        /// Copies out the fields a mutation is expected to move. Record aliases
        /// native memory that the next Record call overwrites, so comparing two
        /// recordings means copying the first one out.
        /// </summary>
        static List<(float x, float y, float w, float h, int type)> Snapshot(LiteHtmlDocument doc,
                                                                             out float height,
                                                                             out int glyphs)
        {
            NativeArray<LiteHtmlQuad> quads = doc.Record(out LiteHtmlFrame frame);
            var shot = new List<(float, float, float, float, int)>(frame.QuadCount);
            glyphs = 0;

            for (int i = 0; i < frame.QuadCount; i++)
            {
                LiteHtmlQuad q = quads[i];
                shot.Add((q.X, q.Y, q.W, q.H, q.TypeRaw));
                if (q.Type == LiteHtmlQuadType.Glyph)
                {
                    glyphs++;
                }
            }

            height = frame.DocHeight;
            return shot;
        }

        static LiteHtmlDocument Load(string html)
        {
            LiteHtmlDocument doc = LiteHtmlNativeTests.CreateDocument();
            Assert.IsTrue(doc.LoadHtml(html), "page should parse");
            doc.Layout(400f);
            return doc;
        }

        [Test]
        public void SetTextMatchesAFullReparse()
        {
            using LiteHtmlDocument mutated = Load(Page);
            Assert.IsTrue(mutated.SetText("#label", "ABCD"), "the text changed, so it should report true");
            mutated.Layout(400f);
            var after = Snapshot(mutated, out float mutatedHeight, out int mutatedGlyphs);

            using LiteHtmlDocument reparsed = Load(Page.Replace(">AB<", ">ABCD<"));
            var expected = Snapshot(reparsed, out float reparsedHeight, out int reparsedGlyphs);

            Assert.AreEqual(expected.Count, after.Count, "mutation should produce the same number of quads");
            Assert.AreEqual(reparsedGlyphs, mutatedGlyphs, "and the same number of glyphs");
            Assert.AreEqual(reparsedHeight, mutatedHeight, 0.01f, "and the same document height");

            for (int i = 0; i < expected.Count; i++)
            {
                Assert.AreEqual(expected[i], after[i], $"quad {i} should match the re-parsed document");
            }
        }

        [Test]
        public void SetTextRemeasuresWithExactAhemMetrics()
        {
            // Every Ahem glyph is exactly 1em wide, so at 16px "AB" is 32px and
            // "ABCD" is 64px. That makes this an equality, not a tolerance.
            using LiteHtmlDocument doc = Load(Page);
            Snapshot(doc, out _, out int before);
            Assert.AreEqual(2, before, "\"AB\" is two glyphs");

            Assert.IsTrue(doc.SetText("#label", "ABCD"));
            doc.Layout(400f);
            Snapshot(doc, out _, out int after);

            Assert.AreEqual(4, after, "\"ABCD\" is four glyphs");
        }

        [Test]
        public void SetTextReportsFalseWhenTheTextIsUnchanged()
        {
            using LiteHtmlDocument doc = Load(Page);

            // The caller uses this to skip a re-layout it does not need.
            Assert.IsFalse(doc.SetText("#label", "AB"), "identical text is not a change");
        }

        [Test]
        public void SetTextLeavesTheDocumentAloneWhenItRefuses()
        {
            using LiteHtmlDocument doc = Load(Page);
            var before = Snapshot(doc, out float beforeHeight, out _);

            Assert.IsFalse(doc.SetText("#missing", "x"), "no element matches this selector");
            Assert.IsFalse(doc.SetText("body", "x"), "body holds elements, not just text");

            doc.Layout(400f);
            var after = Snapshot(doc, out float afterHeight, out _);

            Assert.AreEqual(beforeHeight, afterHeight, 0.01f, "a refused mutation must not move anything");
            CollectionAssert.AreEqual(before, after);
        }

        [Test]
        public void SetTextRoundTripRestoresTheOriginal()
        {
            using LiteHtmlDocument doc = Load(Page);
            var original = Snapshot(doc, out float originalHeight, out _);

            Assert.IsTrue(doc.SetText("#label", "COMPLETELY DIFFERENT"));
            doc.Layout(400f);
            Snapshot(doc, out _, out _);

            Assert.IsTrue(doc.SetText("#label", "AB"));
            doc.Layout(400f);
            var restored = Snapshot(doc, out float restoredHeight, out _);

            Assert.AreEqual(originalHeight, restoredHeight, 0.01f);
            CollectionAssert.AreEqual(original, restored);
        }
    }
}
