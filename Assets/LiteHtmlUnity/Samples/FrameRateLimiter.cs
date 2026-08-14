using UnityEngine;

namespace LiteHtmlUnity.Samples
{
    /// <summary>
    /// Caps the frame rate.
    /// </summary>
    /// <remarks>
    /// Kept as its own component rather than folded into the demo controller so
    /// it can be dropped into any scene.
    ///
    /// The cap only takes effect with VSync off — with VSync on, Unity ignores
    /// <see cref="Application.targetFrameRate"/> entirely and the display's
    /// refresh rate wins. That is why this component owns both settings.
    ///
    /// 61 rather than 60 is deliberate: a cap sitting exactly on the refresh
    /// rate makes the limiter and the display race each other, and a frame that
    /// misses its slot by a fraction of a millisecond waits for the whole next
    /// one. One frame of headroom removes that beat without letting the game run
    /// meaningfully faster.
    /// </remarks>
    [AddComponentMenu("LiteHtml/Samples/Frame Rate Limiter")]
    public class FrameRateLimiter : MonoBehaviour
    {
        [Tooltip("Turn the cap off to let the renderer run as fast as it can.")]
        [SerializeField] private bool _limit = true;

        [Tooltip("Frames per second to aim for when the cap is on.")]
        [SerializeField, Range(15, 240)] private int _targetFrameRate = 61;

        /// <summary>Whether the cap is currently applied.</summary>
        public bool IsLimited => _limit;

        public int TargetFrameRate => _targetFrameRate;

        /// <summary>The value Unity is actually running with (-1 means uncapped).</summary>
        public int AppliedFrameRate => Application.targetFrameRate;

        private void OnEnable()
        {
            Apply();
        }

        private void OnValidate()
        {
            if (isActiveAndEnabled)
            {
                Apply();
            }
        }

        private void OnDisable()
        {
            // Leave the project's own settings behind rather than a value this
            // component happened to be holding.
            Application.targetFrameRate = -1;
        }

        /// <summary>Turns the cap on or off at runtime.</summary>
        public void SetLimited(bool limited)
        {
            _limit = limited;
            Apply();
        }

        /// <summary>Changes the target and enables the cap.</summary>
        public void SetTargetFrameRate(int fps)
        {
            _targetFrameRate = Mathf.Clamp(fps, 15, 240);
            _limit = true;
            Apply();
        }

        private void Apply()
        {
            if (_limit)
            {
                // targetFrameRate is ignored while VSync is on.
                QualitySettings.vSyncCount = 0;
                Application.targetFrameRate = _targetFrameRate;
            }
            else
            {
                Application.targetFrameRate = -1;
            }
        }
    }
}
