using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace LiteHtmlUnity
{
    /// <summary>
    /// One typeface to register with a document.
    /// </summary>
    /// <remarks>
    /// Unity turns imported .ttf files into Font assets, whose raw bytes are not
    /// reachable at runtime. Ship fonts as .bytes (rename the file) and assign
    /// them to <see cref="Bytes"/>, or point <see cref="AbsolutePath"/> at a
    /// system font.
    /// </remarks>
    [Serializable]
    public class LiteHtmlFontEntry
    {
        [Tooltip("CSS family name to match, lowercase. Register the same file under several names to alias it.")]
        public string Family = "sans-serif";

        [Tooltip("CSS weight: 400 normal, 700 bold.")]
        public int Weight = 400;

        public bool Italic;

        [Tooltip("A .ttf/.otf renamed to .bytes so Unity imports it as a TextAsset.")]
        public TextAsset Bytes;

        [Tooltip("Fallback: absolute path to a font file on disk. Used when Bytes is empty.")]
        public string AbsolutePath;

        public byte[] Resolve()
        {
            if (Bytes != null && Bytes.bytes != null && Bytes.bytes.Length > 0)
            {
                return Bytes.bytes;
            }

            if (!string.IsNullOrEmpty(AbsolutePath) && File.Exists(AbsolutePath))
            {
                return File.ReadAllBytes(AbsolutePath);
            }

            return null;
        }
    }

    /// <summary>
    /// Locates fonts already present on the running platform.
    /// </summary>
    /// <remarks>
    /// Convenient for getting started and for tests, but shipping your own font
    /// files is strongly preferred: system fonts differ between OS versions and
    /// devices, so layout will not be reproducible without them.
    /// </remarks>
    public static class LiteHtmlSystemFonts
    {
        private static readonly string[] MacRegular =
        {
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "/Library/Fonts/Arial.ttf",
        };

        private static readonly string[] MacBold =
        {
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        };

        private static readonly string[] MacItalic =
        {
            "/System/Library/Fonts/Supplemental/Arial Italic.ttf",
        };

        private static readonly string[] MacMono =
        {
            "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Supplemental/Courier New.ttf",
        };

        private static readonly string[] AndroidRegular =
        {
            "/system/fonts/Roboto-Regular.ttf",
            "/system/fonts/DroidSans.ttf",
        };

        private static readonly string[] AndroidBold =
        {
            "/system/fonts/Roboto-Bold.ttf",
            "/system/fonts/DroidSans-Bold.ttf",
        };

        private static readonly string[] AndroidItalic =
        {
            "/system/fonts/Roboto-Italic.ttf",
        };

        private static readonly string[] AndroidMono =
        {
            "/system/fonts/RobotoMono-Regular.ttf",
            "/system/fonts/DroidSansMono.ttf",
        };

        private static string FirstExisting(string[] candidates)
        {
            foreach (string path in candidates)
            {
                if (File.Exists(path))
                {
                    return path;
                }
            }

            return null;
        }

        /// <summary>
        /// Builds entries covering the generic CSS families for this platform.
        /// Returns an empty list when nothing usable was found.
        /// </summary>
        public static List<LiteHtmlFontEntry> Discover()
        {
            var result = new List<LiteHtmlFontEntry>();

            string[] regular, bold, italic, mono;

#if UNITY_ANDROID && !UNITY_EDITOR
            regular = AndroidRegular; bold = AndroidBold; italic = AndroidItalic; mono = AndroidMono;
#elif UNITY_STANDALONE_OSX || UNITY_EDITOR_OSX
            regular = MacRegular; bold = MacBold; italic = MacItalic; mono = MacMono;
#else
            regular = Array.Empty<string>();
            bold = Array.Empty<string>();
            italic = Array.Empty<string>();
            mono = Array.Empty<string>();
#endif

            string regularPath = FirstExisting(regular);
            if (regularPath == null)
            {
                return result;
            }

            // Register the regular face under every generic family so unknown
            // font-family values still resolve to something readable.
            foreach (string family in new[] { "sans-serif", "serif", "system-ui", "arial", "helvetica" })
            {
                result.Add(new LiteHtmlFontEntry { Family = family, Weight = 400, AbsolutePath = regularPath });
            }

            string boldPath = FirstExisting(bold);
            if (boldPath != null)
            {
                foreach (string family in new[] { "sans-serif", "serif", "system-ui", "arial", "helvetica" })
                {
                    result.Add(new LiteHtmlFontEntry { Family = family, Weight = 700, AbsolutePath = boldPath });
                }
            }

            string italicPath = FirstExisting(italic);
            if (italicPath != null)
            {
                result.Add(new LiteHtmlFontEntry
                {
                    Family = "sans-serif", Weight = 400, Italic = true, AbsolutePath = italicPath,
                });
            }

            string monoPath = FirstExisting(mono);
            if (monoPath != null)
            {
                foreach (string family in new[] { "monospace", "courier", "menlo" })
                {
                    result.Add(new LiteHtmlFontEntry { Family = family, Weight = 400, AbsolutePath = monoPath });
                }
            }

            return result;
        }
    }
}
