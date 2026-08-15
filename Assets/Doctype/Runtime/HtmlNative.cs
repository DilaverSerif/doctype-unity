using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Doctype
{
    /// <summary>
    /// Raw P/Invoke surface for the native litehtml wrapper.
    /// </summary>
    /// <remarks>
    /// Prefer <see cref="HtmlDocument"/> over calling these directly — it
    /// owns the handle lifetime and keeps the callback delegates alive.
    /// </remarks>
    public static class HtmlNative
    {
#if UNITY_IOS && !UNITY_EDITOR
        // iOS links the plugin statically into the player binary.
        internal const string Lib = "__Internal";
#else
        // macOS: Plugins/macOS/Doctype.bundle
        // Android: Plugins/Android/libs/<abi>/libDoctype.so
        internal const string Lib = "Doctype";
#endif

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr lhu_version();

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_quad_size();

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr lhu_create(ref HtmlHostCallbacks host);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void lhu_destroy(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_register_font(IntPtr ctx,
                                                   [MarshalAs(UnmanagedType.LPUTF8Str)] string family,
                                                   int weight, int italic, byte[] data, int len);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void lhu_set_default_font(IntPtr ctx,
                                                       [MarshalAs(UnmanagedType.LPUTF8Str)] string family,
                                                       float size);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void lhu_set_master_css(IntPtr ctx, int mode);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void lhu_set_viewport(IntPtr ctx, float width, float height);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void lhu_set_device_scale(IntPtr ctx, float scale);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_load_html(IntPtr ctx,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string html,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string userCss);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_set_style(IntPtr ctx,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string selector,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string css);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_set_text(IntPtr ctx,
                                              [MarshalAs(UnmanagedType.LPUTF8Str)] string selector,
                                              [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern float lhu_layout(IntPtr ctx, float maxWidth);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern void lhu_record(IntPtr ctx, out HtmlFrame frame);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int lhu_element_at(IntPtr ctx, float x, float y,
                                                 [MarshalAs(UnmanagedType.LPArray)] byte[] outId, int len);

        // Reused across calls: hit testing runs per pointer probe, and during a
        // cross-surface drag that is every frame for every surface under the
        // finger. Unity scripting is single-threaded, which is what makes a
        // shared buffer safe here.
        private static readonly byte[] s_elementIdBuffer = new byte[256];

        /// <summary>Id of the topmost element at a document point, or null.</summary>
        public static string ElementAt(IntPtr ctx, float x, float y)
        {
            byte[] buffer = s_elementIdBuffer;

            if (lhu_element_at(ctx, x, y, buffer, buffer.Length) == 0)
            {
                return null;
            }

            int end = Array.IndexOf(buffer, (byte)0);
            if (end < 0)
            {
                end = buffer.Length;
            }

            // An id longer than the buffer comes back truncated, and the cut
            // can land inside a multi-byte UTF-8 sequence. Decoding a partial
            // sequence yields U+FFFD, so drop the fragment instead: walk back
            // over continuation bytes to the lead and cut there if its
            // sequence did not complete.
            if (end == buffer.Length - 1)
            {
                int lead = end;
                while (lead > 0 && (buffer[lead - 1] & 0xC0) == 0x80)
                {
                    lead--;
                }

                if (lead > 0 && buffer[lead - 1] >= 0xC0)
                {
                    lead--;
                    byte b = buffer[lead];
                    int expected = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
                    if (end - lead < expected)
                    {
                        end = lead;
                    }
                }
            }

            return System.Text.Encoding.UTF8.GetString(buffer, 0, end);
        }

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_element_rect(IntPtr ctx,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string selector,
                                                  out float x, out float y, out float w, out float h);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern float lhu_doc_width(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern float lhu_doc_height(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_mouse_move(IntPtr ctx, float x, float y);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_mouse_down(IntPtr ctx, float x, float y);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_mouse_up(IntPtr ctx, float x, float y);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_mouse_leave(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern int lhu_scroll(IntPtr ctx, float dx, float dy, float x, float y);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern long lhu_quadcache_stat(IntPtr ctx, int which);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr lhu_last_error(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr lhu_cursor(IntPtr ctx);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr lhu_caption(IntPtr ctx);

        public static string Version() => Utf8(lhu_version());

        public static string LastError(IntPtr ctx) => Utf8(lhu_last_error(ctx));

        public static string Cursor(IntPtr ctx) => Utf8(lhu_cursor(ctx));

        public static string Caption(IntPtr ctx) => Utf8(lhu_caption(ctx));

        /// <summary>
        /// Selectors for <c>lhu_quadcache_stat</c>. Mirrors <c>LhuQuadCacheStat</c>
        /// in Native/src/lhu_api.h; the values are part of the ABI.
        /// </summary>
        public enum QuadCacheStat
        {
            Enabled = 0,
            QuadsReplayed = 1,
            QuadsEmitted = 2,
            RunsReplayed = 3,
            FramesFast = 4,
            FramesRebuild = 5,
            FramesPartial = 6,
            Reset = 7,
            Bytes = 8,

            /// <summary>Throws the retained frame away. See <see cref="InvalidateDrawCache"/>.</summary>
            Invalidate = 9,
        }

        // The retained display list shipped after the first plugin binaries did.
        // A project that updates the C# before the native bundle (or keeps a
        // stale bundle in Plugins/, which Unity is happy to do) would otherwise
        // get an EntryPointNotFoundException on the first frame instead of the
        // old, still-correct behaviour. Probe once, then remember.
        private static bool _quadCacheProbed;
        private static bool _quadCacheAvailable;

        public static long QuadCacheStatistic(IntPtr ctx, QuadCacheStat which)
        {
            if (ctx == IntPtr.Zero)
            {
                return -1;
            }

            if (!_quadCacheProbed)
            {
                _quadCacheProbed = true;
                try
                {
                    lhu_quadcache_stat(ctx, (int)QuadCacheStat.Enabled);
                    _quadCacheAvailable = true;
                }
                catch (EntryPointNotFoundException)
                {
                    _quadCacheAvailable = false;
                    Debug.LogWarning(
                        "Doctype: native plugin predates the retained display list " +
                        "(lhu_quadcache_stat is missing). Image atlas repacks cannot be signalled to it. " +
                        "Rebuild the native plugin if you use an image resource provider.");
                }
            }

            return _quadCacheAvailable ? lhu_quadcache_stat(ctx, (int)which) : -1;
        }

        /// <summary>
        /// Tells the native retained display list that every cached quad is
        /// suspect and the next record must redraw from scratch.
        /// </summary>
        /// <remarks>
        /// The engine invalidates itself for everything it can observe: text,
        /// styles, layout, the glyph atlas, the device scale, the viewport. It
        /// cannot observe the *host's* image atlas. Image UVs reach native
        /// through the GetImageUv callback, and native bakes the returned rect
        /// into the quad it caches. If the host repacks its atlas, those baked
        /// UVs now point at the wrong texels and nothing on the native side can
        /// tell. This is the one invalidation the host owns.
        /// </remarks>
        public static void InvalidateDrawCache(IntPtr ctx)
        {
            QuadCacheStatistic(ctx, QuadCacheStat.Invalidate);
        }

        internal static string Utf8(IntPtr ptr)
        {
            return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(ptr);
        }

        /// <summary>
        /// Fails loudly if the managed and native definitions of LhuQuad have
        /// drifted apart, which would otherwise show up as silently garbled
        /// geometry.
        /// </summary>
        public static void AssertLayout()
        {
            int managed = Marshal.SizeOf<HtmlQuad>();
            int native = lhu_quad_size();

            if (managed != native)
            {
                throw new InvalidOperationException(
                    $"HtmlQuad layout mismatch: C# says {managed} bytes, native says {native}. " +
                    "HtmlTypes.cs and Native/src/lhu_types.h are out of sync.");
            }
        }
    }
}
