#include "lhu_font.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lhu
{

namespace
{
constexpr int kAtlasInitial = 512;
constexpr int kAtlasMax     = 4096;
constexpr int kGlyphPad     = 1; // keeps bilinear sampling from bleeding

std::string lcase_trim(const std::string& in)
{
    size_t b = in.find_first_not_of(" \t\r\n'\"");
    if(b == std::string::npos)
    {
        return {};
    }
    size_t      e   = in.find_last_not_of(" \t\r\n'\"");
    std::string out = in.substr(b, e - b + 1);
    for(char& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
} // namespace

Typeface::~Typeface() = default;

//
// UTF-8
//

int utf8_decode(const char* s, uint32_t* out_cp)
{
    const auto* u = reinterpret_cast<const unsigned char*>(s);
    unsigned    c = u[0];

    if(c < 0x80)
    {
        *out_cp = c;
        return 1;
    }
    if((c & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80)
    {
        *out_cp = ((c & 0x1Fu) << 6) | (u[1] & 0x3Fu);
        return 2;
    }
    if((c & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80)
    {
        *out_cp = ((c & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
        return 3;
    }
    if((c & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80)
    {
        *out_cp = ((c & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
        return 4;
    }

    *out_cp = 0xFFFD;
    return 1;
}

//
// Atlas
//

Atlas::Atlas()
{
    m_width  = kAtlasInitial;
    m_height = kAtlasInitial;
    m_pixels.assign(static_cast<size_t>(m_width) * m_height, 0);
}

void Atlas::reset()
{
    std::fill(m_pixels.begin(), m_pixels.end(), 0);
    m_shelf_y = 0;
    m_shelf_h = 0;
    m_pen_x   = 0;
    ++m_epoch;
    ++m_version;
}

bool Atlas::grow()
{
    if(m_width >= kAtlasMax && m_height >= kAtlasMax)
    {
        return false;
    }

    // Grow the shorter axis first so the atlas stays roughly square.
    if(m_width <= m_height)
    {
        m_width = std::min(m_width * 2, kAtlasMax);
    }
    else
    {
        m_height = std::min(m_height * 2, kAtlasMax);
    }

    m_pixels.assign(static_cast<size_t>(m_width) * m_height, 0);
    m_shelf_y = 0;
    m_shelf_h = 0;
    m_pen_x   = 0;
    ++m_epoch;
    ++m_version;
    return true;
}

bool Atlas::alloc(int w, int h, int* out_x, int* out_y)
{
    const int aw = w + kGlyphPad * 2;
    const int ah = h + kGlyphPad * 2;

    bool tried_reset = false;

    for(;;)
    {
        if(aw <= m_width)
        {
            if(m_pen_x + aw > m_width)
            {
                // Close the current shelf and open a new one.
                m_shelf_y += m_shelf_h;
                m_shelf_h = 0;
                m_pen_x   = 0;
            }

            const int shelf_h = std::max(m_shelf_h, ah);
            if(m_shelf_y + shelf_h <= m_height)
            {
                *out_x    = m_pen_x + kGlyphPad;
                *out_y    = m_shelf_y + kGlyphPad;
                m_pen_x  += aw;
                m_shelf_h = shelf_h;
                return true;
            }
        }

        if(!grow())
        {
            // Already at the maximum size and out of room. Wiping the atlas and
            // starting over loses cached glyphs for a frame, but the
            // alternative is returning false forever, which makes text vanish
            // permanently.
            if(tried_reset)
            {
                return false;
            }

            tried_reset = true;
            reset();
        }
        // grow()/reset() rewound the packer; retry from scratch.
    }
}

void Atlas::blit(const uint8_t* src, int sw, int sh, int dx, int dy)
{
    for(int y = 0; y < sh; ++y)
    {
        std::memcpy(&m_pixels[static_cast<size_t>(dy + y) * m_width + dx], src + static_cast<size_t>(y) * sw, sw);
    }
    ++m_version;
}

//
// FontManager
//

FontManager::FontManager()  = default;
FontManager::~FontManager() = default;

bool FontManager::register_font(const char* family, int weight, bool italic, const uint8_t* data, size_t len)
{
    if(!family || !data || len == 0)
    {
        return false;
    }

    auto face    = std::make_unique<Typeface>();
    face->family = lcase_trim(family);
    face->weight = weight;
    face->italic = italic;
    face->data.assign(data, data + len);
    face->info = std::make_unique<stbtt_fontinfo>();

    const int offset = stbtt_GetFontOffsetForIndex(face->data.data(), 0);
    if(offset < 0 || !stbtt_InitFont(face->info.get(), face->data.data(), offset))
    {
        return false;
    }

    // stbtt itself short-circuits when neither table is present; recording it
    // here means such a face never even reaches the cache lookup.
    face->has_kerning = face->info->kern != 0 || face->info->gpos != 0;

    if(m_default_family.empty())
    {
        m_default_family = face->family;
    }

    m_faces.push_back(std::move(face));

    // Kerning is cached by face index. Registration only ever appends, so no
    // existing index is renumbered and the entries are still valid -- but this
    // runs a handful of times at startup and never in a frame, so clearing
    // removes the standing hazard that a future change to m_faces ordering
    // would silently return another typeface's kerning.
    m_kern.clear();

    return true;
}

void FontManager::set_default_family(const char* family)
{
    if(family)
    {
        m_default_family = lcase_trim(family);
    }
}

Typeface* FontManager::best_in_family(const std::string& family, int weight, bool italic)
{
    Typeface* best       = nullptr;
    int       best_score = 0;

    for(auto& f : m_faces)
    {
        if(f->family != family)
        {
            continue;
        }

        // Lower is better: weight distance dominates, style mismatch is a
        // fixed heavy penalty so we never pick italic over upright by accident.
        const int score = std::abs(f->weight - weight) + (f->italic == italic ? 0 : 1000);
        if(!best || score < best_score)
        {
            best       = f.get();
            best_score = score;
        }
    }

    return best;
}

Typeface* FontManager::match(const char* family_list, int weight, bool italic)
{
    if(family_list)
    {
        std::string        list(family_list);
        std::string::size_type start = 0;

        while(start <= list.size())
        {
            const std::string::size_type comma = list.find(',', start);
            const std::string            name  = lcase_trim(
                list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));

            if(!name.empty())
            {
                if(Typeface* f = best_in_family(name, weight, italic))
                {
                    return f;
                }
            }

            if(comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
    }

    if(Typeface* f = best_in_family(m_default_family, weight, italic))
    {
        return f;
    }

    return m_faces.empty() ? nullptr : m_faces.front().get();
}

Font* FontManager::create_font(const char* family_list, float size, int weight, bool italic, int decoration_line,
                               int decoration_style, uint32_t decoration_color, float decoration_thickness)
{
    Typeface* face = match(family_list, weight, italic);
    if(!face)
    {
        return nullptr;
    }

    size_t face_index = 0;
    for(size_t i = 0; i < m_faces.size(); ++i)
    {
        if(m_faces[i].get() == face)
        {
            face_index = i;
            break;
        }
    }

    // Everything that makes two fonts behave differently goes in the key.
    const std::string key = std::to_string(face_index) + ":" + std::to_string(size) + ":" +
                            std::to_string(decoration_line) + ":" + std::to_string(decoration_style) + ":" +
                            std::to_string(decoration_color) + ":" + std::to_string(decoration_thickness);

    auto pooled = m_font_by_key.find(key);
    if(pooled != m_font_by_key.end())
    {
        ++pooled->second->refs;
        return pooled->second;
    }

    auto font       = std::make_unique<Font>();
    font->face       = face;
    font->key        = key;
    font->refs       = 1;
    font->face_index = static_cast<uint32_t>(face_index);

    // Glyph rasterization depends only on typeface and pixel size, so the cache
    // key ignores decoration. Size is quantized to quarter-pixels.
    const uint64_t size_q = static_cast<uint64_t>(
        std::min(65535.f, std::max(0.f, std::round(size * 4.f))));
    font->glyph_key_base = (static_cast<uint64_t>(face_index & 0xFFFF) << 48) | (size_q << 32);
    font->size  = size;
    // CSS font-size is the em size, so map em -> pixels (not ascent+descent).
    font->scale = stbtt_ScaleForMappingEmToPixels(face->info.get(), size);

    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(face->info.get(), &asc, &desc, &gap);
    font->ascent  = asc * font->scale;
    font->descent = -desc * font->scale; // stb reports descent as negative
    font->height  = font->ascent + font->descent;

    // x-height from the actual 'x' outline, with a sane fallback for fonts
    // that do not contain one.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if(stbtt_GetCodepointBox(face->info.get(), 'x', &x0, &y0, &x1, &y1) && y1 > y0)
    {
        font->x_height = (y1 - y0) * font->scale;
    }
    else
    {
        font->x_height = font->height * 0.5f;
    }

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(face->info.get(), '0', &adv, &lsb);
    font->ch_width = adv * font->scale;

    font->decoration_line  = decoration_line;
    font->decoration_style = decoration_style;
    font->decoration_color = decoration_color;
    font->decoration_thickness =
        decoration_thickness > 0.f ? decoration_thickness : std::max(1.f, std::round(size / 14.f));
    font->underline_offset = std::max(1.f, std::round(size / 12.f));

    Font* raw = font.get();
    m_fonts.push_back(std::move(font));
    m_font_by_key.emplace(key, raw);
    return raw;
}

void FontManager::destroy_font(Font* font)
{
    if(!font || --font->refs > 0)
    {
        return;
    }

    m_font_by_key.erase(font->key);

    // Cached glyphs deliberately stay behind: the next parse will ask for the
    // same typeface and size, and re-packing them is what exhausts the atlas.
    m_fonts.erase(std::remove_if(m_fonts.begin(), m_fonts.end(),
                                 [font](const std::unique_ptr<Font>& f) { return f.get() == font; }),
                  m_fonts.end());
}

float kern(Font* font, uint32_t left, uint32_t right)
{
    if(!font || !font->face)
    {
        return 0.f;
    }
    return stbtt_GetCodepointKernAdvance(font->face->info.get(), static_cast<int>(left), static_cast<int>(right)) *
           font->scale;
}

float FontManager::kern_px(Font* font, uint32_t left, uint32_t right)
{
    if(!font || !font->face || !font->face->has_kerning)
    {
        return 0.f;
    }

    // 16 bits of face index, 21 bits per code point -- enough for the whole of
    // Unicode, and 58 bits in total, so nothing is truncated and two different
    // pairs can never fold onto the same key.
    const uint64_t key = (static_cast<uint64_t>(font->face_index & 0xFFFFu) << 42) |
                         (static_cast<uint64_t>(left & 0x1FFFFFu) << 21) |
                         static_cast<uint64_t>(right & 0x1FFFFFu);

    auto it = m_kern.find(key);
    if(it != m_kern.end())
    {
        ++m_kern_hits;
        // Font units -> pixels. The size lives here, not in the key, which is
        // why the same entry serves 13 px and 26 px without colliding.
        return static_cast<float>(it->second) * font->scale;
    }

    if(m_kern.size() >= kMaxKernEntries)
    {
        m_kern.clear();
    }

    const int units = stbtt_GetCodepointKernAdvance(font->face->info.get(), static_cast<int>(left),
                                                    static_cast<int>(right));
    m_kern.emplace(key, units);
    ++m_kern_misses;

    return static_cast<float>(units) * font->scale;
}

const Glyph& FontManager::glyph(Font* font, uint32_t codepoint)
{
    static const Glyph kEmpty {};

    if(!font || !font->face)
    {
        return kEmpty;
    }

    // The atlas was reset or resized since these glyphs were packed, so every
    // cached UV is stale.
    if(m_glyph_epoch != m_atlas.epoch())
    {
        m_glyphs.clear();
        m_glyph_epoch = m_atlas.epoch();
    }

    const uint64_t key = font->glyph_key_base | codepoint;

    auto it = m_glyphs.find(key);
    if(it != m_glyphs.end())
    {
        return it->second;
    }

    stbtt_fontinfo* info = font->face->info.get();

    Glyph g {};
    int   adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info, static_cast<int>(codepoint), &adv, &lsb);
    g.advance = adv * font->scale;

    int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
    stbtt_GetCodepointBitmapBox(info, static_cast<int>(codepoint), font->scale, font->scale, &gx0, &gy0, &gx1, &gy1);

    const int gw = gx1 - gx0;
    const int gh = gy1 - gy0;

    if(gw > 0 && gh > 0)
    {
        std::vector<uint8_t> bitmap(static_cast<size_t>(gw) * gh);
        stbtt_MakeCodepointBitmap(info, bitmap.data(), gw, gh, gw, font->scale, font->scale,
                                  static_cast<int>(codepoint));

        int ax = 0, ay = 0;
        if(m_atlas.alloc(gw, gh, &ax, &ay))
        {
            // alloc() may have grown or reset the atlas, which invalidates every
            // other cached glyph and changes the dimensions the UVs divide by.
            if(m_glyph_epoch != m_atlas.epoch())
            {
                m_glyphs.clear();
                m_glyph_epoch = m_atlas.epoch();
            }

            m_atlas.blit(bitmap.data(), gw, gh, ax, ay);

            const float aw = static_cast<float>(m_atlas.width());
            const float ah = static_cast<float>(m_atlas.height());

            g.u0 = ax / aw;
            g.v0 = ay / ah;
            g.u1 = (ax + gw) / aw;
            g.v1 = (ay + gh) / ah;
            // gy0 is relative to the baseline and grows downward in stb's
            // bitmap space, which is exactly our quad space.
            g.xoff       = static_cast<float>(gx0);
            g.yoff       = static_cast<float>(gy0);
            g.w          = static_cast<float>(gw);
            g.h          = static_cast<float>(gh);
            g.has_pixels = true;
        }
    }

    return m_glyphs.emplace(key, g).first->second;
}

float FontManager::text_width(Font* font, const char* utf8)
{
    return walk_text(font, utf8, [](const Glyph&, float) {});
}

} // namespace lhu
