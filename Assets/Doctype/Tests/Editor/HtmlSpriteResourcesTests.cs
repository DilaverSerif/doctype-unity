using System.Text.RegularExpressions;
using NUnit.Framework;
using Unity.Collections;
using UnityEngine;
using UnityEngine.TestTools;

namespace Doctype.Tests
{
    /// <summary>
    /// The sprite-backed provider: UVs straight from sprites that share one
    /// texture, no runtime packing, and loud refusal of anything that breaks
    /// the single-texture contract.
    /// </summary>
    public class HtmlSpriteResourcesTests
    {
        private GameObject _host;
        private HtmlSpriteResources _resources;
        private Texture2D _sheet;
        private Texture2D _foreign;
        private Sprite _gold;
        private Sprite _gem;
        private Sprite _stray;

        [SetUp]
        public void SetUp()
        {
            _host = new GameObject("SpriteResources");
            _resources = _host.AddComponent<HtmlSpriteResources>();

            _sheet = new Texture2D(64, 64, TextureFormat.RGBA32, false);
            var pixels = new Color32[64 * 64];
            for (int i = 0; i < pixels.Length; i++)
            {
                pixels[i] = new Color32(0, 200, 80, 255);
            }
            _sheet.SetPixels32(pixels);
            _sheet.Apply();

            _foreign = new Texture2D(16, 16, TextureFormat.RGBA32, false);

            _gold = Sprite.Create(_sheet, new Rect(0, 0, 32, 32), new Vector2(0.5f, 0.5f));
            _gem = Sprite.Create(_sheet, new Rect(32, 0, 16, 24), new Vector2(0.5f, 0.5f));
            _stray = Sprite.Create(_foreign, new Rect(0, 0, 16, 16), new Vector2(0.5f, 0.5f));
        }

        [TearDown]
        public void TearDown()
        {
            Object.DestroyImmediate(_host);
            Object.DestroyImmediate(_gold);
            Object.DestroyImmediate(_gem);
            Object.DestroyImmediate(_stray);
            Object.DestroyImmediate(_sheet);
            Object.DestroyImmediate(_foreign);
        }

        [Test]
        public void UvsComeStraightFromTheSpriteRects()
        {
            _resources.Register("gold", _gold);
            _resources.Register("gem", _gem);

            Assert.AreSame(_sheet, _resources.ImageAtlas, "the shared texture is the atlas");

            Assert.IsTrue(_resources.TryGetImageSize("gem", out int w, out int h));
            Assert.AreEqual(16, w);
            Assert.AreEqual(24, h);

            Assert.IsTrue(_resources.TryGetImageUv("gem", out Rect uv));
            Assert.AreEqual(32f / 64f, uv.x, 1e-4f);
            Assert.AreEqual(0f, uv.y, 1e-4f);
            Assert.AreEqual(16f / 64f, uv.width, 1e-4f);
            Assert.AreEqual(24f / 64f, uv.height, 1e-4f);
        }

        [Test]
        public void ASpriteFromAnotherTextureIsRefusedByName()
        {
            _resources.Register("gold", _gold);

            LogAssert.Expect(LogType.Error, new Regex("stray.*One page per provider|lives on texture"));
            _resources.Register("stray", _stray);

            Assert.IsFalse(_resources.TryGetImageUv("stray", out _), "the foreign sprite got no UVs");
            Assert.IsTrue(_resources.TryGetImageUv("gold", out _), "and the valid one is untouched");
            Assert.AreSame(_sheet, _resources.ImageAtlas, "the atlas stays the first texture");
        }

        [Test]
        public void ADocumentDrawsTheSpriteAtItsIntrinsicSize()
        {
            _resources.Register("gold", _gold);

            HtmlDocument doc = HtmlNativeTests.CreateDocument();
            try
            {
                doc.Resources = _resources;
                doc.SetViewport(64, 64);
                Assert.IsTrue(doc.LoadHtml("<body style='margin:0'><img src='gold'></body>"), doc.LastError);
                doc.Layout(64);

                NativeArray<HtmlQuad> quads = doc.Record(out HtmlFrame frame);

                bool found = false;
                for (int i = 0; i < frame.QuadCount; i++)
                {
                    if (quads[i].Type == HtmlQuadType.Image)
                    {
                        found = true;
                        Assert.AreEqual(32f, quads[i].W, 0.5f, "intrinsic width from sprite.rect");
                        Assert.AreEqual(32f, quads[i].H, 0.5f, "intrinsic height from sprite.rect");
                        Assert.Greater(quads[i].U1, quads[i].U0, "with real atlas UVs");
                    }
                }

                Assert.IsTrue(found, "an image quad was recorded");
            }
            finally
            {
                doc.Dispose();
            }
        }
    }
}
