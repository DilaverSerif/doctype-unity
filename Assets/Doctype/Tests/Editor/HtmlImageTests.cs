using Doctype;
using NUnit.Framework;
using Unity.Collections;
using UnityEngine;

namespace Doctype.Tests
{
    /// <summary>
    /// End-to-end cover for using a project texture in markup. Nothing in this
    /// path had ever run: the resource interface existed but no class implemented
    /// it, so image quads were always skipped.
    /// </summary>
    public class HtmlImageTests
    {
        const int IconSize = 16;
        static readonly Color32 IconColor = new Color32(0, 200, 80, 255);

        GameObject _host;
        HtmlResources _resources;
        Texture2D _icon;
        RenderTexture _target;
        Texture2D _readback;
        HtmlRenderer _renderer;

        [SetUp]
        public void SetUp()
        {
            _host = new GameObject("Resources");
            _resources = _host.AddComponent<HtmlResources>();

            // Built in code so it is readable by construction, which is what
            // packing needs.
            _icon = new Texture2D(IconSize, IconSize, TextureFormat.RGBA32, false);
            var pixels = new Color32[IconSize * IconSize];
            for (int i = 0; i < pixels.Length; i++)
            {
                pixels[i] = IconColor;
            }
            _icon.SetPixels32(pixels);
            _icon.Apply();

            _resources.Register("gold", _icon);

            _target = new RenderTexture(64, 64, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
            _target.Create();
            _readback = new Texture2D(64, 64, TextureFormat.RGBA32, false, false);
            _renderer = new HtmlRenderer();
        }

        [TearDown]
        public void TearDown()
        {
            _renderer?.Dispose();
            if (_target != null) { _target.Release(); Object.DestroyImmediate(_target); }
            if (_readback != null) Object.DestroyImmediate(_readback);
            if (_icon != null) Object.DestroyImmediate(_icon);
            if (_host != null) Object.DestroyImmediate(_host);
        }

        HtmlDocument Load(string html)
        {
            HtmlDocument doc = HtmlNativeTests.CreateDocument();
            doc.Resources = _resources;
            doc.SetViewport(64, 64);
            Assert.IsTrue(doc.LoadHtml(html), doc.LastError);
            doc.Layout(64);
            return doc;
        }

        [Test]
        public void ProviderReportsTheTextureSizeBeforeAnythingIsPacked()
        {
            // Layout asks for this first, so it must not depend on the atlas.
            Assert.IsTrue(_resources.TryGetImageSize("gold", out int w, out int h));
            Assert.AreEqual(IconSize, w);
            Assert.AreEqual(IconSize, h);
        }

        [Test]
        public void UnknownNameIsReportedRatherThanGuessed()
        {
            Assert.IsFalse(_resources.TryGetImageSize("nope", out _, out _));
            Assert.IsFalse(_resources.TryGetImageUv("nope", out _));
        }

        [Test]
        public void ImageIsLaidOutAtItsIntrinsicSize()
        {
            using HtmlDocument doc = Load(
                "<body style='margin:0'><img id='ico' src='gold'></body>");

            Assert.IsTrue(doc.TryGetElementRect("#ico", out Rect rect));
            Assert.AreEqual(IconSize, rect.width, 0.5f, "an img with no CSS size takes the texture's");
            Assert.AreEqual(IconSize, rect.height, 0.5f);
        }

        [Test]
        public void RecordingEmitsAnImageQuadWithAtlasUvs()
        {
            using HtmlDocument doc = Load(
                "<body style='margin:0'><img id='ico' src='gold'></body>");

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

            bool found = false;
            for (int i = 0; i < frame.QuadCount; i++)
            {
                if (quads[i].Type != HtmlQuadType.Image)
                {
                    continue;
                }

                found = true;
                HtmlQuad q = quads[i];
                Assert.AreNotEqual(q.U0, q.U1, "the quad should span a real region of the atlas");
                Assert.AreNotEqual(q.V0, q.V1);
                break;
            }

            Assert.IsTrue(found, "an <img> should produce an Image quad");
            Assert.IsNotNull(_resources.ImageAtlas, "and the atlas should exist by now");
        }

        [Test]
        public void ImageActuallyDrawsItsPixels()
        {
            using HtmlDocument doc = Load(
                "<body style='margin:0'><img src='gold' style='width:64px;height:64px'></body>");

            NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);
            _renderer.Render(quads, frame, _target, Color.clear, new Vector2(64, 64), _resources.ImageAtlas);

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = _target;
            _readback.ReadPixels(new Rect(0, 0, 64, 64), 0, 0);
            _readback.Apply(false, false);
            RenderTexture.active = previous;

            Color32 c = _readback.GetPixel(32, 32);
            Assert.AreEqual(IconColor.g, c.g, 12, $"the icon's green should reach the surface -> {c}");
            Assert.Greater(c.g, c.r + 40, $"and it should not be washed out -> {c}");
            Assert.AreEqual(255, c.a, 6, $"an opaque icon stays opaque -> {c}");
        }

        [Test]
        public void VersionBumpsWhenAnImageBecomesAvailable()
        {
            // The view watches this to re-layout and to drop cached draw
            // commands that still hold the old UVs.
            int before = _resources.Version;
            _resources.BeginLoadImage("gold");
            Assert.Greater(_resources.Version, before);
        }
    }
}
