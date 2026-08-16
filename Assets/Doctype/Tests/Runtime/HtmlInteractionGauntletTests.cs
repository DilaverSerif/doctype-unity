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
    }
}
