using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using AOT;
using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;
using UnityEngine;

namespace LiteHtmlUnity
{
    /// <summary>
    /// Supplies images and stylesheets to a document.
    /// </summary>
    /// <remarks>
    /// litehtml asks for an image's size during layout, synchronously. An
    /// implementation that cannot answer yet should report 0x0 and start
    /// loading; once the image arrives, bump <see cref="Version"/> and the view
    /// will re-run layout.
    /// <para>
    /// <b>Bump <see cref="Version"/> after repacking the atlas, too.</b> Native
    /// keeps a retained display list: a subtree whose geometry has not moved is
    /// not re-walked, its quads are replayed from a snapshot, and the image UVs
    /// baked into them are whatever <see cref="TryGetImageUv"/> returned when
    /// they were captured. A repack that moves a sprite without moving its box
    /// is therefore invisible to the engine -- the cached quads would keep
    /// sampling the old texels forever. <see cref="Version"/> is the only signal
    /// the host has; <see cref="LiteHtmlView"/> turns a change in it into a
    /// draw-cache invalidation as well as a relayout.
    /// </para>
    /// <para>
    /// <b><see cref="TryGetImageUv"/> must be a pure lookup.</b> It is called
    /// from inside the native draw, once per image quad, while quads for the
    /// same frame are already in the output buffer. A provider that packed or
    /// repacked from inside it would move the UVs of quads it had already
    /// answered for, in the same frame, and nothing would catch it. Do the
    /// packing in <see cref="BeginLoadImage"/> or on the main loop, then bump
    /// <see cref="Version"/>.
    /// </para>
    /// </remarks>
    public interface ILiteHtmlResourceProvider
    {
        /// <summary>Incremented whenever previously-unavailable content became available.</summary>
        int Version { get; }

        /// <summary>The single texture every image UV refers to, or null when there are no images.</summary>
        Texture ImageAtlas { get; }

        bool TryGetImageSize(string url, out int width, out int height);

        void BeginLoadImage(string url);

        bool TryGetImageUv(string url, out Rect uv);

        /// <summary>Returns stylesheet text for a &lt;link&gt; or @import, or null.</summary>
        string LoadCss(string url, string baseUrl);
    }

    /// <summary>
    /// Managed owner of one native litehtml document.
    /// </summary>
    public sealed class LiteHtmlDocument : IDisposable
    {
        private IntPtr _ctx;
        private ILiteHtmlResourceProvider _resources;
        private GCHandle _self;

        // The native side stores these function pointers, so the delegates must
        // outlive the context or the GC will collect them mid-call.
        private readonly GetImageSizeCallback _getImageSize;
        private readonly LoadImageCallback _loadImage;
        private readonly GetImageUvCallback _getImageUv;
        private readonly ImportCssCallback _importCss;
        private readonly AnchorClickCallback _anchorClick;
        private readonly ElementClickCallback _elementClick;
        private readonly SetCursorCallback _setCursor;

        // Handlers registered by element id, e.g. BindClick("save", ...).
        private readonly Dictionary<string, Action<LiteHtmlElementClick>> _clickBindings =
            new Dictionary<string, Action<LiteHtmlElementClick>>();

        private IntPtr _cssBuffer;

        /// <summary>Raised when a link inside the document is clicked.</summary>
        public event Action<string> AnchorClicked;

        /// <summary>
        /// Raised when any non-anchor element is clicked.
        /// </summary>
        /// <remarks>
        /// litehtml bubbles a click up the tree until a handler claims it, so
        /// this fires first for the deepest element under the pointer. Returning
        /// without a match on <see cref="BindClick"/> lets the parent get a turn.
        /// </remarks>
        public event Action<LiteHtmlElementClick> ElementClicked;

        /// <summary>Raised when the CSS cursor under the pointer changes.</summary>
        public event Action<string> CursorChanged;

        /// <summary>
        /// Supplies images and stylesheets. Swapping providers replaces every
        /// image UV, so the retained display list is dropped on assignment.
        /// </summary>
        public ILiteHtmlResourceProvider Resources
        {
            get => _resources;
            set
            {
                if (ReferenceEquals(_resources, value))
                {
                    return;
                }

                _resources = value;
                InvalidateDrawCache();
            }
        }

