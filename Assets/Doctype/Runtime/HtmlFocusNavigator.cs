using UnityEngine;
using UnityEngine.EventSystems;

namespace Doctype
{
    /// <summary>
    /// Drives a document's gamepad/keyboard focus from Unity's EventSystem,
    /// and hands focus across to a neighbouring panel when it walks off an
    /// edge.
    /// </summary>
    /// <remarks>
    /// Attach next to a <see cref="HtmlView"/> and make this object the
    /// EventSystem's selected GameObject (for example with
    /// <c>EventSystem.current.SetSelectedGameObject(gameObject)</c> when the
    /// HUD opens). The standard input modules then deliver Move/Submit/Cancel
    /// here from whatever the project's actions map them to — arrow keys,
    /// WASD, a D-pad, a left stick — without this package referencing any
    /// input backend.
    /// <para>
    /// A HUD built the Doctype way is several independent panels, and a D-pad
    /// does not care where one surface ends: wire each edge's neighbour in
    /// the Inspector (or through <see cref="Up"/>/<see cref="Right"/>/
    /// <see cref="Down"/>/<see cref="Left"/>) and a move past the edge hands
    /// focus to that panel instead of stopping. The handoff is a transaction:
    /// the giving panel only lets go once the receiving panel has actually
    /// focused something, and each panel remembers its last focused element
    /// so returning lands where the user left, not at the top.
    /// </para>
    /// <para>
    /// Everything is also callable directly (<see cref="Move"/>,
    /// <see cref="Activate"/>) for games that route input themselves. Focus
    /// navigation only visits elements carrying a <c>tabindex</c> attribute;
    /// style them with <c>:focus</c> and override individual hops with
    /// <c>data-nav-up/right/down/left="targetId"</c>.
    /// </para>
    /// </remarks>
    [AddComponentMenu("Doctype/Focus Navigator")]
    [RequireComponent(typeof(HtmlView))]
    public sealed class HtmlFocusNavigator : MonoBehaviour, IMoveHandler, ISubmitHandler, ICancelHandler
    {
        [Tooltip("Focus the top-left focusable element as soon as the document has one.")]
        [SerializeField] private bool _focusFirstOnEnable;

        [Tooltip("Panel that receives focus when a move walks off this panel's top edge.")]
        [SerializeField] private HtmlFocusNavigator _up;

        [Tooltip("Panel that receives focus when a move walks off this panel's right edge.")]
        [SerializeField] private HtmlFocusNavigator _right;

        [Tooltip("Panel that receives focus when a move walks off this panel's bottom edge.")]
        [SerializeField] private HtmlFocusNavigator _down;

        [Tooltip("Panel that receives focus when a move walks off this panel's left edge.")]
        [SerializeField] private HtmlFocusNavigator _left;

        private HtmlView _view;
        private bool _wantsFirstFocus;

        // The element the user was on when focus left this panel, restored
        // when it comes back. An id, because elements do not survive reloads
        // and an id does.
        private string _rememberedFocus;

        /// <summary>Raised when Cancel (B / Esc) fires while this object is selected.</summary>
        public event System.Action Cancelled;

        public HtmlView View => _view != null ? _view : _view = GetComponent<HtmlView>();

        public HtmlFocusNavigator Up { get => _up; set => _up = value; }
        public HtmlFocusNavigator Right { get => _right; set => _right = value; }
        public HtmlFocusNavigator Down { get => _down; set => _down = value; }
        public HtmlFocusNavigator Left { get => _left; set => _left = value; }

        private void OnEnable()
        {
            _wantsFirstFocus = _focusFirstOnEnable;
        }

        private void Update()
        {
            // The first focus has to wait for a layout: candidates need boxes.
            if (_wantsFirstFocus && View.MoveFocus(HtmlNavDirection.Down))
            {
                _wantsFirstFocus = false;
                _rememberedFocus = View.FocusedId;
            }
        }

        /// <summary>
        /// Moves focus one step, handing it to the wired neighbour when this
        /// panel's edge is reached. False when there was nowhere to go at all
        /// (no move inside, and no neighbour that could take it).
        /// </summary>
        public bool Move(HtmlNavDirection direction)
        {
            if (View.MoveFocus(direction))
            {
                _rememberedFocus = View.FocusedId;
                return true;
            }

            HtmlFocusNavigator neighbour = NeighbourFor(direction);
            return neighbour != null && neighbour.ReceiveFocus(this, direction);
        }

        /// <summary>Activates the focused element, exactly like a click on it.</summary>
        public bool Activate() => View.Activate();

        private HtmlFocusNavigator NeighbourFor(HtmlNavDirection direction)
        {
            switch (direction)
            {
                case HtmlNavDirection.Up: return _up;
                case HtmlNavDirection.Right: return _right;
                case HtmlNavDirection.Down: return _down;
                default: return _left;
            }
        }

        /// <summary>
        /// Takes focus from another panel. A transaction: the giver keeps its
        /// focus unless something here really took it, so a neighbour with
        /// nothing focusable behaves exactly like no neighbour at all.
        /// </summary>
        private bool ReceiveFocus(HtmlFocusNavigator from, HtmlNavDirection direction)
        {
            bool focused = false;

            // Land where the user left this panel, if that element still
            // exists; otherwise enter fresh (reading order, top-left first).
            if (!string.IsNullOrEmpty(_rememberedFocus))
            {
                View.SetFocus("#" + _rememberedFocus);
                focused = View.FocusedId == _rememberedFocus;
            }

            if (!focused)
            {
                focused = View.MoveFocus(direction);
            }

            if (!focused)
            {
                return false;
            }

            _rememberedFocus = View.FocusedId;

            from._rememberedFocus = from.View.FocusedId;
            from.View.SetFocus(null);

            // Subsequent Move/Submit events must route here, not to the panel
            // the user just left.
            if (EventSystem.current != null)
            {
                EventSystem.current.SetSelectedGameObject(gameObject);
            }

            return true;
        }

        void IMoveHandler.OnMove(AxisEventData eventData)
        {
            switch (eventData.moveDir)
            {
                case MoveDirection.Up: Move(HtmlNavDirection.Up); break;
                case MoveDirection.Right: Move(HtmlNavDirection.Right); break;
                case MoveDirection.Down: Move(HtmlNavDirection.Down); break;
                case MoveDirection.Left: Move(HtmlNavDirection.Left); break;
            }
        }

        void ISubmitHandler.OnSubmit(BaseEventData eventData)
        {
            Activate();
        }

        void ICancelHandler.OnCancel(BaseEventData eventData)
        {
            Cancelled?.Invoke();
        }
    }
}
