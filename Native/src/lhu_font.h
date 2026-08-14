// LiteHtmlUnity — font handling and glyph atlas, built on stb_truetype.
//
// Responsibilities:
//   * map a CSS font-description (family list + size + weight + style) onto a
//     registered typeface,
//   * report metrics to litehtml so it can lay text out,
//   * rasterize glyphs on demand into a single R8 atlas that Unity uploads as
//     a texture.
//
// Text measurement and text drawing walk the string through the *same*
// function (walk_text) so that layout and drawing can never disagree about
// advances — a mismatch there shows up as text drifting out of its box.

#ifndef LHU_FONT_H
#define LHU_FONT_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

namespace lhu
{

// One rasterized glyph inside the atlas.
struct Glyph
{
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // atlas UVs, normalized
    float xoff = 0, yoff = 0;             // pen(baseline) -> quad top-left, pixels
    float w = 0, h = 0;                   // quad size, pixels
    float advance = 0;                    // pen advance, pixels
    bool  has_pixels = false;             // false for whitespace / .notdef
};

// Single-channel glyph atlas with a shelf packer.
class Atlas
{
  public:
    Atlas();

    // Reserve space for a `w` x `h` bitmap. Returns false when the atlas is
    // full even after growing to the maximum size.
    bool alloc(int w, int h, int* out_x, int* out_y);

    void blit(const uint8_t* src, int sw, int sh, int dx, int dy);

    // Throws away all packed content and starts over. Callers must also drop
    // any cached Glyph UVs — see FontManager::glyph_cache_epoch().
    void reset();

    const uint8_t* pixels() const { return m_pixels.data(); }
    int            width() const { return m_width; }
    int            height() const { return m_height; }

    // Bumped whenever pixel content changed; Unity re-uploads on change.
    int version() const { return m_version; }

    // Bumped whenever the atlas was reset/resized, invalidating existing UVs.
    int epoch() const { return m_epoch; }

  private:
    bool grow();

    std::vector<uint8_t> m_pixels;
    int                  m_width   = 0;
    int                  m_height  = 0;
    int                  m_shelf_y = 0; // top of the current shelf
    int                  m_shelf_h = 0; // height of the current shelf
    int                  m_pen_x   = 0; // next free x on the current shelf
    int                  m_version = 0;
    int                  m_epoch   = 0;
};

// A registered TTF/TTC file.
struct Typeface
{
    std::string                     family;      // lowercased
    int                             weight = 400;
    bool                            italic = false;
    std::vector<uint8_t>            data;
    std::unique_ptr<stbtt_fontinfo> info;

    // True when the file carries a GPOS or kern table. Fonts without either
    // never produce a non-zero kerning value, so they never populate the
    // kerning cache -- which is what keeps a large CJK face from filling it
    // with hundreds of thousands of zero entries.
    bool has_kerning = false;

    ~Typeface();
};

// A typeface at a concrete pixel size, plus the text-decoration state that
// litehtml folds into the font description.
//
// Fonts are pooled and reference counted. litehtml creates fonts while parsing
// and destroys them with the document, so a page that re-parses every frame
// would otherwise churn through a new Font — and a fresh glyph cache — sixty
// times a second.
struct Font
{
    Typeface* face = nullptr;

    // Identity used to pool identical descriptions.
    std::string key;
    int         refs = 0;

    // High bits of the glyph cache key: typeface and size. Glyph rasterization
    // does not depend on decoration, so those fonts share cached glyphs.
    uint64_t glyph_key_base = 0;

    // Index into FontManager::m_faces. Kerning is cached per typeface, not per
    // Font, because the value stbtt returns is in font units and therefore
    // independent of pixel size.
    uint32_t face_index = 0;

    float size    = 16.f;
    float scale   = 1.f; // font units -> pixels
    float ascent  = 0.f; // positive, baseline -> top
    float descent = 0.f; // positive, baseline -> bottom
    float height  = 0.f; // ascent + descent
    float x_height = 0.f;
    float ch_width = 0.f;

    int decoration_line  = 0;
    int decoration_style = 0;
    // Non-premultiplied RGBA. Alpha 0 means "use the text colour".
    uint32_t decoration_color = 0;
    float    decoration_thickness = 0.f;
    float    underline_offset     = 0.f;

};

class FontManager
{
  public:
    FontManager();
    ~FontManager();

    // Registers a font file. `family` is matched case-insensitively against
    // CSS font-family names. Returns false if stb_truetype rejects the data.
    bool register_font(const char* family, int weight, bool italic, const uint8_t* data, size_t len);

    // Family used when nothing in the CSS list matches.
    void set_default_family(const char* family);
    const std::string& default_family() const { return m_default_family; }

    bool has_fonts() const { return !m_faces.empty(); }