        public bool IsValid => _ctx != IntPtr.Zero;

        /// <summary>
        /// Discards native's retained display list, forcing the next
        /// <see cref="Record"/> to redraw the document from scratch.
        /// </summary>
        /// <remarks>
        /// Only needed for things native cannot observe -- in practice exactly
        /// one: the host repacking the image atlas behind
        /// <see cref="ILiteHtmlResourceProvider.TryGetImageUv"/>. Text, styles,
        /// layout, fonts, the glyph atlas, viewport and device scale are all
        /// invalidated by the engine itself. Calling this more often than
        /// necessary is safe but costs a full redraw of the next frame.
        /// </remarks>
        public void InvalidateDrawCache()
        {
            if (IsValid)
            {
                LiteHtmlNative.InvalidateDrawCache(_ctx);
            }
        }

        public float DocumentWidth => IsValid ? LiteHtmlNative.lhu_doc_width(_ctx) : 0f;

        public float DocumentHeight => IsValid ? LiteHtmlNative.lhu_doc_height(_ctx) : 0f;

        public string LastError => IsValid ? LiteHtmlNative.LastError(_ctx) : "disposed";

        public LiteHtmlDocument()
        {
            LiteHtmlNative.AssertLayout();

            _self = GCHandle.Alloc(this, GCHandleType.Normal);

            _getImageSize = OnGetImageSize;
            _loadImage = OnLoadImage;
            _getImageUv = OnGetImageUv;
            _importCss = OnImportCss;
            _anchorClick = OnAnchorClick;
            _elementClick = OnElementClick;
            _setCursor = OnSetCursor;

            var host = new LiteHtmlHostCallbacks
            {
                UserData = GCHandle.ToIntPtr(_self),
                GetImageSize = Marshal.GetFunctionPointerForDelegate(_getImageSize),
                LoadImage = Marshal.GetFunctionPointerForDelegate(_loadImage),
                GetImageUv = Marshal.GetFunctionPointerForDelegate(_getImageUv),
                ImportCss = Marshal.GetFunctionPointerForDelegate(_importCss),
                OnAnchorClick = Marshal.GetFunctionPointerForDelegate(_anchorClick),
                OnElementClick = Marshal.GetFunctionPointerForDelegate(_elementClick),
                OnSetCursor = Marshal.GetFunctionPointerForDelegate(_setCursor),
            };

            _ctx = LiteHtmlNative.lhu_create(ref host);

            if (_ctx == IntPtr.Zero)
            {
                throw new InvalidOperationException("lhu_create returned null");
            }
        }

        public void Dispose()
        {
            if (_ctx != IntPtr.Zero)
            {
                LiteHtmlNative.lhu_destroy(_ctx);
                _ctx = IntPtr.Zero;
            }

            if (_cssBuffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(_cssBuffer);
                _cssBuffer = IntPtr.Zero;
            }

            if (_self.IsAllocated)
            {
                _self.Free();
            }
        }

        /// <summary>
        /// Runs <paramref name="handler"/> when the element with this id is
        /// clicked. Pass null to remove the binding.
        /// </summary>
        /// <example>
        /// <code>
        /// document.BindClick("save", _ => Debug.Log("Save clicked"));
        /// // &lt;button id="save"&gt;Kaydet&lt;/button&gt;
        /// </code>
        /// </example>
        public void BindClick(string elementId, Action<LiteHtmlElementClick> handler)
        {
            if (string.IsNullOrEmpty(elementId))
            {
                return;
            }

            if (handler == null)
            {
                _clickBindings.Remove(elementId);
            }
            else
            {
                _clickBindings[elementId] = handler;
            }
        }

        /// <summary>Removes every id binding.</summary>
        public void ClearClickBindings()
        {
            _clickBindings.Clear();
        }

        // --- setup -----------------------------------------------------------

        /// <param name="family">CSS family name to match, e.g. "arial" or "sans-serif".</param>
        /// <param name="weight">CSS weight; 400 normal, 700 bold.</param>
        public bool RegisterFont(string family, byte[] ttf, int weight = 400, bool italic = false)
        {
            if (!IsValid || ttf == null || ttf.Length == 0)
            {
                return false;
            }

            return LiteHtmlNative.lhu_register_font(_ctx, family, weight, italic ? 1 : 0, ttf, ttf.Length) != 0;
        }

