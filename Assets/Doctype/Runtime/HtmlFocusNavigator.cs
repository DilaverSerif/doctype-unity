using UnityEngine;
using UnityEngine.EventSystems;

namespace Doctype
{
    /// <summary>
    /// Drives a document's gamepad/keyboard focus from Unity's EventSystem.
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

        private HtmlView _view;
        private bool _wantsFirstFocus;

        /// <summary>Raised when Cancel (B / Esc) fires while this object is selected.</summary>
        public event System.Action Cancelled;

        public HtmlView View => _view != null ? _view : _view = GetComponent<HtmlView>();

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
            }
        }

        /// <summary>Moves focus one step; false when it had nowhere to go.</summary>
        public bool Move(HtmlNavDirection direction) => View.MoveFocus(direction);

        /// <summary>Activates the focused element, exactly like a click on it.</summary>
        public bool Activate() => View.Activate();

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
