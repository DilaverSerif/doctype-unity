// Doctype — shared POD contract between the native plugin and C#.
//
// IMPORTANT: every struct here is mirrored 1:1 in
// Assets/Doctype/Runtime/HtmlTypes.cs. If you change a field here,
// change it there too — the C# side blits these directly out of native memory.
//
// All coordinates are in document-space pixels, origin top-left, y down.

#ifndef LHU_TYPES_H
#define LHU_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Quad kinds. Mirrored by LhuQuadType in C#.
enum LhuQuadType
{
    LHU_QUAD_RECT            = 0, // rounded-rect solid fill
    LHU_QUAD_BORDER          = 1, // one edge of a border ring (params.x = edge 0..3)
    LHU_QUAD_GLYPH           = 2, // glyph from the R8 font atlas
    LHU_QUAD_IMAGE           = 3, // sprite from the RGBA image atlas
    LHU_QUAD_LINEAR_GRAD     = 4, // linear gradient, t interpolated per-vertex (exact)
    LHU_QUAD_RADIAL_GRAD     = 5, // radial gradient, t computed per-fragment
    LHU_QUAD_CONIC_GRAD      = 6, // conic gradient, t computed per-fragment
};

// Border edge indices for LHU_QUAD_BORDER.
enum LhuBorderEdge
{
    LHU_EDGE_TOP    = 0,
    LHU_EDGE_RIGHT  = 1,
    LHU_EDGE_BOTTOM = 2,
    LHU_EDGE_LEFT   = 3,
};

// A single axis-aligned quad to be drawn on the GPU.
//
// The shader reconstructs everything (rounded corners, border rings, clipping)
// analytically from these fields — nothing is rasterized on the CPU except
// glyph coverage.
//
// Layout is explicitly all-float32 + one uint32 so that C# can blit it.
typedef struct LhuQuad
{
    // Geometry of the quad in document space.
    float x, y, w, h;

    // Corner radii, elliptical (x and y independent), in the order
    // top-left, top-right, bottom-right, bottom-left.
    float rx[4];
    float ry[4];

    // Border widths: left, top, right, bottom. Only meaningful for
    // LHU_QUAD_BORDER, but the fill quad also carries them so the shader can
    // derive the inner rounded rect when it needs to.
    float border[4];

    // Clip rectangle (document space) and its corner radii. A quad is
    // discarded outside this rounded rect. w<=0 means "no clip".
    float clip_x, clip_y, clip_w, clip_h;
    float clip_r[4];

    // UV rect into the relevant atlas (glyph / image). u0,v0,u1,v1.
    float u0, v0, u1, v1;

    // Type-specific parameters:
    //   BORDER       -> params[0] = edge index
    //   RADIAL_GRAD  -> params[0..1] = center xy, params[2..3] = radius xy
    //   CONIC_GRAD   -> params[0..1] = center xy, params[2] = angle (deg),
    //                   params[3] = radius
    //   LINEAR_GRAD  -> params[0..1] = axis start xy, params[2..3] = axis end xy
    float params[4];

    // Row index into the gradient LUT texture (gradients only).
    int32_t grad_row;

    // Quad kind, one of LhuQuadType.
    int32_t type;

    // Fill / tint colour, non-premultiplied sRGB, packed as R | G<<8 | B<<16 | A<<24.
    uint32_t color;

    int32_t _pad; // keep the struct 16-byte aligned
} LhuQuad;