        public void SetDefaultFont(string family, float size)
        {
            if (IsValid)
            {
                LiteHtmlNative.lhu_set_default_font(_ctx, family, size);
            }
        }

        /// <summary>
        /// Chooses the default stylesheet. Takes effect on the next
        /// <see cref="LoadHtml"/>.
        /// </summary>
        public void SetMasterStylesheet(LiteHtmlMasterStylesheet sheet)
        {
            if (IsValid)
            {
                LiteHtmlNative.lhu_set_master_css(_ctx, (int)sheet);
            }
        }

        public void SetViewport(float width, float height)
        {
            if (IsValid)
            {
                LiteHtmlNative.lhu_set_viewport(_ctx, width, height);
            }
        }

        public void SetDeviceScale(float scale)
        {
            if (IsValid)
            {
                LiteHtmlNative.lhu_set_device_scale(_ctx, scale);
            }
        }

        // --- document --------------------------------------------------------

        public bool LoadHtml(string html, string userCss = null)
        {
            if (!IsValid)
            {
                return false;
            }

            return LiteHtmlNative.lhu_load_html(_ctx, html ?? string.Empty, userCss) != 0;
        }

        /// <summary>
        /// Replaces the inline style of the first element matching
        /// <paramref name="selector"/> without re-parsing the document. Returns
        /// true when the style actually changed.
        /// </summary>
        /// <remarks>
        /// This is the path for animating: a bar's height, a gradient's angle, a
        /// colour. Rebuilding the markup instead re-parses the whole page, which
        /// on a mid-range phone costs milliseconds per frame.
        /// <para>
        /// Unlike <see cref="SetText"/> it can never skip layout — a declaration
        /// has no measured size to compare — so cost scales with the mutated
        /// element's subtree. Animate a leaf, not a container that a long list
        /// hangs off.
        /// </para>
        /// <para>
        /// Changing display, float or position rebuilds the render tree for that
        /// subtree; correct, but roughly a fresh layout, so not a per-frame move.
        /// </para>
        /// </remarks>
        public bool SetStyle(string selector, string css)
        {
            if (!IsValid || string.IsNullOrEmpty(selector))
            {
                return false;
            }

            return LiteHtmlNative.lhu_set_style(_ctx, selector, css ?? string.Empty) != 0;
        }

        /// <summary>
        /// Replaces the text of the first element matching <paramref name="selector"/>
        /// without re-parsing the document. Returns true when the text actually
        /// changed, so the caller can skip a re-layout when it did not.
        /// </summary>
        /// <remarks>
        /// This is the cheap path for a score, a counter or an item label:
        /// <see cref="LoadHtml"/> re-parses everything and re-runs selector
        /// matching, which on a mid-range phone costs milliseconds, while this
        /// only re-measures the affected text nodes.
        /// <para>
        /// Call <see cref="Layout"/> before the next <see cref="Record"/> —
        /// recording without a layout in between draws the previous frame's line
        /// boxes.
        /// </para>
        /// <para>
        /// Returns false, leaving the document untouched, when nothing matched,
        /// when the matched element contains child elements rather than only
        /// text, or when it is a flex or table container. <see cref="LastError"/>
        /// says which.
        /// </para>
        /// </remarks>
        public bool SetText(string selector, string text)
        {
            if (!IsValid || string.IsNullOrEmpty(selector))
            {
                return false;
            }

            return LiteHtmlNative.lhu_set_text(_ctx, selector, text ?? string.Empty) != 0;
        }

        /// <summary>
        /// Id of the topmost element covering a document point, or null when
        /// nothing with an id is there.
        /// </summary>
        /// <remarks>
        /// A pure query: it moves no hover or active state, which is what makes
        /// it usable as a drop-target test while a drag is in flight. Driving
        /// <see cref="MouseMove"/> to find the same answer would light the
        /// element up as hovered.
        /// <para>
        /// Elements without an id are see-through to it, so an icon inside a slot
        /// reports the slot — normally the thing a caller wants to act on.
        /// </para>
        /// </remarks>
        public string ElementAt(Vector2 documentPoint)
        {
            return IsValid ? LiteHtmlNative.ElementAt(_ctx, documentPoint.x, documentPoint.y) : null;
        }

