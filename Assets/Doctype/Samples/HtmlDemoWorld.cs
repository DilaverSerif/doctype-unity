using System;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;

namespace Doctype.Samples
{
    /// <summary>
    /// A stand-in for the game the HUD is drawn over: a full-screen backdrop
    /// that answers taps.
    /// </summary>
    /// <remarks>
    /// It exists to make one claim checkable. A HUD whose middle is transparent
    /// still covers the screen, and the difference between "the gap is empty"
    /// and "the gap is dark" is invisible until something behind it reacts. Tap
    /// the band between the bag and the hotbar and a ring lands where the finger
    /// was; tap a panel and nothing happens, because the page kept that touch.
    /// </remarks>
    [AddComponentMenu("Doctype/Samples/Demo World")]
    public class HtmlDemoWorld : MonoBehaviour, IPointerClickHandler
    {
        const float RingSeconds = 0.7f;
        const int Rings = 8;          // enough that a fast tapper never runs out

        /// <summary>Raised on every tap that reached the world, with its screen point.</summary>
        public event Action<Vector2> Tapped;

        readonly RawImage[] _rings = new RawImage[Rings];
        readonly float[] _born = new float[Rings];

        int _next;

        /// <summary>
        /// Builds the backdrop as the first child of <paramref name="canvas"/>,
        /// so everything else in the canvas draws over it.
        /// </summary>
        public static HtmlDemoWorld Create(Canvas canvas)
        {
            var go = new GameObject("World", typeof(RectTransform), typeof(RawImage), typeof(HtmlDemoWorld));
            go.transform.SetParent(canvas.transform, false);
            go.transform.SetAsFirstSibling();

            Stretch((RectTransform)go.transform);

            var image = go.GetComponent<RawImage>();
            image.texture = Ground(256);
            image.color = Color.white;

            var world = go.GetComponent<HtmlDemoWorld>();
            world.BuildRings();
            return world;
        }

        static void Stretch(RectTransform rect)
        {
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = Vector2.zero;
            rect.offsetMax = Vector2.zero;
        }

        void BuildRings()
        {
            Texture2D ring = Ring(128);

            for (int i = 0; i < Rings; i++)
            {
                var go = new GameObject("Ring", typeof(RectTransform), typeof(RawImage));
                go.transform.SetParent(transform, false);

                ((RectTransform)go.transform).sizeDelta = new Vector2(180f, 180f);

                RawImage image = go.GetComponent<RawImage>();
                image.texture = ring;
                image.raycastTarget = false;
                image.color = Color.clear;

                _rings[i] = image;
                _born[i] = -RingSeconds;
            }
        }

        public void OnPointerClick(PointerEventData eventData)
        {
            var rect = (RectTransform)transform;
            if (!RectTransformUtility.ScreenPointToLocalPointInRectangle(
                    rect, eventData.position, eventData.pressEventCamera, out Vector2 local))
            {
                return;
            }

            RawImage ring = _rings[_next];
            ((RectTransform)ring.transform).anchoredPosition = local;

            _born[_next] = Time.time;
            _next = (_next + 1) % Rings;

            Tapped?.Invoke(eventData.position);
        }

        void Update()
        {
            for (int i = 0; i < Rings; i++)
            {
                float age = Time.time - _born[i];
                if (age >= RingSeconds)
                {
                    if (_rings[i].color.a != 0f)
                    {
                        _rings[i].color = Color.clear;
                    }
                    continue;
                }

                float t = age / RingSeconds;
                _rings[i].color = new Color(0.72f, 0.92f, 1f, 1f - t);
                ((RectTransform)_rings[i].transform).sizeDelta = Vector2.one * Mathf.Lerp(90f, 260f, t);
            }
        }

        // --- textures ----------------------------------------------------------

        /// <summary>
        /// Something that reads as terrain rather than as UI: a sky fading to a
        /// horizon, and a ground grid that converges towards it.
        /// </summary>
        static Texture2D Ground(int size)
        {
            var tex = new Texture2D(size, size, TextureFormat.RGBA32, false) { wrapMode = TextureWrapMode.Clamp };
            var px = new Color32[size * size];

            const float Horizon = 0.42f;

            for (int y = 0; y < size; y++)
            {
                // Texture space is bottom-up; the sky belongs at the top.
                float v = 1f - (y + 0.5f) / size;

                for (int x = 0; x < size; x++)
                {
                    float u = (x + 0.5f) / size;
                    Color c;

                    if (v < Horizon)
                    {
                        float t = v / Horizon;
                        c = Color.Lerp(new Color(0.20f, 0.28f, 0.46f), new Color(0.42f, 0.52f, 0.72f), t);
                    }
                    else
                    {
                        // Depth: rows near the horizon are hazier and their grid
                        // lines are packed closer together.
                        float depth = (v - Horizon) / (1f - Horizon);
                        c = Color.Lerp(new Color(0.30f, 0.40f, 0.34f), new Color(0.16f, 0.26f, 0.20f), depth);

                        float spacing = Mathf.Lerp(0.012f, 0.16f, depth);
                        float across = Mathf.Repeat((u - 0.5f) / Mathf.Max(depth, 0.05f) + 0.5f, 0.125f);
                        float along = Mathf.Repeat(v, spacing);

                        if (across < 0.006f || along < spacing * 0.06f)
                        {
                            c = Color.Lerp(c, new Color(0.55f, 0.68f, 0.55f), 0.5f);
                        }
                    }

                    px[y * size + x] = c;
                }
            }

            tex.SetPixels32(px);
            tex.Apply(false, true);
            return tex;
        }

        /// <summary>A soft ring, drawn where a tap landed.</summary>
        static Texture2D Ring(int size)
        {
            var tex = new Texture2D(size, size, TextureFormat.RGBA32, false);
            var px = new Color32[size * size];

            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    var p = new Vector2((x + 0.5f) / size - 0.5f, (y + 0.5f) / size - 0.5f);

                    // Distance from the ring's line, not from its centre.
                    float d = Mathf.Abs(p.magnitude - 0.38f);
                    float a = Mathf.Clamp01(1f - d / 0.06f);

                    px[y * size + x] = new Color(1f, 1f, 1f, a * a);
                }
            }

            tex.SetPixels32(px);
            tex.Apply(false, true);
            return tex;
        }
    }
}
