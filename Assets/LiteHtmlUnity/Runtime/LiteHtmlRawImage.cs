using System;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;

namespace LiteHtmlUnity
{
    /// <summary>
    /// Displays a <see cref="LiteHtmlView"/> on a uGUI RawImage and forwards
    /// pointer input into the document.
    /// </summary>
    /// <remarks>
    /// Kept separate from LiteHtmlView so the view itself stays usable without
    /// uGUI — on a world-space quad, as a material input, or headless in tests.
    /// </remarks>
    /// <summary>What a drag is touching, in the document's own terms.</summary>
    public readonly struct LiteHtmlDrag
    {
        public LiteHtmlDrag(string elementId, Vector2 documentPoint, Vector2 screenPosition)
        {
            ElementId = elementId;
            DocumentPoint = documentPoint;
            ScreenPosition = screenPosition;
        }

        /// <summary>Id of the element under the pointer, or null over bare page.</summary>
        public string ElementId { get; }

        /// <summary>Pointer position in CSS pixels.</summary>
        public Vector2 DocumentPoint { get; }

        /// <summary>
        /// Pointer position in screen pixels, for placing something that has to
        /// follow the finger outside the page — a dragged icon cannot leave the
        /// surface the document is clipped to, so it has to be its own object.
        /// </summary>
        public Vector2 ScreenPosition { get; }
    }

    [RequireComponent(typeof(RawImage))]
    [AddComponentMenu("LiteHtml/LiteHtml Raw Image")]
    public class LiteHtmlRawImage : MonoBehaviour,
        IPointerMoveHandler, IPointerDownHandler, IPointerUpHandler, IPointerExitHandler, IScrollHandler,
        IBeginDragHandler, IDragHandler, IEndDragHandler
    {
        [SerializeField] private LiteHtmlView _view;

        [Tooltip("Resize the document to match the RawImage's rect as it changes.")]
        [SerializeField] private bool _matchRectSize = true;

        [Tooltip("Render at the canvas scale factor so text stays sharp, and treat one CSS pixel " +
                 "as one canvas unit. Turn off only to drive DeviceScale yourself.")]
        [SerializeField] private bool _matchCanvasScale = true;

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
                _view = GetComponent<LiteHtmlView>() ?? GetComponentInChildren<LiteHtmlView>();
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
                Shader shader = Shader.Find("LiteHtml/Composite");
                if (shader == null)
                {
                    // Degrade to the stock material rather than to a pink one.
                    Debug.LogWarning("[LiteHtml] LiteHtml/Composite not found; transparent content will " +
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
                float scale = ResolveScale();
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
        public Func<LiteHtmlDrag, bool> DragFilter { get; set; }

        /// <summary>Pointer moved during a claimed drag.</summary>
        public event Action<LiteHtmlDrag> ItemDragged;

        /// <summary>Claimed drag finished; the id is whatever it was dropped on.</summary>
        public event Action<LiteHtmlDrag> ItemDropped;

        private bool _dragClaimed;

        private LiteHtmlDrag Probe(PointerEventData eventData)
        {
            // ElementAt is a pure query, so probing a drop target mid-drag does
            // not light it up as hovered.
            return TryGetDocumentPoint(eventData, out Vector2 p)
                ? new LiteHtmlDrag(_view != null ? _view.ElementAt(p) : null, p, eventData.position)
                : default;
        }

        public void OnBeginDrag(PointerEventData eventData)
        {
            _dragClaimed = false;

            if (_view == null)
            {
                return;
            }

            LiteHtmlDrag drag = Probe(eventData);
            _dragClaimed = DragFilter != null && DragFilter(drag);
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