        /// <summary>
        /// Painted rectangle of the first element matching <paramref name="selector"/>,
        /// in CSS pixels, valid after <see cref="Layout"/>. False when nothing
        /// matched or the element was not laid out.
        /// </summary>
        /// <remarks>
        /// The document is never narrower than the viewport it was laid out
        /// against — litehtml does not shrink-to-fit at the document level — so
        /// <see cref="DocumentWidth"/> cannot tell you how wide a badge or a
        /// counter actually is. Lay out against the widest it may become, then
        /// measure it with this.
        /// <para>
        /// The rectangle is the border box, padding and borders included, because
        /// that is what gets painted. Sizing a surface to the content box alone
        /// would clip the background.
        /// </para>
        /// </remarks>
        public bool TryGetElementRect(string selector, out Rect rect)
        {
            rect = default;

            if (!IsValid || string.IsNullOrEmpty(selector))
            {
                return false;
            }

            if (LiteHtmlNative.lhu_element_rect(_ctx, selector,
                                                out float x, out float y, out float w, out float h) == 0)
            {
                return false;
            }

            rect = new Rect(x, y, w, h);
            return true;
        }

        /// <summary>Runs layout at the given width and returns the document height.</summary>
        public float Layout(float maxWidth)
        {
            return IsValid ? LiteHtmlNative.lhu_layout(_ctx, maxWidth) : 0f;
        }

        /// <summary>
        /// Records this frame's quads.
        /// </summary>
        /// <remarks>
        /// The returned array aliases native memory and is invalidated by the
        /// next Record call, so consume it before recording again. It is
        /// deliberately not a copy: a busy page produces thousands of quads per
        /// frame and copying them would dominate the cost.
        /// </remarks>
        public unsafe NativeArray<LiteHtmlQuad> Record(out LiteHtmlFrame frame)
        {
            frame = default;

            if (!IsValid)
            {
                return default;
            }

            LiteHtmlNative.lhu_record(_ctx, out frame);

            if (frame.Quads == IntPtr.Zero || frame.QuadCount <= 0)
            {
                return default;
            }

            var array = NativeArrayUnsafeUtility.ConvertExistingDataToNativeArray<LiteHtmlQuad>(
                (void*)frame.Quads, frame.QuadCount, Allocator.None);

#if ENABLE_UNITY_COLLECTIONS_CHECKS
            NativeArrayUnsafeUtility.SetAtomicSafetyHandle(ref array, AtomicSafetyHandle.GetTempMemoryHandle());
#endif
            return array;
        }

        // --- input -----------------------------------------------------------

        /// <returns>True when the document changed and needs re-recording.</returns>
        public bool MouseMove(Vector2 documentPoint)
        {
            return IsValid && LiteHtmlNative.lhu_mouse_move(_ctx, documentPoint.x, documentPoint.y) != 0;
        }

        public bool MouseDown(Vector2 documentPoint)
        {
            return IsValid && LiteHtmlNative.lhu_mouse_down(_ctx, documentPoint.x, documentPoint.y) != 0;
        }

        public bool MouseUp(Vector2 documentPoint)
        {
            return IsValid && LiteHtmlNative.lhu_mouse_up(_ctx, documentPoint.x, documentPoint.y) != 0;
        }

        public bool MouseLeave()
        {
            return IsValid && LiteHtmlNative.lhu_mouse_leave(_ctx) != 0;
        }

        /// <returns>Number of elements that consumed the scroll; 0 means the host should scroll.</returns>
        public int Scroll(Vector2 delta, Vector2 documentPoint)
        {
            return IsValid ? LiteHtmlNative.lhu_scroll(_ctx, delta.x, delta.y, documentPoint.x, documentPoint.y) : 0;
        }

        // --- native callbacks ------------------------------------------------

        private static LiteHtmlDocument FromUserData(IntPtr userData)
        {
            if (userData == IntPtr.Zero)
            {
                return null;
            }

            var handle = GCHandle.FromIntPtr(userData);
            return handle.IsAllocated ? handle.Target as LiteHtmlDocument : null;
        }

