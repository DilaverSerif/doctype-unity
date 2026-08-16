using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace Doctype.Tests
{
    /// <summary>
    /// The gauntlet: input the way a player actually delivers it, not the way
    /// a unit test politely does. Reload from inside a click handler, button
    /// spam, open/close toggling, drag-scroll storms, hover flicker over live
    /// mutations. Every test's real assertion is the same: the view survives
    /// and ends in the state the last input asked for.
    /// </summary>
    /// <remarks>
    /// The first test IS the crash a real game found on day one: a click
    /// handler that calls LoadHtml re-entered the ABI while the clicked
    /// document's native frames were still on the stack, the old document
    /// died mid-dispatch, and a pure virtual call took the editor down. It is
    /// held off by two layers now -- the native document pin and the deferred
    /// managed events -- and this suite is the regression tripwire for both.
    /// </remarks>
    public class HtmlInteractionGauntletTests
    {
        private GameObject _go;
        private HtmlView _view;

        private const string MenuPage =
            "<body style='margin:0;font-family:sans-serif'>" +
            "<div id='open' tabindex='0' style='width:120px;height:40px;background:#235'>Ac</div>" +
            "</body>";

        private const string PanelPage =
            "<body style='margin:0;font-family:sans-serif'>" +
            "<div id='close' tabindex='0' style='width:120px;height:40px;background:#523'>Kapat</div>" +
            "<div id='panel' style='width:200px;height:80px;background:#171c2b'>panel icerigi</div>" +
            "</body>";

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
            Object.DestroyImmediate(_go);
        }

        private IEnumerator Settle(int frames = 3)
        {
            for (int i = 0; i < frames; i++)
            {
                yield return null;
            }
        }

        private void Click(Vector2 at)
        {
            _view.PointerMove(at);
            _view.PointerDown(at);
            _view.PointerUp(at);
        }

        private static readonly Vector2 OnButton = new Vector2(60f, 20f);

        [UnityTest]
        public IEnumerator ClickHandlerThatReloadsTheDocumentSurvives()
        {
            // The day-one crash, as a regression test: open and close a panel
            // twenty times, each transition a LoadHtml from inside the click
            // handler of the page being destroyed.
            int opens = 0, closes = 0;

            _view.BindClick("open", _ => { opens++; _view.LoadHtml(PanelPage); });
            _view.BindClick("close", _ => { closes++; _view.LoadHtml(MenuPage); });

            _view.LoadHtml(MenuPage);
            yield return Settle();

            for (int i = 0; i < 20; i++)
            {
                Click(OnButton); // both buttons sit at the same spot on purpose
                yield return null;
            }

            Assert.AreEqual(10, opens, "every odd click opened");
            Assert.AreEqual(10, closes, "every even click closed");
            Assert.IsNotNull(_view.Texture, "and the surface is still alive");
        }

        [UnityTest]
        public IEnumerator AnchorHandlerThatReloadsSurvivesTheSameWay()
        {
            int navigations = 0;

            _view.Document.AnchorClicked += url =>
            {
                navigations++;
                _view.LoadHtml(url == "page://a"
                    ? "<body style='margin:0'><a id='go' href='page://b' " +
                      "style='display:block;width:120px;height:40px'>b</a></body>"
                    : "<body style='margin:0'><a id='go' href='page://a' " +
                      "style='display:block;width:120px;height:40px'>a</a></body>");
            };

            _view.LoadHtml("<body style='margin:0'><a id='go' href='page://a' " +
                           "style='display:block;width:120px;height:40px'>a</a></body>");
            yield return Settle();

            for (int i = 0; i < 10; i++)
            {
                Click(OnButton);
                yield return null;
            }

            Assert.AreEqual(10, navigations, "every click navigated");
        }

        [UnityTest]
        public IEnumerator ButtonSpamWithMutatingHandlerCountsEveryPress()
        {
            // A trigger-happy thumb: bursts of clicks, and the handler itself
            // mutates the document it was clicked in.
            int presses = 0;

            _view.BindClick("hit", _ =>
            {
                presses++;
                _view.SetText("#count", "Sayi " + presses);
            });

            _view.LoadHtml("<body style='margin:0;font-family:sans-serif'>" +
                           "<div id='hit' style='width:120px;height:40px;background:#235'>vur</div>" +
                           "<div id='count'>Sayi 0</div></body>");
            yield return Settle();

            for (int burst = 0; burst < 10; burst++)
            {
                for (int i = 0; i < 10; i++)
                {
                    Click(OnButton); // ten clicks inside one frame
                }
                yield return null;
            }

            Assert.AreEqual(100, presses, "no press was swallowed");
        }

        [UnityTest]
        public IEnumerator DragScrollStormThenAClickStillLands()
        {
            string rows = "";
            for (int i = 0; i < 30; i++)
            {
                rows += "<div style='height:24px'>satir " + i + "</div>";
            }

            _view.LoadHtml("<body style='margin:0;font-family:sans-serif'>" +
                           "<div id='btn' style='width:120px;height:30px;background:#235'>dur</div>" +
                           "<div id='vp' style='height:120px;overflow:auto'>" + rows + "</div></body>");
            yield return Settle();

            var inList = new Vector2(60f, 90f);

            // Five storms: press, sixty jittering moves and alternating scroll
            // deltas, release. The kind of input a thumb on glass produces.
            for (int storm = 0; storm < 5; storm++)
            {
                _view.PointerDown(inList);
                for (int i = 0; i < 60; i++)
                {
                    _view.PointerMove(inList + new Vector2(i % 7, i % 5));
                    _view.Scroll(new Vector2(0f, (i % 2 == 0 ? 12f : -9f)), inList);
                }
                _view.PointerUp(inList);
                yield return null;
            }

            int clicks = 0;
            _view.BindClick("btn", _ => clicks++);
            Click(new Vector2(60f, 15f));
            yield return null;

            Assert.AreEqual(1, clicks, "a clean click still lands after the storms");
        }

        [UnityTest]
        public IEnumerator HoverFlickerOverLiveMutationsStaysCoherent()
        {
            _view.LoadHtml("<body style='margin:0;font-family:sans-serif'><style>" +
                           ".b:hover { background:#802030; }" +
                           "</style>" +
                           "<div id='a' class='b' style='width:100px;height:40px;background:#235'>A</div>" +
                           "<div id='b' class='b' style='width:100px;height:40px;background:#235'>B</div>" +
                           "<div id='n' style='font-size:14px'>Sayi 0</div></body>");
            yield return Settle();

            // Flicker the pointer between the two hover targets while the
            // text below them mutates every pass: hover restyles, E1/E8 fast
            // paths and the quad cache all churn together.
            for (int i = 0; i < 50; i++)
            {
                _view.PointerMove(i % 2 == 0 ? new Vector2(50f, 20f) : new Vector2(50f, 60f));
                _view.SetText("#n", "Sayi " + i);
                if (i % 10 == 9)
                {
                    yield return null;
                }
            }

            _view.PointerExit();
            yield return Settle();

            Assert.IsNotNull(_view.Texture, "the view survived the flicker");
            Assert.IsTrue(_view.TryGetElementRect("#n", out Rect rect), "the mutated element is still laid out");
            Assert.Greater(rect.width, 0f);

            // The last mutation is the one on screen: pushing the same string
            // again reports "unchanged", which only holds if it really landed.
            Assert.IsFalse(_view.SetText("#n", "Sayi 49"), "the final text is the final input");
        }

        [UnityTest]
        public IEnumerator SliderDragEndsWhereTheFingerStopped()
        {
            // A slider the way a game builds one on this API: the thumb is a
            // div, the game reads ElementAt on press, applies SetStyle
            // margin-left per move, clamping to the track. The finger jitters,
            // overshoots both ends, and settles; the thumb must end exactly
            // under the last clamped position.
            _view.LoadHtml("<body style='margin:0'>" +
                           "<div id='track' style='margin:20px;width:220px;height:20px;background:#333'>" +
                           "<div id='thumb' style='width:20px;height:20px;background:#4a6'></div>" +
                           "</div></body>");
            yield return Settle();

            Assert.IsTrue(_view.TryGetElementRect("#track", out Rect track));
            const float thumbWidth = 20f;
            float travel = track.width - thumbWidth; // margin-left range [0, 200]

            float margin = 0f;
            void DragTo(float x)
            {
                _view.PointerMove(new Vector2(x, 30f));
                margin = Mathf.Clamp(x - track.x - thumbWidth / 2f, 0f, travel);
                _view.SetStyle("#thumb", "margin-left:" + margin + "px");
            }

            var grip = new Vector2(30f, 30f); // thumb center at rest
            _view.PointerMove(grip);
            _view.PointerDown(grip);
            Assert.AreEqual("thumb", _view.ElementAt(grip), "the press really grabbed the thumb");

            // Jitter forward, overshoot far right, whip back past the left
            // edge, then settle mid-track. Clamping happens in game code, the
            // way it would in production.
            for (int x = 30; x <= 150; x += 7)
            {
                DragTo(x + (x % 3)); // uneven steps
            }
            yield return null;
            DragTo(400f); // beyond the right end: clamps to travel
            yield return null;
            DragTo(-60f); // beyond the left end: clamps to 0
            yield return null;
            DragTo(127f); // the finger stops here
            _view.PointerUp(new Vector2(127f, 30f));
            yield return Settle();

            float expected = Mathf.Clamp(127f - track.x - thumbWidth / 2f, 0f, travel);
            Assert.IsTrue(_view.TryGetElementRect("#thumb", out Rect thumb));
            Assert.AreEqual(track.x + expected, thumb.x, 0.5f, "the thumb sits under the last position");

            // Pushing the final style again must report "unchanged": the last
            // move of the storm is the one that actually landed.
            Assert.IsFalse(_view.SetStyle("#thumb", "margin-left:" + expected + "px"),
                "the final margin is the final input");
        }

        [UnityTest]
        public IEnumerator ToggleSwitchParityUnderRapidClicking()
        {
            // A settings switch: every click flips it, so the final state is
            // the parity of the click count -- seven rapid clicks end ON, one
            // more ends OFF. The same screen point hits the switch body when
            // the knob is left and the knob itself when it is right, so the
            // test also proves child clicks bubble to the bound ancestor.
            int fired = 0;
            bool on = false;

            _view.BindClick("sw", _ =>
            {
                fired++;
                on = !on;
                _view.SetStyle("#knob", on
                    ? "margin-left:50px;background:#2a2"
                    : "margin-left:0px;background:#ccc");
            });

            _view.LoadHtml("<body style='margin:0'>" +
                           "<div id='sw' style='width:80px;height:30px;background:#555'>" +
                           "<div id='knob' style='width:30px;height:30px;background:#ccc'></div>" +
                           "</div></body>");
            yield return Settle();

            Assert.IsTrue(_view.TryGetElementRect("#sw", out Rect sw));
            var onSwitch = new Vector2(65f, 15f);

            // Seven clicks in two bursts: three in one frame, four in the next.
            for (int i = 0; i < 3; i++)
            {
                Click(onSwitch);
            }
            yield return null;
            for (int i = 0; i < 4; i++)
            {
                Click(onSwitch);
            }
            yield return Settle();

            Assert.AreEqual(7, fired, "every flip was counted");
            Assert.IsTrue(on, "odd number of clicks ends ON");
            Assert.IsTrue(_view.TryGetElementRect("#knob", out Rect knob));
            Assert.AreEqual(sw.x + 50f, knob.x, 0.5f, "the knob really moved right");

            Click(onSwitch); // the eighth click, through the knob this time
            yield return Settle();

            Assert.AreEqual(8, fired);
            Assert.IsFalse(on, "even number of clicks ends OFF");
            Assert.IsTrue(_view.TryGetElementRect("#knob", out knob));
            Assert.AreEqual(sw.x, knob.x, 0.5f, "and the knob came back");
        }

        [UnityTest]
        public IEnumerator PressThenDragOffAndReleaseIsNotAClick()
        {
            // The universal cancel gesture: press a button, change your mind,
            // slide off, let go. No click may fire -- litehtml only clicks
            // when the pressed and released element agree.
            int clicks = 0;
            _view.BindClick("btn", _ => clicks++);

            _view.LoadHtml("<body style='margin:0'>" +
                           "<div id='btn' style='width:120px;height:40px;background:#235'>iptal edilebilir</div>" +
                           "</body>");
            yield return Settle();

            // Press on the button, slide out in steps, release in empty space.
            _view.PointerMove(OnButton);
            _view.PointerDown(OnButton);
            _view.PointerMove(new Vector2(140f, 60f));
            _view.PointerMove(new Vector2(220f, 150f));
            _view.PointerUp(new Vector2(220f, 150f));
            yield return null;

            Assert.AreEqual(0, clicks, "sliding off cancelled the press");

            // The mirror image: press empty space, slide onto the button,
            // release there. Still not a click of the button.
            _view.PointerMove(new Vector2(220f, 150f));
            _view.PointerDown(new Vector2(220f, 150f));
            _view.PointerMove(OnButton);
            _view.PointerUp(OnButton);
            yield return null;

            Assert.AreEqual(0, clicks, "releasing on a button you never pressed is not a click");

            // And after both aborted gestures, a clean click still lands.
            Click(OnButton);
            yield return null;

            Assert.AreEqual(1, clicks, "the deliberate click fires exactly once");
        }
    }
}