// Snapshot of one recorded frame. Pointers stay valid until the next call to
// lhu_document_record() or lhu_document_destroy().
typedef struct LhuFrame
{
    const LhuQuad* quads;
    int32_t        quad_count;

    // Document extents produced by the last layout pass.
    float doc_width;
    float doc_height;

    // Font atlas state. `font_atlas_version` increases whenever new glyphs were
    // rasterized, which is the signal for C# to re-upload the texture.
    const uint8_t* font_atlas_pixels; // R8, font_atlas_w * font_atlas_h bytes
    int32_t        font_atlas_w;
    int32_t        font_atlas_h;
    int32_t        font_atlas_version;

    // Gradient LUT, RGBA8, grad_lut_w * grad_lut_rows texels.
    const uint8_t* grad_lut_pixels;
    int32_t        grad_lut_w;
    int32_t        grad_lut_rows;
    int32_t        grad_lut_version;

    // What changed since the previous recorded frame, so a host that keeps its
    // render target alive can repaint just that. Encoded so that all-zero (a
    // plugin older than this field, a zeroed struct) means "repaint everything":
    //   LHU_DIRTY_MODE_FULL   (0) -> repaint the whole target
    //   LHU_DIRTY_MODE_NONE   (1) -> byte-identical to the previous frame
    //   LHU_DIRTY_MODE_RECT   (2) -> only the rect below changed (document
    //                                space, already padded for the AA skirt)
    //   LHU_DIRTY_MODE_SCROLL (3) -> the frame is the previous one with the
    //                                pixels of scroll_* translated by
    //                                (scroll_dx, scroll_dy), plus the dirty
    //                                rect below (the strip that scrolled in).
    //                                A host that cannot move pixels may treat
    //                                this exactly like FULL.
    int32_t dirty_mode;
    float   dirty_x, dirty_y, dirty_w, dirty_h;

    // Only meaningful when dirty_mode == LHU_DIRTY_MODE_SCROLL. The rect is the
    // DESTINATION of the copy (document space, always whole pixels): the host
    // fills it from the previous frame's pixels at (x - scroll_dx, y -
    // scroll_dy), BEFORE repainting the dirty rects. Content scrolling down by
    // 40 is dy = -40. Emitted only when every quad of the new frame was
    // verified against the translation, so acting on it can never produce
    // wrong pixels.
    float scroll_x, scroll_y, scroll_w, scroll_h;
    float scroll_dx, scroll_dy;

    // Second dirty rect, used by SCROLL: the scrolled window's edges live on
    // fractional pixels whenever text sizes above them do, and the pixel row
    // straddling such an edge blends content with background -- it cannot ride
    // a whole-pixel copy and is repainted instead. One edge sliver is adjacent
    // to the entered strip (dirty_*) already; this is the opposite one. Zero
    // size when unused.
    float dirty2_x, dirty2_y, dirty2_w, dirty2_h;
} LhuFrame;

enum LhuDirtyMode
{
    LHU_DIRTY_MODE_FULL   = 0,
    LHU_DIRTY_MODE_NONE   = 1,
    LHU_DIRTY_MODE_RECT   = 2,
    LHU_DIRTY_MODE_SCROLL = 3,
};

// Callbacks the host (Unity) provides so the engine can resolve external
// resources. All strings are UTF-8 and only valid for the duration of the call.
typedef struct LhuHostCallbacks
{
    void* user_data;

    // Asked for the pixel size of an image. Return 0 in both fields if the
    // image is not (yet) available; litehtml will lay out with a zero-sized box
    // and you can re-layout once it loads.
    void (*get_image_size)(void* user_data, const char* url, int32_t* out_w, int32_t* out_h);

    // Asked to begin loading an image. Fire and forget.
    void (*load_image)(void* user_data, const char* url);

    // Asked for the UV rect of an already-loaded image inside the host's image
    // atlas. Return 0 to skip drawing.
    int32_t (*get_image_uv)(void* user_data, const char* url, float* out_uv4);

    // A stylesheet needs to be imported (<link rel=stylesheet> / @import).
    // Return UTF-8 CSS text, or NULL if it cannot be resolved. The returned
    // pointer only has to stay valid until the next callback invocation.
    const char* (*import_css)(void* user_data, const char* url, const char* baseurl);

    // An <a href> was clicked.
    void (*on_anchor_click)(void* user_data, const char* url);

    // Any non-anchor element was clicked. litehtml bubbles the click to the
    // parent when this returns 0, so return non-zero once something has handled
    // it. `id`, `class_names` and `action` (the data-action attribute) are empty
    // strings rather than NULL when absent.
    int32_t (*on_element_click)(void* user_data, const char* id, const char* tag, const char* class_names,
                                const char* action);

    // The cursor should change (CSS cursor property).
    void (*on_set_cursor)(void* user_data, const char* cursor);
} LhuHostCallbacks;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LHU_TYPES_H