        [MonoPInvokeCallback(typeof(GetImageSizeCallback))]
        private static void OnGetImageSize(IntPtr userData, IntPtr url, IntPtr outWidth, IntPtr outHeight)
        {
            int w = 0, h = 0;

            try
            {
                var doc = FromUserData(userData);
                doc?.Resources?.TryGetImageSize(LiteHtmlNative.Utf8(url), out w, out h);
            }
            catch (Exception e)
            {
                // Exceptions must never cross back into native code.
                Debug.LogException(e);
                w = 0;
                h = 0;
            }

            Marshal.WriteInt32(outWidth, w);
            Marshal.WriteInt32(outHeight, h);
        }

        [MonoPInvokeCallback(typeof(LoadImageCallback))]
        private static void OnLoadImage(IntPtr userData, IntPtr url)
        {
            try
            {
                var doc = FromUserData(userData);
                doc?.Resources?.BeginLoadImage(LiteHtmlNative.Utf8(url));
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
        }

        [MonoPInvokeCallback(typeof(GetImageUvCallback))]
        private static int OnGetImageUv(IntPtr userData, IntPtr url, IntPtr outUv4)
        {
            try
            {
                var doc = FromUserData(userData);
                if (doc?.Resources == null)
                {
                    return 0;
                }

                if (!doc.Resources.TryGetImageUv(LiteHtmlNative.Utf8(url), out Rect uv))
                {
                    return 0;
                }

                var values = new[] { uv.xMin, uv.yMin, uv.xMax, uv.yMax };
                Marshal.Copy(values, 0, outUv4, 4);
                return 1;
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                return 0;
            }
        }

        [MonoPInvokeCallback(typeof(ImportCssCallback))]
        private static IntPtr OnImportCss(IntPtr userData, IntPtr url, IntPtr baseUrl)
        {
            try
            {
                var doc = FromUserData(userData);
                string css = doc?.Resources?.LoadCss(LiteHtmlNative.Utf8(url), LiteHtmlNative.Utf8(baseUrl));

                if (doc == null || string.IsNullOrEmpty(css))
                {
                    return IntPtr.Zero;
                }

                // Native copies the string immediately, so one reusable buffer
                // is enough; it is freed on the next import and on dispose.
                if (doc._cssBuffer != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(doc._cssBuffer);
                }

                // litehtml expects UTF-8, so encode explicitly rather than
                // going through the ANSI helper.
                byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(css);
                doc._cssBuffer = Marshal.AllocHGlobal(utf8.Length + 1);
                Marshal.Copy(utf8, 0, doc._cssBuffer, utf8.Length);
                Marshal.WriteByte(doc._cssBuffer, utf8.Length, 0);

                return doc._cssBuffer;
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                return IntPtr.Zero;
            }
        }

        [MonoPInvokeCallback(typeof(AnchorClickCallback))]
        private static void OnAnchorClick(IntPtr userData, IntPtr url)
        {
            try
            {
                FromUserData(userData)?.AnchorClicked?.Invoke(LiteHtmlNative.Utf8(url));
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
        }

        [MonoPInvokeCallback(typeof(ElementClickCallback))]
        private static int OnElementClick(IntPtr userData, IntPtr id, IntPtr tag, IntPtr classNames, IntPtr action)
        {
            try
            {
                LiteHtmlDocument doc = FromUserData(userData);
                if (doc == null)
                {
                    return 0;
                }

                var click = new LiteHtmlElementClick(LiteHtmlNative.Utf8(id), LiteHtmlNative.Utf8(tag),
                                                     LiteHtmlNative.Utf8(classNames), LiteHtmlNative.Utf8(action));

                doc.ElementClicked?.Invoke(click);

                // Only an id binding claims the click. Everything else keeps
                // bubbling, which is what lets a click on a button's label text
                // reach the button itself.
                if (!string.IsNullOrEmpty(click.Id) &&
                    doc._clickBindings.TryGetValue(click.Id, out Action<LiteHtmlElementClick> handler))
                {
                    handler(click);
                    return 1;
                }

                return 0;
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                return 0;
            }
        }

        [MonoPInvokeCallback(typeof(SetCursorCallback))]
        private static void OnSetCursor(IntPtr userData, IntPtr cursor)
        {
            try
            {
                FromUserData(userData)?.CursorChanged?.Invoke(LiteHtmlNative.Utf8(cursor));
            }
            catch (Exception e)
            {
                Debug.LogException(e);
            }
        }
    }
}
