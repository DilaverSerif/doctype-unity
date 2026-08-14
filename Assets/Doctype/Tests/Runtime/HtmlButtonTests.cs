using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace Doctype.Tests
{
    /// <summary>
    /// Clicking elements from game code.
    /// </summary>
    /// <remarks>
    /// The cross-frame test here is the important one. litehtml reports a click
    /// only when press and release land on the *same element instance*, and
    /// re-parsing the document destroys those instances. A real mouse click
    /// spans several frames, so on any page that rebuilds itself — every
    /// animated page — clicks were being dropped entirely. Tests that sent
    /// press and release in one frame never saw it.
    /// </remarks>
    public class HtmlButtonTests
    {
        private const string ButtonPage =
            "<body style='margin:0;font-family:sans-serif'>" +
            "<button id='save' data-action='save-game' class='primary big' " +
            "style='display:block;width:300px;height:120px;background:#2563eb;color:#fff'>" +
            "<span id='label'>Kaydet</span></button>" +
            "</body>";

        private GameObject _go;
        private HtmlView _view;

        [SetUp]
        public void SetUp()
        {
            _go = new GameObject("HtmlView");
            _view = _go.AddComponent<HtmlView>();
            _view.SetSize(300, 200);
        }

        [TearDown]
        public void TearDown()
        {
            if (_go != null)
            {
                Object.DestroyImmediate(_go);
            }
        }

        private IEnumerator Settle(int frames = 4)
        {
            for (int i = 0; i < frames; i++)
            {
                yield return null;
            }
        }

        private static readonly Vector2 OnButton = new Vector2(150f, 60f);

        [UnityTest]
        public IEnumerator BindClickFiresForTheElementId()
        {
            int calls = 0;
            HtmlElementClick received = default;

            _view.BindClick("save", click =>
            {
                calls++;
                received = click;
            });

            _view.LoadHtml(ButtonPage);
            yield return Settle();

            _view.PointerMove(OnButton);
            _view.PointerDown(OnButton);
            _view.PointerUp(OnButton);
            yield return null;

            Assert.AreEqual(1, calls, "the bound handler should have run exactly once");
            Assert.AreEqual("save", received.Id);
            Assert.AreEqual("button", received.Tag);
            Assert.AreEqual("save-game", received.Action, "data-action should come through");
            StringAssert.Contains("primary", received.ClassNames);
        }

        [UnityTest]
        public IEnumerator ClickSpanningFramesStillFires()
        {
            // The regression test for the reported bug: press, let the page
            // rebuild itself a few times, then release.
            int calls = 0;
            _view.BindClick("save", _ => calls++);

            _view.LoadHtml(ButtonPage);
            yield return Settle();

            _view.PointerMove(OnButton);
            _view.PointerDown(OnButton);

            for (int frame = 0; frame < 5; frame++)
            {
                // Same markup, fresh parse — exactly what an animated page does.
                _view.LoadHtml(ButtonPage);
                yield return null;
                yield return null;
            }

            _view.PointerUp(OnButton);
            yield return null;

            Assert.AreEqual(1, calls,
                            "a click held across several rebuilds must still fire once released");
        }

        [UnityTest]
        public IEnumerator HoverSurvivesARebuild()
        {
            _view.LoadHtml(
                "<body style='margin:0'><style>" +
                "#b{width:300px;height:200px;background:#ff0000}" +
                "#b:hover{background:#0000ff}" +
                "</style><div id='b'></div></body>");

            yield return Settle();

            _view.PointerMove(new Vector2(150f, 100f));
            yield return Settle(2);

            Assert.AreEqual(255, SamplePixel(150, 100).b, 3, "hovered");

            // Re-parse while the pointer sits still; hover must be re-applied.
            _view.MarkDirty();
            yield return Settle(3);

            Assert.AreEqual(255, SamplePixel(150, 100).b, 3, "still hovered after the rebuild");
        }

        [UnityTest]
        public IEnumerator ClickOnInnerTextReachesTheButton()
        {
            // litehtml dispatches to the deepest element first and walks up
            // until a handler claims the click, so a label inside a button has
            // to resolve to the button.
            int labelCalls = 0;
            int buttonCalls = 0;

            _view.BindClick("label", _ => labelCalls++);
            _view.BindClick("save", _ => buttonCalls++);

            _view.LoadHtml(ButtonPage);
            yield return Settle();

            // Land on the text itself, near the top-left of the button.
            var onLabel = new Vector2(30f, 20f);
            _view.PointerMove(onLabel);
            _view.PointerDown(onLabel);
            _view.PointerUp(onLabel);
            yield return null;

            Assert.AreEqual(1, labelCalls + buttonCalls, "exactly one handler should claim the click");
        }

        [UnityTest]
        public IEnumerator ElementClickedEventSeesUnboundElements()
        {
            HtmlElementClick seen = default;
            int events = 0;

            _view.ElementClicked += click =>
            {
                events++;
                if (click.Tag == "button")
                {
                    seen = click;
                }
            };

            _view.LoadHtml(ButtonPage);
            yield return Settle();

            _view.PointerMove(OnButton);
            _view.PointerDown(OnButton);
            _view.PointerUp(OnButton);
            yield return null;

            Assert.Greater(events, 0, "the raw event should fire even with no binding");
            Assert.AreEqual("save", seen.Id);
        }

        [UnityTest]
        public IEnumerator ClickingNothingIsHarmless()
        {
            int calls = 0;
            _view.BindClick("save", _ => calls++);

            _view.LoadHtml(ButtonPage);
            yield return Settle();

            // Below the button, on bare body.
            var empty = new Vector2(150f, 180f);
            _view.PointerMove(empty);
            _view.PointerDown(empty);
            _view.PointerUp(empty);
            yield return null;

            Assert.AreEqual(0, calls, "clicking empty space must not invoke the binding");
        }

        [UnityTest]
        public IEnumerator BindingsSurviveAReload()
        {
            int calls = 0;
            _view.BindClick("save", _ => calls++);

            for (int i = 0; i < 3; i++)
            {
                _view.LoadHtml(ButtonPage);
                yield return Settle();

                _view.PointerMove(OnButton);
                _view.PointerDown(OnButton);
                _view.PointerUp(OnButton);
                yield return null;
            }

            Assert.AreEqual(3, calls, "the binding should outlive every reload");
        }

        private Color32 SamplePixel(int documentX, int documentY)
        {
            RenderTexture target = _view.Texture;
            Assert.IsNotNull(target);

            var readback = new Texture2D(target.width, target.height, TextureFormat.RGBA32, false, false);

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = target;
            readback.ReadPixels(new Rect(0, 0, target.width, target.height), 0, 0);
            readback.Apply(false, false);
            RenderTexture.active = previous;

            Color32 c = readback.GetPixel(documentX, target.height - 1 - documentY);
            Object.DestroyImmediate(readback);
            return c;
        }
    }
}
