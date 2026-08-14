using System;
using UnityEngine;

namespace Doctype.Samples
{
    /// <summary>
    /// The demo's icons, drawn in code so the samples need no art assets.
    /// </summary>
    /// <remarks>
    /// Textures created here are readable by construction, which is what packing
    /// into the image atlas needs — a texture imported from a file has to have
    /// Read/Write ticked before it can be packed.
    /// <para>
    /// The caller owns what it is handed and should Destroy it.
    /// </para>
    /// </remarks>
    public static class HtmlDemoIcons
    {
        /// <summary>
        /// Rasterises a shading function over a square. `p` runs -1..1 with the
        /// origin at the centre, so the shapes below are written as distances
        /// rather than as pixel arithmetic.
        /// </summary>
        public static Texture2D Icon(int size, Func<Vector2, Color> shade)
        {
            var tex = new Texture2D(size, size, TextureFormat.RGBA32, false);
            var pixels = new Color[size * size];

            for (int y = 0; y < size; y++)
            {
                for (int x = 0; x < size; x++)
                {
                    var p = new Vector2((x + 0.5f) / size * 2f - 1f, 1f - (y + 0.5f) / size * 2f);
                    pixels[(size - 1 - y) * size + x] = shade(p);
                }
            }

            tex.SetPixels(pixels);
            tex.Apply();
            tex.filterMode = FilterMode.Bilinear;
            return tex;
        }

        // Soft coverage across one pixel's worth of the -1..1 range, so edges are
        // antialiased instead of stepping.
        static float Cover(float distance, float feather = 0.04f) =>
            Mathf.SmoothStep(1f, 0f, distance / feather);

        public static Color Coin(Vector2 p)
        {
            float r = p.magnitude;
            float body = Cover(r - 0.92f);
            if (body <= 0f)
            {
                return Color.clear;
            }

            // Lighter towards the upper left, plus a darker rim.
            float lit = Mathf.Clamp01(0.55f + 0.45f * Vector2.Dot(p.normalized, new Vector2(-0.5f, 0.7f)));
            Color gold = Color.Lerp(new Color(0.55f, 0.36f, 0.05f), new Color(1f, 0.86f, 0.35f), lit);
            gold = Color.Lerp(gold, new Color(0.42f, 0.27f, 0.03f), Cover(0.74f - r) * 0.8f);
            gold.a = body;
            return gold;
        }

        public static Color Gem(Vector2 p)
        {
            // A diamond: the L1 distance instead of the L2 one.
            float d = Mathf.Abs(p.x) + Mathf.Abs(p.y);
            float body = Cover(d - 0.95f);
            if (body <= 0f)
            {
                return Color.clear;
            }

            float facet = p.y > 0.35f - Mathf.Abs(p.x) * 0.5f ? 1f : 0f;
            Color cyan = Color.Lerp(new Color(0.05f, 0.35f, 0.55f), new Color(0.45f, 0.92f, 1f), 0.35f + 0.65f * facet);
            cyan.a = body;
            return cyan;
        }

        public static Color Potion(Vector2 p)
        {
            // A corked flask: a round body unioned with a narrow neck, both as
            // signed distances so the union is just a min().
            float body = new Vector2(p.x, p.y + 0.25f).magnitude - 0.62f;
            float neck = Mathf.Max(Mathf.Abs(p.x) - 0.17f, Mathf.Abs(p.y - 0.55f) - 0.35f);
            float glass = Mathf.Min(body, neck);

            float cover = Cover(glass);
            if (cover <= 0f)
            {
                return Color.clear;
            }

            // Cork caps the neck, a touch wider so it reads as a stopper.
            float cork = Mathf.Max(Mathf.Abs(p.x) - 0.21f, Mathf.Abs(p.y - 0.88f) - 0.12f);
            if (cork < 0f)
            {
                return new Color(0.56f, 0.37f, 0.19f, cover);
            }

            // Liquid fills the body to a flat surface; glass above it.
            Color c;
            if (body < 0f && p.y < 0.05f)
            {
                c = Color.Lerp(new Color(0.82f, 0.10f, 0.48f), new Color(1f, 0.52f, 0.80f),
                               Mathf.Clamp01((p.y + 0.85f) / 0.9f));
            }
            else
            {
                c = new Color(0.78f, 0.88f, 0.98f, 0.42f);
            }

            c.a *= cover;
            return c;
        }

        /// <summary>Convenience wrappers so callers read as intent, not shading.</summary>
        public static Texture2D Coin(int size) => Icon(size, Coin);

        public static Texture2D Gem(int size) => Icon(size, Gem);

        public static Texture2D Potion(int size) => Icon(size, Potion);
    }
}
