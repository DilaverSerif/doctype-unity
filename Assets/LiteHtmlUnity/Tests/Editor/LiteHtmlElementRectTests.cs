using LiteHtmlUnity;
using NUnit.Framework;
using UnityEngine;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// Covers <see cref="LiteHtmlDocument.TryGetElementRect"/>, which exists so a
    /// host can size a surface to a badge or a counter instead of to the whole
    /// screen.
    /// </summary>
    /// <remarks>
    /// Uses Ahem, whose every glyph is exactly 1em wide, so the widths below are
    /// equalities rather than tolerances.
    /// </remarks>
    public class LiteHtmlElementRectTests
    {
        // 16px Ahem: one glyph is 16px, so "12" is 32px and "123456" is 96px.
        // 10px of padding either side puts the painted box 20px wider than that.
        const float Glyph = 16f;
        const float PadX = 10f;
        const float PadY = 6f;

        static string Badge(string text) =>
            "<body style='margin:0'><div id='hud' style='display:inline-block;" +
            $"padding:{PadY}px {PadX}px;background:#c9a227'>{text}</div></body>";

        static LiteHtmlDocument Load(string html, float viewport = 1000f)
        {
            LiteHtmlDocument doc = LiteHtmlNativeTests.CreateDocument();
            doc.SetViewport(viewport, 400f);
            Assert.IsTrue(doc.LoadHtml(html), "page should parse");
            doc.Layout(viewport);
            return doc;
        }

        [Test]
        public void RectWidthFollowsContentWhileTheDocumentDoesNot()
        {
            // The whole reason this API exists: the document fills its viewport,
            // so DocumentWidth cannot say how wide the badge is.
            using LiteHtmlDocument shortText = Load(Badge("12"));
            Assert.IsTrue(shortText.TryGetElementRect("#hud", out Rect small));

            using LiteHtmlDocument longText = Load(Badge("123456"));
            Assert.IsTrue(longText.TryGetElementRect("#hud", out Rect big));

            Assert.AreEqual(2 * Glyph + 2 * PadX, small.width, 0.01f);
            Assert.AreEqual(6 * Glyph + 2 * PadX, big.width, 0.01f);

            Assert.AreEqual(1000f, shortText.DocumentWidth, 0.01f, "the document still fills the viewport");
            Assert.AreEqual(1000f, longText.DocumentWidth, 0.01f);
        }

        [Test]
        public void RectIsThePaintedBoxNotTheContentBox()
        {
            // Sizing a surface to the content box would clip the background by
            // exactly the padding, which is the bug this guards.
            using LiteHtmlDocument doc = Load(Badge("12"));
            Assert.IsTrue(doc.TryGetElementRect("#hud", out Rect rect));

            Assert.AreEqual(0f, rect.x, 0.01f, "the badge sits at the document origin");
            Assert.AreEqual(0f, rect.y, 0.01f);
            Assert.AreEqual(2 * Glyph + 2 * PadX, rect.width, 0.01f, "padding is included");
            Assert.Greater(rect.height, 2 * PadY, "so is vertical padding");
            Assert.AreEqual(doc.DocumentHeight, rect.height, 0.01f,
                            "and the badge is the only thing in the document");
        }

        [Test]
        public void MeasurementSurvivesATextUpdate()
        {
            // The cheap update path must leave the box measurable, since that is
            // exactly when a counter needs re-measuring.
            using LiteHtmlDocument doc = Load(Badge("12"));
            Assert.IsTrue(doc.TryGetElementRect("#hud", out Rect before));

            Assert.IsTrue(doc.SetText("#hud", "123456"));
            doc.Layout(1000f);

            Assert.IsTrue(doc.TryGetElementRect("#hud", out Rect after));
            Assert.AreEqual(2 * Glyph + 2 * PadX, before.width, 0.01f);
            Assert.AreEqual(6 * Glyph + 2 * PadX, after.width, 0.01f);
        }

        [Test]
        public void UnmatchedSelectorReportsFailureAndSaysWhy()
        {
            using LiteHtmlDocument doc = Load(Badge("12"));

            Assert.IsFalse(doc.TryGetElementRect("#missing", out Rect rect));
            Assert.AreEqual(default(Rect), rect, "a failed query must not hand back a stale rectangle");
            Assert.IsNotEmpty(doc.LastError);
        }

        // --- hit testing -------------------------------------------------------

        /// <summary>
        /// Three slots in a row. Uses flex, because inline-block slots do not
        /// line up: litehtml aligns them on the text baseline and ignores
        /// vertical-align, so a slot holding an icon sits lower than an empty
        /// one. float:left works too; inline-block and table-cell do not.
        /// </summary>
        static string Grid() =>
            "<body style='margin:0'><div style='display:flex'>" +
            "<div id='slot0' style='width:100px;height:100px'>" +
            "<div style='width:60px;height:60px;margin:20px'>x</div></div>" +
            "<div id='slot1' style='width:100px;height:100px'>" +
            "<div style='width:60px;height:60px;margin:20px'>x</div></div>" +
            "<div id='slot2' style='width:100px;height:100px'></div>" +
            "</div></body>";

        [Test]
        public void HitTestReportsTheSlotUnderAPoint()
        {
            using LiteHtmlDocument doc = Load(Grid(), 400f);

            Assert.AreEqual("slot0", doc.ElementAt(new Vector2(50f, 50f)), "over slot0's icon");
            Assert.AreEqual("slot0", doc.ElementAt(new Vector2(5f, 5f)), "and over its bare corner");
            Assert.AreEqual("slot1", doc.ElementAt(new Vector2(150f, 50f)));
            Assert.AreEqual("slot2", doc.ElementAt(new Vector2(250f, 50f)), "an empty slot still answers");
        }

        [Test]
        public void HitTestReturnsNullOffAnySlot()
        {
            using LiteHtmlDocument doc = Load(Grid(), 400f);

            Assert.IsNull(doc.ElementAt(new Vector2(350f, 50f)), "right of the last slot");
            Assert.IsNull(doc.ElementAt(new Vector2(150f, 300f)), "below the row");
            Assert.IsNull(doc.ElementAt(new Vector2(-10f, 50f)), "outside the document");
        }

        [Test]
        public void HitTestLeavesHoverStateAlone()
        {
            // The point of a separate query: probing a drop target must not light
            // the element up as hovered.
            using LiteHtmlDocument doc = Load(Grid(), 400f);

            NativeArrayCount(doc, out int before);
            doc.ElementAt(new Vector2(50f, 50f));
            NativeArrayCount(doc, out int after);

            Assert.AreEqual(before, after, "a hit test must not change what gets recorded");
        }

        static void NativeArrayCount(LiteHtmlDocument doc, out int quads)
        {
            doc.Record(out LiteHtmlFrame frame);
            quads = frame.QuadCount;
        }
    }
}
