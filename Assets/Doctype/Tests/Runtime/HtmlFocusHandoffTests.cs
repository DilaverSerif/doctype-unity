using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace Doctype.Tests
{
    /// <summary>
    /// Focus handoff between panels: a HUD built the Doctype way is several
    /// independent HtmlViews, and the D-pad must not stop at a surface edge.
    /// The single-view metric is proven in the native harness; these prove
    /// the cross-panel transaction — hand over only when the neighbour really
    /// takes it, remember the seat, come back to it.
    /// </summary>
    public class HtmlFocusHandoffTests
    {
        private const string LeftPage =
            "<body style='margin:0'><style>" +
            ".b{position:absolute;width:60px;height:40px;background:#333}" +
            ".b:focus{background:#00f}" +
            "</style>" +
            "<div id='l1' class='b' tabindex='0' style='left:0;top:0'></div>" +
            "<div id='l2' class='b' tabindex='0' style='left:100px;top:0'></div>" +
            "</body>";

        private const string RightPage =
            "<body style='margin:0'><style>" +
            ".b{position:absolute;width:60px;height:40px;background:#333}" +
            ".b:focus{background:#00f}" +
            "</style>" +
            "<div id='r1' class='b' tabindex='0' style='left:0;top:0'></div>" +
            "<div id='r2' class='b' tabindex='0' style='left:100px;top:0'></div>" +
            "</body>";

        private GameObject _leftGo;
        private GameObject _rightGo;
        private HtmlFocusNavigator _left;
        private HtmlFocusNavigator _right;

        [SetUp]
        public void SetUp()
        {
            _leftGo = new GameObject("Left");
            _rightGo = new GameObject("Right");

            _leftGo.AddComponent<HtmlView>().SetSize(300, 100);
            _rightGo.AddComponent<HtmlView>().SetSize(300, 100);

            _left = _leftGo.AddComponent<HtmlFocusNavigator>();
            _right = _rightGo.AddComponent<HtmlFocusNavigator>();

            _left.Right = _right;
            _right.Left = _left;
        }

        [TearDown]
        public void TearDown()
        {
            Object.DestroyImmediate(_leftGo);
            Object.DestroyImmediate(_rightGo);
        }

        private IEnumerator LoadBoth()
        {
            _left.View.LoadHtml(LeftPage);
            _right.View.LoadHtml(RightPage);

            for (int i = 0; i < 4; i++)
            {
                yield return null;
            }
        }

        [UnityTest]
        public IEnumerator WalkingOffTheEdgeEntersTheNeighbour()
        {
            yield return LoadBoth();

            _left.View.SetFocus("#l2");
            Assert.IsTrue(_left.Move(HtmlNavDirection.Right), "the edge move succeeds via handoff");

            Assert.IsNull(_left.View.FocusedId, "the giver let go");
            Assert.AreEqual("r1", _right.View.FocusedId, "the neighbour entered in reading order");
        }

        [UnityTest]
        public IEnumerator ComingBackRestoresTheRememberedSeat()
        {
            yield return LoadBoth();

            // Leave the left panel from l2, wander in the right panel, and
            // come back: the seat must be l2 again, not the top-left default.
            _left.View.SetFocus("#l2");
            _left.Move(HtmlNavDirection.Right);

            Assert.IsTrue(_right.Move(HtmlNavDirection.Right), "r1 -> r2 inside the panel");
            Assert.AreEqual("r2", _right.View.FocusedId);
            Assert.IsTrue(_right.Move(HtmlNavDirection.Left), "r2 -> r1");

            Assert.IsTrue(_right.Move(HtmlNavDirection.Left), "and off the edge, back to the left panel");
            Assert.AreEqual("l2", _left.View.FocusedId, "which restores the remembered element");
            Assert.IsNull(_right.View.FocusedId, "and the right panel let go");
        }

        [UnityTest]
        public IEnumerator AnEmptyNeighbourRefusesTheHandoffAndFocusStays()
        {
            yield return LoadBoth();

            // The right panel reloads to a page with nothing focusable: the
            // handoff must fail as a whole and the giver must keep its focus,
            // exactly as if no neighbour were wired.
            _right.View.LoadHtml("<body style='margin:0'><div id='deco'>bos</div></body>");
            for (int i = 0; i < 4; i++)
            {
                yield return null;
            }

            _left.View.SetFocus("#l2");
            Assert.IsFalse(_left.Move(HtmlNavDirection.Right), "nowhere to go");
            Assert.AreEqual("l2", _left.View.FocusedId, "so focus stays put");
        }
    }
}
