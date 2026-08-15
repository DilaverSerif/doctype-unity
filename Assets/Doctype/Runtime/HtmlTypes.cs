using System;
using System.Runtime.InteropServices;

namespace Doctype
{
    /// <summary>
    /// Which default stylesheet is applied before the page's own CSS.
    /// </summary>
    /// <remarks>
    /// litehtml re-parses this for every document it builds, and that step
    /// dominates the cost — more than parsing the page itself. The trimmed
    /// sheet roughly halves document creation for a typical UI page.
    /// </remarks>
    public enum HtmlMasterStylesheet
    {
        /// <summary>
        /// Trimmed for game UI: drops form controls, legacy presentational tags,
        /// definition lists and table attribute selectors. Everything else
        /// matches litehtml's own values.
        /// </summary>
        GameUI = 0,

        /// <summary>litehtml's full default stylesheet.</summary>
        Full = 1,
    }

    /// <summary>
    /// A click on an element that is not an &lt;a href&gt;.
    /// </summary>
    /// <remarks>
    /// litehtml walks the click up the tree until something handles it, so a
    /// click on the text inside a button still arrives with the button's id.
    /// </remarks>
    public readonly struct HtmlElementClick
    {
        /// <summary>The element's id attribute, or empty.</summary>
        public readonly string Id;

        /// <summary>Tag name, lowercase, e.g. "button".</summary>
        public readonly string Tag;

        /// <summary>The raw class attribute, or empty.</summary>
        public readonly string ClassNames;

        /// <summary>The data-action attribute, or empty.</summary>
        public readonly string Action;

        public HtmlElementClick(string id, string tag, string classNames, string action)
        {
            Id = id;
            Tag = tag;
            ClassNames = classNames;
            Action = action;
        }

        public override string ToString()
        {
            return $"<{Tag} id='{Id}' class='{ClassNames}' data-action='{Action}'>";
        }
    }

    /// <summary>
    /// What a <see cref="HtmlQuad"/> represents. Mirrors LhuQuadType in
    /// Native/src/lhu_types.h.
    /// </summary>
    public enum HtmlQuadType
    {
        Rect = 0,
        Border = 1,
        Glyph = 2,
        Image = 3,
        LinearGradient = 4,
        RadialGradient = 5,
        ConicGradient = 6,
    }

    /// <summary>
    /// One GPU quad produced by the native recorder.
    /// </summary>
    /// <remarks>
    /// This is blitted straight out of native memory, so the field order and
    /// types must match LhuQuad in Native/src/lhu_types.h exactly.
    /// <see cref="HtmlNative.AssertLayout"/> checks the size at runtime.
    /// </remarks>
    [StructLayout(LayoutKind.Sequential)]
    public struct HtmlQuad
    {
        public float X, Y, W, H;

        // Corner radii in CSS order: top-left, top-right, bottom-right, bottom-left.
        public float Rx0, Rx1, Rx2, Rx3;
        public float Ry0, Ry1, Ry2, Ry3;

        // Border widths: left, top, right, bottom.
        public float BorderL, BorderT, BorderR, BorderB;

        // Rounded clip rectangle. ClipW below zero means "unclipped".
        public float ClipX, ClipY, ClipW, ClipH;
        public float ClipR0, ClipR1, ClipR2, ClipR3;

        public float U0, V0, U1, V1;

        // Type-specific: border edge index / gradient geometry.
        public float P0, P1, P2, P3;

        public int GradRow;
        public int TypeRaw;

        /// <summary>Non-premultiplied sRGB packed as R | G&lt;&lt;8 | B&lt;&lt;16 | A&lt;&lt;24.</summary>
        public uint Color;

        private int _pad;

        public HtmlQuadType Type => (HtmlQuadType)TypeRaw;

        public bool IsClipped => ClipW >= 0f;
    }

    /// <summary>
    /// A recorded frame. All pointers are owned by native code and stay valid
    /// only until the next <see cref="HtmlDocument.Record"/> call.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct HtmlFrame
    {
        public IntPtr Quads;
        public int QuadCount;

        public float DocWidth;
        public float DocHeight;

        public IntPtr FontAtlasPixels;
        public int FontAtlasWidth;
        public int FontAtlasHeight;
        public int FontAtlasVersion;

        public IntPtr GradLutPixels;
        public int GradLutWidth;
        public int GradLutRows;
        public int GradLutVersion;
    }

    // Delegates matching LhuHostCallbacks. Every implementation passed to native
    // code must be a static method tagged with [MonoPInvokeCallback] so IL2CPP
    // can generate a reverse wrapper for it.

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetImageSizeCallback(IntPtr userData, IntPtr url, IntPtr outWidth, IntPtr outHeight);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LoadImageCallback(IntPtr userData, IntPtr url);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int GetImageUvCallback(IntPtr userData, IntPtr url, IntPtr outUv4);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr ImportCssCallback(IntPtr userData, IntPtr url, IntPtr baseUrl);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void AnchorClickCallback(IntPtr userData, IntPtr url);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int ElementClickCallback(IntPtr userData, IntPtr id, IntPtr tag, IntPtr classNames,
                                             IntPtr action);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetCursorCallback(IntPtr userData, IntPtr cursor);

    [StructLayout(LayoutKind.Sequential)]
    public struct HtmlHostCallbacks
    {
        public IntPtr UserData;
        public IntPtr GetImageSize;
        public IntPtr LoadImage;
        public IntPtr GetImageUv;
        public IntPtr ImportCss;
        public IntPtr OnAnchorClick;
        public IntPtr OnElementClick;
        public IntPtr OnSetCursor;
    }

    /// <summary>
    /// What an input event dirtied. Mirrors <c>LhuDirtyFlags</c> in
    /// Native/src/lhu_api.h; the values are part of the ABI.
    /// </summary>
    /// <remarks>
    /// <see cref="Paint"/> alone means styles changed but every box kept its
    /// size and place — re-record and redraw. <see cref="Layout"/> means a
    /// :hover/:active rule touched a value layout reads, and the positions in
    /// the render tree are stale until layout runs again. The distinction is
    /// what lets a pointer move over a recolouring button cost a redraw instead
    /// of a full layout pass.
    /// </remarks>
    [Flags]
    public enum HtmlDirty
    {
        None = 0,
        Paint = 1,
        Layout = 2,
    }

}