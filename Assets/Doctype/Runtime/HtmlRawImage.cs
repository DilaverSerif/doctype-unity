using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;

namespace Doctype
{
    /// <summary>
    /// Displays a <see cref="HtmlView"/> on a uGUI RawImage and forwards
    /// pointer input into the document.
    /// </summary>
    /// <remarks>
    /// Kept separate from HtmlView so the view itself stays usable without
    /// uGUI — on a world-space quad, as a material input, or headless in tests.
    /// </remarks>
    /// <summary>What a drag is touching, in the document's own terms.</summary>
    public readonly struct HtmlDrag
    {
        public HtmlDrag(HtmlView view, string elementId, Vector2 documentPoint, Vector2 screenPosition)
        {
            View = view;
            ElementId = elementId;
            DocumentPoint = documentPoint;
            ScreenPosition = screenPosition;
        }

        /// <summary>
        /// The document the pointer is over, which is not always the one the
        /// drag started in — an item can be dragged from one surface onto
        /// another. Null when the pointer is over no page at all.
        /// </summary>
        public HtmlView View { get; }

        /// <summary>Id of the element under the pointer, or null over bare page.</summary>
        public string ElementId { get; }

        /// <summary>Pointer position in CSS pixels, in <see cref="View"/>'s document.</summary>
        public Vector2 DocumentPoint { get; }

        /// <summary>
        /// Pointer position in screen pixels, for placing something that has to
        /// follow the finger outside the page — a dragged icon cannot leave the
        /// surface the document is clipped to, so it has to be its own object.
        /// </summary>
        public Vector2 ScreenPosition { get; }
    }

