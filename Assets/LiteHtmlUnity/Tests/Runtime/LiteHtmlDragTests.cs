using System.Collections;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.TestTools;
using UnityEngine.UI;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// Covers dragging, which on a phone is the only way to scroll at all:
    /// OnScroll fires for a wheel or a trackpad and never for a finger.
    /// </summary>
    public class LiteHtmlDragTests
    {
        const float RectWidth = 300f;
        const float RectHeight = 150f;

        GameObject _canvasGo;
        LiteHtmlView _view;
        LiteHtmlRawImage _raw;
        RectTransform _rect;

        [SetUp]
        public void SetUp()
        {
            _canvasGo = new GameObject("Canvas", typeof(Canvas));
            Canvas canvas = _canvasGo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.scaleFactor = 1f;

            var go = new GameObject("View", typeof(RectTransform), typeof(RawImage),
                                    typeof(LiteHtmlView), typeof(LiteHtmlRawImage));
            _rect = (RectTransform)go.transform;
            _rect.SetParent(_canvasGo.transform, false);
            _rect.anchorMin = _rect.anchorMax = new Vector2(0.5f, 0.5f);
            _rect.sizeDelta = new Vector2(RectWidth, RectHeight);
            _rect.anchoredPosition = Vector2.zero;

            _view = go.GetComponent<LiteHtmlView>();
            _raw = go.GetComponent<LiteHtmlRawImage>();

            string rows = "";
            for (int i = 0; i < 20; i++)
            {
                rows += $"<div id='r{i}' style='height:30px'>satir {i}</div>";
            }

            _view.LoadHtml($"<body style='margin:0'><div style='height:150px;overflow:auto'>{rows}</div></body>");
        }

        [TearDown]
        public void TearDown()
        {
            if (_canvasGo != null)
            {
                Object.DestroyImmediate(_canvasGo);
            }
        }

        static IEnumerator Settle()
        {
            for (int i = 0; i < 4; i++)
            {
                yield return null;
            }
        }

        /// <summary>Screen point at the middle of the view.</summary>
        static Vector2 Centre() => new Vector2(Screen.width * 0.5f, Screen.height * 0.5f);

        PointerEventData At(Vector2 screenPoint, Vector2 delta)
        {
            // pressEventCamera is read-only and derives from pressEventData's
            // camera raycast; leaving it null is what an overlay canvas reports
            // anyway, which is the case under test.
            return new PointerEventData(EventSystem.current)
            {
                position = screenPoint,
                delta = delta,
            };
        }

        float RowY(string id)
        {
            Assert.IsTrue(_view.TryGetElementRect(id, out Rect r), $"{id} should be measurable");
            return r.y;
        }

        [UnityTest]
        public IEnumerator DraggingScrollsTheList()
        {
            yield return Settle();
            float before = RowY("#r5");

            // Finger moves up the screen, which pulls the list up and reveals
            // what is below — the same direction a phone behaves.
            var drag = At(Centre(), new Vector2(0f, 40f));
            _raw.OnBeginDrag(drag);
            _raw.OnDrag(drag);
            yield return Settle();

            float after = RowY("#r5");
            Assert.Less(after, before, $"the row should have moved up ({before} -> {after})");
            Assert.AreEqual(40f, before - after, 6f, "and by roughly the distance dragged");
        }

        [UnityTest]
        public IEnumerator DraggingBackDownRestoresThePosition()
        {
            yield return Settle();
            float start = RowY("#r5");

            var up = At(Centre(), new Vector2(0f, 40f));
            _raw.OnBeginDrag(up);
            _raw.OnDrag(up);
            yield return Settle();

            var down = At(Centre(), new Vector2(0f, -40f));
            _raw.OnBeginDrag(down);
            _raw.OnDrag(down);
            yield return Settle();

            Assert.AreEqual(start, RowY("#r5"), 1f, "back where it started");
        }

        [UnityTest]
        public IEnumerator ClaimedDragDoesNotScrollAndReportsTheDrop()
        {
            yield return Settle();
            float before = RowY("#r5");

            string picked = null;
            string dropped = null;
            _raw.DragFilter = d => { picked = d.ElementId; return true; };
            _raw.ItemDropped += d => dropped = d.ElementId;

            var drag = At(Centre(), new Vector2(0f, 40f));
            _raw.OnBeginDrag(drag);
            _raw.OnDrag(drag);
            _raw.OnEndDrag(drag);
            yield return Settle();

            Assert.IsNotNull(picked, "the filter should have been offered the element under the finger");
            Assert.AreEqual(picked, dropped, "and the drop should report where it ended");
            Assert.AreEqual(before, RowY("#r5"), 0.5f, "a claimed drag must not scroll the page");
        }

        /// <summary>
        /// Assigning Resources before the view has built its document must not
        /// lose it. Awake runs before OnEnable, so this is the ordinary case, and
        /// getting it wrong shows up only as images laid out at zero size.
        /// </summary>
        [UnityTest]
        public IEnumerator ResourcesSetBeforeInitialiseSurvive()
        {
            var go = new GameObject("Pending", typeof(RectTransform));
            go.SetActive(false);
            LiteHtmlView view = go.AddComponent<LiteHtmlView>();

            var provider = new GameObject("Res").AddComponent<LiteHtmlResources>();
            view.Resources = provider;          // document does not exist yet
            Assert.AreSame(provider, view.Resources, "the view should hold on to it");

            go.SetActive(true);                 // OnEnable builds the document
            yield return Settle();

            Assert.AreSame(provider, view.Resources, "and hand it to the document once there is one");

            Object.DestroyImmediate(provider.gameObject);
            Object.DestroyImmediate(go);
        }
    }
}