    // `family_list` is a raw CSS font-family value, e.g. "Segoe UI, Arial, sans-serif".
    Font* create_font(const char* family_list, float size, int weight, bool italic, int decoration_line,
                      int decoration_style, uint32_t decoration_color, float decoration_thickness);

    void destroy_font(Font* font);

    // Sum of advances (including kerning) for a UTF-8 string.
    float text_width(Font* font, const char* utf8);

    // Walks a UTF-8 string, invoking `emit` once per glyph that has pixels.
    // `pen_x` is advanced across the whole string, so calling this with a
    // no-op emit yields exactly text_width().
    template <typename EmitFn>
    float walk_text(Font* font, const char* utf8, EmitFn&& emit);

    const Glyph& glyph(Font* font, uint32_t codepoint);

    // Kerning between two code points, in pixels, memoized.
    //
    // stbtt_GetCodepointKernAdvance is the single most expensive thing in text
    // measurement: it re-resolves both code points to glyph indices (two binary
    // searches through cmap) and re-walks the GPOS/kern table on every call.
    // Measured on Arial that is ~52 ns per adjacent character pair, against
    // ~8 ns for a glyph-cache hit -- i.e. roughly 85% of the per-character cost
    // of walking a string, and it is paid again on every draw.
    //
    // The cached value is the raw font-unit advance, so one entry serves every
    // pixel size of that typeface; the caller-visible result is that value
    // times font->scale, which is exactly the expression the uncached path
    // computes. See kern() for the uncached reference implementation.
    float kern_px(Font* font, uint32_t left, uint32_t right);

    size_t    kern_cache_size() const { return m_kern.size(); }
    long long kern_hits() const { return m_kern_hits; }
    long long kern_misses() const { return m_kern_misses; }
    void      reset_kern_stats() { m_kern_hits = m_kern_misses = 0; }

    Atlas&       atlas() { return m_atlas; }
    const Atlas& atlas() const { return m_atlas; }

    // Distinct glyphs currently packed in the atlas. Useful in tests to show
    // that re-parsing does not keep re-packing the same characters.
    size_t cached_glyph_count() const { return m_glyphs.size(); }

    size_t live_font_count() const { return m_fonts.size(); }

  private:
    Typeface* match(const char* family_list, int weight, bool italic);
    Typeface* best_in_family(const std::string& family, int weight, bool italic);

    std::vector<std::unique_ptr<Typeface>> m_faces;
    std::vector<std::unique_ptr<Font>>     m_fonts;

    // Pool of live fonts by description, so an identical create_font call
    // returns the existing one instead of allocating another.
    std::unordered_map<std::string, Font*> m_font_by_key;

    // Glyphs live here, not on Font: they must outlive the font objects that
    // happen to be alive right now, or every re-parse re-packs the same
    // characters into fresh atlas space until the atlas is exhausted and text
    // silently disappears.
    std::unordered_map<uint64_t, Glyph> m_glyphs;
    int                                 m_glyph_epoch = -1;

    // (face index, left code point, right code point) -> kerning in font units.
    //
    // Bounded rather than unbounded: a long-running game that streams in
    // unusual text must not grow this forever. The cap is a hard ceiling on
    // memory (~16k entries, a few hundred KB) and overflow clears the whole map
    // instead of doing LRU bookkeeping -- distinct character pairs saturate
    // within the first few frames for any fixed script, so a clear is a rare
    // one-off rewarm rather than a steady-state cost. Fonts with no kerning
    // table never insert at all.
    static constexpr size_t             kMaxKernEntries = 16384;
    std::unordered_map<uint64_t, int32_t> m_kern;
    long long                             m_kern_hits   = 0;
    long long                             m_kern_misses = 0;

    Atlas       m_atlas;
    std::string m_default_family;
};

// Decodes one UTF-8 code point. Returns the number of bytes consumed (>=1);
// invalid sequences yield U+FFFD and consume one byte.
int utf8_decode(const char* s, uint32_t* out_cp);

// Kerning between two code points, in pixels, computed from scratch every call.
//
// This is the reference implementation: FontManager::kern_px memoizes exactly
// this value and the harness asserts the two agree. walk_text goes through
// kern_px; this stays as the thing the cache is checked against.
float kern(Font* font, uint32_t left, uint32_t right);

template <typename EmitFn>
float FontManager::walk_text(Font* font, const char* utf8, EmitFn&& emit)
{
    if(!font || !utf8)
    {
        return 0.f;
    }

    float       pen  = 0.f;
    uint32_t    prev = 0;
    const char* p    = utf8;

    while(*p)
    {
        uint32_t cp = 0;
        p += utf8_decode(p, &cp);

        const Glyph& g = glyph(font, cp);

        if(prev)
        {
            pen += kern_px(font, prev, cp);
        }

        if(g.has_pixels)
        {
            emit(g, pen);
        }

        pen += g.advance;
        prev = cp;
    }

    return pen;
}

} // namespace lhu

#endif // LHU_FONT_H