    [RequireComponent(typeof(RawImage))]
    [AddComponentMenu("Doctype/Raw Image")]
    public class HtmlRawImage : MonoBehaviour, ICanvasRaycastFilter,
        IPointerMoveHandler, IPointerDownHandler, IPointerUpHandler, IPointerExitHandler, IScrollHandler,
        IBeginDragHandler, IDragHandler, IEndDragHandler
    {
        [SerializeField] private HtmlView _view;

        [Tooltip("Let a touch that lands on bare page fall through to whatever is behind. " +
                 "Only elements carrying an id catch input, so a HUD stretched over the whole " +
                 "screen does not swallow the game everywhere it happens to be transparent.")]
        [SerializeField] private bool _passThroughEmptyAreas;

        [Tooltip("Resize the document to match the RawImage's rect as it changes.")]
        [SerializeField] private bool _matchRectSize = true;

        [Tooltip("Render at the canvas scale factor so text stays sharp, and treat one CSS pixel " +
                 "as one canvas unit. Turn off only to drive DeviceScale yourself.")]
        [SerializeField] private bool _matchCanvasScale = true;

        [Tooltip("Resolution multiplier, like a game's render scale. Below 1 the page is drawn into " +
                 "fewer pixels and stretched back up; the layout is unchanged.")]
        [SerializeField, Range(0.25f, 2f)] private float _renderScale = 1f;

        [Tooltip("Composites the surface. Leave empty for the built-in premultiplied-alpha " +
                 "material, which is what a see-through page needs.")]
        [SerializeField] private Material _compositeMaterial;

        // One material for every view: uGUI clones it per masking context anyway,
        // and a per-instance copy would leak one material per view.
        private static Material s_composite;

        private RawImage _image;
        private RectTransform _rect;
        private Canvas _canvas;
        private Vector2 _lastPixelSize;
        private float _lastScale;

        private void Awake()
        {
            _image = GetComponent<RawImage>();
            _rect = (RectTransform)transform;

            if (_view == null)
            {
                _view = GetComponent<HtmlView>() ?? GetComponentInChildren<HtmlView>();
            }

            ApplyCompositeMaterial();
        }

        /// <summary>
        /// Puts the surface on a premultiplied-alpha material.
        /// </summary>
        /// <remarks>
        /// The quad shader blends RGB with SrcAlpha, so the surface holds colour
        /// already scaled by its own alpha. uGUI's default material scales by
        /// alpha again, which shows a half-transparent page at a quarter of its
        /// opacity — invisible while everything is opaque, obvious the moment a
        /// menu is made see-through.
        /// <para>
        /// A material assigned in the inspector is left alone, so this can be
        /// overridden without editing code.
        /// </para>
        /// </remarks>
        private void ApplyCompositeMaterial()
        {
            if (_compositeMaterial != null)
            {
                _image.material = _compositeMaterial;
                return;
            }

            // Only replace the stock material; anything deliberate stays.
            Material current = _image.material;
            if (current != null && current.shader != null && current.shader.name != "UI/Default")
            {
                return;
            }

            if (s_composite == null)
            {
                Shader shader = Shader.Find("Doctype/Composite");
                if (shader == null)
                {
                    // Degrade to the stock material rather than to a pink one.
                    Debug.LogWarning("[Doctype] Doctype/Composite not found; transparent content will " +
                                     "composite too dark. Add the shader to Always Included Shaders for " +
                                     "player builds.", this);
                    return;
                }

                s_composite = new Material(shader) { hideFlags = HideFlags.HideAndDontSave };
            }

            _image.material = s_composite;
        }

        private void LateUpdate()
        {
            if (_view == null)
            {
                return;
            }

            if (_matchRectSize)
            {
                // rect.size is in canvas units, not screen pixels. Under a
                // CanvasScaler set to scale with screen size those are not the
                // same thing, and sizing the surface in canvas units renders the
                // page below the display's real resolution and then stretches it,
                // which shows up as soft text. Multiplying by the scale factor
                // gives the true pixel count; feeding the same factor to
                // DeviceScale keeps the CSS viewport equal to the rect in canvas
                // units, so a layout is authored once and stays put at any
                // resolution.
                // RenderScale multiplies into both, which is what keeps the CSS
                // viewport fixed: pixels and DeviceScale move together, so their
                // quotient -- the layout -- does not move at all.
                float scale = ResolveScale() * _renderScale;
                Vector2 pixels = _rect.rect.size * scale;

                if ((pixels != _lastPixelSize || !Mathf.Approximately(scale, _lastScale)) &&
                    pixels.x >= 1f && pixels.y >= 1f)
                {
                    _lastPixelSize = pixels;
                    _lastScale = scale;

                    // Scale first: SetSize re-lays-out, and doing it in the other
                    // order lays the page out once against a stale viewport.
                    if (_matchCanvasScale)
                    {
                        _view.DeviceScale = scale;
                    }

                    _view.SetSize(Mathf.RoundToInt(pixels.x), Mathf.RoundToInt(pixels.y));
                }
            }

            // The view recreates its target whenever the size changes, so the
            // reference has to be refreshed rather than cached once.
            if (_image.texture != _view.Texture)
            {
                _image.texture = _view.Texture;
            }
        }

        // The owning canvas is not known at Awake: a prefab is normally
        // instantiated first and parented afterwards, and it can be re-parented
        // later. Drop the cache whenever either can have happened.
        private void OnTransformParentChanged() => _canvas = null;

        private void OnCanvasHierarchyChanged() => _canvas = null;

        /// <summary>
        /// Canvas units to screen pixels. One when the canvas does no scaling, or
        /// when the caller has taken DeviceScale over.
        /// </summary>
        private float ResolveScale()
        {
            if (!_matchCanvasScale)
            {
                return 1f;
            }

            if (_canvas == null)
            {
                _canvas = GetComponentInParent<Canvas>();
            }

            // A nested canvas carries the root's factor, but only once Unity has
            // run its layout; guard against the zero it reports before that.
            float scale = _canvas != null ? _canvas.scaleFactor : 1f;
            return scale > 0f ? scale : 1f;
        }

        /// <summary>
        /// Converts a screen-space pointer position into document coordinates.
        /// </summary>
        private bool TryGetDocumentPoint(PointerEventData eventData, out Vector2 documentPoint)
        {
            return TryGetDocumentPoint(eventData.position, eventData.pressEventCamera, out documentPoint);
        }

        private bool TryGetDocumentPoint(Vector2 screenPoint, Camera camera, out Vector2 documentPoint)
        {
            documentPoint = default;

            if (_view == null ||
                !RectTransformUtility.ScreenPointToLocalPointInRectangle(
                    _rect, screenPoint, camera, out Vector2 local))
            {
                return false;
            }

            Rect r = _rect.rect;
            var normalized = new Vector2((local.x - r.xMin) / r.width, (local.y - r.yMin) / r.height);

            documentPoint = _view.NormalizedToDocument(normalized);
            return true;
        }

        /// <summary>
        /// Resolution multiplier for the surface, like a game's render scale.
        /// </summary>
        /// <remarks>
        /// Scales the pixels and DeviceScale together, so the CSS viewport — and
        /// therefore the layout, and the quad stream — comes out identical and
        /// only the resolution changes. Below 1 the page is drawn into fewer
        /// pixels and stretched back up: softer text, proportionally less fill.
        /// <para>
        /// Its other use is measurement. Quad count and fill move together in
        /// any normal page, so a slower frame cannot be blamed on either one;
        /// holding the quads fixed and cutting the pixels is what separates them.
        /// </para>
        /// </remarks>
        public float RenderScale
        {
            get => _renderScale;
            set
            {
                value = Mathf.Clamp(value, 0.25f, 2f);

                if (!Mathf.Approximately(_renderScale, value))
                {
                    _renderScale = value;

                    // The cached pixel size was derived under the old factor.
                    _lastPixelSize = Vector2.zero;
                }
            }
        }

        /// <summary>
        /// Whether touches landing on bare page fall through to what is behind.
        /// </summary>
        public bool PassThroughEmptyAreas
        {
            get => _passThroughEmptyAreas;
            set => _passThroughEmptyAreas = value;
        }

        /// <summary>
        /// Decides whether a raycast stops here, so a page that is mostly holes
        /// stops acting like a sheet of glass over the game.
        /// </summary>
        /// <remarks>
        /// The rule is "an element with an id catches the pointer, bare page does
        /// not". That is coarser than asking whether the pixel under the finger
        /// was painted, but it is the rule the page author already works in: the
        /// parts a game reacts to are the parts it named, and a decorative
        /// wrapper left unnamed stays out of the way. The alternative — reading
        /// back the surface's alpha — costs a GPU round trip per pointer move.
        /// <para>
        /// Only consulted while <see cref="PassThroughEmptyAreas"/> is on; the
        /// default is off, because a page that fills its rect wants every touch.
        /// </para>
        /// </remarks>
        public bool IsRaycastLocationValid(Vector2 screenPoint, Camera eventCamera)
        {
            if (!_passThroughEmptyAreas || _view == null || _rect == null)
            {
                return true;
            }

            // Off the rect entirely: uGUI has already decided, and a miss here
            // would only be reported as a miss again.
            if (!TryGetDocumentPoint(screenPoint, eventCamera, out Vector2 p))
            {
                return false;
            }

            return !string.IsNullOrEmpty(_view.ElementAt(p));
        }

        public void OnPointerMove(PointerEventData eventData)
        {
            if (TryGetDocumentPoint(eventData, out Vector2 p))
            {
                _view.PointerMove(p);
            }
        }

        public void OnPointerDown(PointerEventData eventData)
        {
            if (TryGetDocumentPoint(eventData, out Vector2 p))
            {
                _view.PointerDown(p);
            }
        }

        public void OnPointerUp(PointerEventData eventData)
        {
            if (TryGetDocumentPoint(eventData, out Vector2 p))
            {
                _view.PointerUp(p);
            }
        }

        public void OnPointerExit(PointerEventData eventData)
        {
            _view?.PointerExit();
        }

        public void OnScroll(PointerEventData eventData)
        {
            if (TryGetDocumentPoint(eventData, out Vector2 p))
            {
                // Scroll deltas are reported in "lines"; CSS wants pixels, and
                // the y axis points the other way in document space.
                _view.Scroll(new Vector2(eventData.scrollDelta.x * -20f, eventData.scrollDelta.y * -20f), p);
            }
        }

        // --- dragging ----------------------------------------------------------
        //
        // OnScroll only ever fires for a wheel or a trackpad, so without these a
        // page cannot be scrolled at all on a phone — which is the platform this
        // engine targets. Dragging also gives a game somewhere to hook picking an
        // item up: DragFilter claims the gesture before it becomes a scroll.

        /// <summary>
        /// Called once when a drag starts. Return true to claim it — the page
        /// then stops scrolling and <see cref="ItemDragged"/> and
        /// <see cref="ItemDropped"/> report the rest of the gesture. Return false
        /// (or leave it unset) and the drag scrolls the page.
        /// </summary>
        public Func<HtmlDrag, bool> DragFilter { get; set; }

        /// <summary>Pointer moved during a claimed drag.</summary>
        public event Action<HtmlDrag> ItemDragged;

        /// <summary>Claimed drag finished; the id is whatever it was dropped on.</summary>
        public event Action<HtmlDrag> ItemDropped;

        private bool _dragClaimed;

        // Reused across every probe: one drag issues one of these per frame, and
        // a fresh List per frame is a fresh allocation per frame.
        private static readonly List<RaycastResult> s_hits = new List<RaycastResult>();

        /// <summary>
        /// What the pointer is over, across every Doctype surface on screen.
        /// </summary>
        /// <remarks>
        /// Not just this one. uGUI sends an entire drag to the object it began
        /// on, so an inventory drawn on its own surface would never hear about
        /// the hotbar drawn on another one — the drop would report nothing and
        /// the item would spring back. Answering with whatever page is actually
        /// under the finger is what lets a HUD be several panels instead of one
        /// screen-sized sheet.
        /// <para>
        /// The search runs through EventSystem rather than over a list of live
        /// surfaces, which buys the whole of uGUI's ordering for free: canvas
        /// sorting order, hierarchy order, and <see cref="IsRaycastLocationValid"/>,
        /// so a surface that is see-through at this point is already excluded.
        /// ElementAt itself is a pure query, so probing a drop target mid-drag
        /// does not light it up as hovered.
        /// </para>
        /// </remarks>
        private HtmlDrag Probe(PointerEventData eventData)
        {
            if (EventSystem.current != null)
            {
                s_hits.Clear();
                EventSystem.current.RaycastAll(eventData, s_hits);

                // Front to back: the topmost page that names something wins.
                for (int i = 0; i < s_hits.Count; i++)
                {
                    var surface = s_hits[i].gameObject.GetComponent<HtmlRawImage>();
                    if (surface == null || surface._view == null)
                    {
                        continue;
                    }

                    if (!surface.TryGetDocumentPoint(eventData.position, eventData.pressEventCamera,
                                                     out Vector2 hit))
                    {
                        continue;
                    }

                    string id = surface._view.ElementAt(hit);
                    if (!string.IsNullOrEmpty(id))
                    {
                        return new HtmlDrag(surface._view, id, hit, eventData.position);
                    }
                }
            }

            // This surface, as before. Either the raycast found no page that
            // names anything under the finger, or there is no EventSystem at all
            // — a bare canvas, or a test — and the cross-surface search is the
            // addition, not the replacement.
            return TryGetDocumentPoint(eventData, out Vector2 p)
                ? new HtmlDrag(_view, _view != null ? _view.ElementAt(p) : null, p, eventData.position)
                : default;
        }

        public void OnBeginDrag(PointerEventData eventData)
        {
            _dragClaimed = false;

            // No filter means nothing can claim the gesture, and Probe costs a
            // full raycast — so a page that only ever scrolls does not pay for
            // the cross-surface search it will never read.
            if (_view == null || DragFilter == null)
            {
                return;
            }

            _dragClaimed = DragFilter(Probe(eventData));
        }

        public void OnDrag(PointerEventData eventData)
        {
            if (_view == null)
            {
                return;
            }

            if (_dragClaimed)
            {
                ItemDragged?.Invoke(Probe(eventData));
                return;
            }

            if (!TryGetDocumentPoint(eventData, out Vector2 now))
            {
                return;
            }

            // Take the delta through the same screen-to-document conversion as
            // the position, so canvas scale and any rotation are handled once
            // rather than approximated twice.
            Vector2 previousScreen = eventData.position - eventData.delta;
            if (!TryGetDocumentPoint(previousScreen, eventData.pressEventCamera, out Vector2 before))
            {
                return;
            }

            Vector2 moved = now - before;

            // Dragging the content down reveals what is above it, which is a
            // negative scroll. Verified against the engine: a positive dy moves
            // content up.
            if (_view.Scroll(new Vector2(-moved.x, -moved.y), now))
            {
                return;
            }

            // Nothing in the page could take it — a list already at its end, or a
            // page that does not scroll at all. Hand the gesture to whatever
            // contains us so an outer ScrollRect still works.
            if (transform.parent != null)
            {
                ExecuteEvents.ExecuteHierarchy(transform.parent.gameObject, eventData, ExecuteEvents.dragHandler);
            }
        }

        public void OnEndDrag(PointerEventData eventData)
        {
            if (_dragClaimed)
            {
                ItemDropped?.Invoke(Probe(eventData));
            }

            _dragClaimed = false;
        }
    }
}
