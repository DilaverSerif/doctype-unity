#include "lhu_container.h"
#include "lhu_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lhu
{

namespace
{

inline uint32_t pack_color(const litehtml::web_color& c)
{
    return static_cast<uint32_t>(c.red) | (static_cast<uint32_t>(c.green) << 8) |
           (static_cast<uint32_t>(c.blue) << 16) | (static_cast<uint32_t>(c.alpha) << 24);
}

inline bool visible(const litehtml::web_color& c)
{
    return c.alpha != 0;
}

// Intersects two rects. A negative width marks "unset" and is treated as
// "covers everything".
inline void intersect(float& x, float& y, float& w, float& h, float ox, float oy, float ow, float oh)
{
    if(w < 0.f)
    {
        x = ox;
        y = oy;
        w = ow;
        h = oh;
        return;
    }

    const float x0 = std::max(x, ox);
    const float y0 = std::max(y, oy);
    const float x1 = std::min(x + w, ox + ow);
    const float y1 = std::min(y + h, oy + oh);

    x = x0;
    y = y0;
    w = std::max(0.f, x1 - x0);
    h = std::max(0.f, y1 - y0);
}

std::string roman_numeral(int value, bool upper)
{
    static const int         kVals[]  = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* const kRoman[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};

    std::string out;
    if(value <= 0)
    {
        return "0";
    }

    for(int i = 0; i < 13; ++i)
    {
        while(value >= kVals[i])
        {
            out   += kRoman[i];
            value -= kVals[i];
        }
    }

    if(upper)
    {
        for(char& c : out)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

std::string alpha_numeral(int value, bool upper)
{
    std::string out;
    const char  base = upper ? 'A' : 'a';

    while(value > 0)
    {
        --value;
        out.insert(out.begin(), static_cast<char>(base + value % 26));
        value /= 26;
    }
    return out.empty() ? "0" : out;
}

} // namespace

Container::Container(FontManager& fonts, const LhuHostCallbacks& host) :
    m_fonts(fonts),
    m_host(host)
{
}

//
// Recording
//

void Container::begin_record()
{
    m_quads.clear();
    m_clip_stack.clear();
    m_grad_lut.clear();
    m_grad_rows = 0;
    ++m_grad_version;
}

void Container::end_record()
{
}

// Re-appends the LUT rows a cached quad run baked when it was recorded. Bumps
// the version once per row, exactly as bake_gradient() would have, so
// grad_lut_version is the same number in both modes.
void Container::append_grad_rows(const uint8_t* rows, int count)
{
    if(!rows || count <= 0)
    {
        return;
    }

    const size_t bytes = static_cast<size_t>(count) * kGradLutWidth * 4;
    m_grad_rows       += count;
    m_grad_lut.resize(static_cast<size_t>(m_grad_rows) * kGradLutWidth * 4, 0);
    std::memcpy(m_grad_lut.data() + m_grad_lut.size() - bytes, rows, bytes);
    m_grad_version += count;
}

LhuQuad& Container::push_quad(int type)
{
    m_quads.emplace_back();
    LhuQuad& q = m_quads.back();
    std::memset(&q, 0, sizeof(q));
    q.type     = type;
    q.grad_row = -1;
    q.clip_w   = -1.f;
    q.clip_h   = -1.f;
    return q;
}

ClipRect Container::effective_clip(const litehtml::position* extra) const
{
    ClipRect out;

    if(!m_clip_stack.empty())
    {
        out = m_clip_stack.back();
    }

    if(extra)
    {
        intersect(out.x, out.y, out.w, out.h, static_cast<float>(extra->x), static_cast<float>(extra->y),
                  static_cast<float>(extra->width), static_cast<float>(extra->height));
    }

    return out;
}

void Container::set_geometry(LhuQuad& q, const litehtml::position& pos, const litehtml::border_radiuses* radii)
{
    q.x = static_cast<float>(pos.x);
    q.y = static_cast<float>(pos.y);
    q.w = static_cast<float>(pos.width);
    q.h = static_cast<float>(pos.height);

    if(radii)
    {
        q.rx[0] = static_cast<float>(radii->top_left_x);
        q.ry[0] = static_cast<float>(radii->top_left_y);
        q.rx[1] = static_cast<float>(radii->top_right_x);
        q.ry[1] = static_cast<float>(radii->top_right_y);
        q.rx[2] = static_cast<float>(radii->bottom_right_x);
        q.ry[2] = static_cast<float>(radii->bottom_right_y);
        q.rx[3] = static_cast<float>(radii->bottom_left_x);
        q.ry[3] = static_cast<float>(radii->bottom_left_y);
    }

    const ClipRect clip = effective_clip(nullptr);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

//
// Fonts
//

litehtml::uint_ptr Container::create_font(const litehtml::font_description& descr, const litehtml::document* /*doc*/,
                                          litehtml::font_metrics* fm)
{
    Font* font = m_fonts.create_font(descr.family.c_str(), static_cast<float>(descr.size), descr.weight,
                                     descr.style == litehtml::font_style_italic, descr.decoration_line,
                                     static_cast<int>(descr.decoration_style), pack_color(descr.decoration_color),
                                     descr.decoration_thickness.is_predefined()
                                         ? 0.f
                                         : static_cast<float>(descr.decoration_thickness.val()));

    if(!font)
    {
        // No fonts registered at all: report something sane so layout does not
        // divide by zero, and draw nothing.
        if(fm)
        {
            const float size = static_cast<float>(descr.size);
            fm->font_size    = size;
            fm->ascent       = size * 0.8f;
            fm->descent      = size * 0.2f;
            fm->height       = size;
            fm->x_height     = size * 0.5f;
            fm->ch_width     = size * 0.5f;
            fm->draw_spaces  = false;
            fm->sub_shift    = size / 5.f;
            fm->super_shift  = size / 3.f;
        }
        return 0;
    }

    if(fm)
    {
        fm->font_size = font->size;
        fm->ascent    = font->ascent;
        fm->descent   = font->descent;
        fm->height    = font->height;
        fm->x_height  = font->x_height;
        fm->ch_width  = font->ch_width;
        // Spaces only need a draw call when there is a decoration line to paint
        // across them.
        fm->draw_spaces = descr.decoration_line != litehtml::text_decoration_line_none;
        fm->sub_shift   = font->size / 5.f;
        fm->super_shift = font->size / 3.f;
    }

    return reinterpret_cast<litehtml::uint_ptr>(font);
}

void Container::delete_font(litehtml::uint_ptr hFont)
{
    if(hFont)
    {
        m_fonts.destroy_font(reinterpret_cast<Font*>(hFont));
    }
}

// NOT a layout hot path, despite the name suggesting otherwise.
//
// litehtml measures each text run exactly once, in el_text::compute_styles,
// and stores the result in el_text::m_size; get_content_size then just returns
// it. So every text_width call happens while the *document is being built*, and
// render() makes none at all. Measured with tests/probe_text.cpp on the four
// bench pages: calls during layout 0/0/0/0, calls during draw 0/0/0/0, calls
// during document creation 12/32/160/600 -- and even there they are only 1-3%
// of creation time.
//
// A (string, font) -> width cache in front of this therefore cannot move layout
// at all, and there is deliberately none. The measurement cost that *is* worth
// caching sits one level down, in the per-character kerning lookup that both
// this and draw_text share; see FontManager::kern_px.
litehtml::pixel_t Container::text_width(const char* text, litehtml::uint_ptr hFont)
{
    if(!hFont)
    {
        return litehtml::pixel_t(0.f);
    }
    return litehtml::pixel_t(m_fonts.text_width(reinterpret_cast<Font*>(hFont), text));
}

void Container::emit_decoration_line(float x, float y, float w, float thickness, uint32_t color)
{
    LhuQuad& q = push_quad(LHU_QUAD_RECT);
    q.x        = x;
    q.y        = std::round(y);
    q.w        = w;
    q.h        = std::max(1.f, thickness);
    q.color    = color;

    const ClipRect clip = effective_clip(nullptr);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

void Container::draw_text(litehtml::uint_ptr /*hdc*/, const char* text, litehtml::uint_ptr hFont,
                          litehtml::web_color color, const litehtml::position& pos)
{
    if(!hFont || !text || !visible(color))
    {
        return;
    }

    Font* font = reinterpret_cast<Font*>(hFont);

    // litehtml hands us the text box, not the baseline. Matching the reference
    // containers: baseline sits `descent` above the bottom of that box.
    const float box_x    = static_cast<float>(pos.x);
    const float box_y    = static_cast<float>(pos.y);
    const float baseline = box_y + static_cast<float>(pos.height) - font->descent;

    const ClipRect clip     = effective_clip(nullptr);
    const uint32_t tint     = pack_color(color);
    float          text_len = 0.f;

    text_len = m_fonts.walk_text(font, text, [&](const Glyph& g, float pen) {
        LhuQuad& q = push_quad(LHU_QUAD_GLYPH);
        // Rounding to whole pixels keeps small UI text crisp. The pen itself
        // stays fractional, so this never accumulates drift.
        q.x     = std::round(box_x + pen + g.xoff);
        q.y     = std::round(baseline + g.yoff);
        q.w     = g.w;
        q.h     = g.h;
        q.u0    = g.u0;
        q.v0    = g.v0;
        q.u1    = g.u1;
        q.v1    = g.v1;
        q.color = tint;

        q.clip_x = clip.x;
        q.clip_y = clip.y;
        q.clip_w = clip.w;
        q.clip_h = clip.h;
        std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
    });

    if(font->decoration_line == litehtml::text_decoration_line_none || text_len <= 0.f)
    {
        return;
    }

    // Alpha 0 in the description means "use the text colour".
    const uint32_t deco = (font->decoration_color >> 24) == 0 ? tint : font->decoration_color;
    const float    th   = font->decoration_thickness;

    if(font->decoration_line & litehtml::text_decoration_line_underline)
    {
        emit_decoration_line(box_x, baseline + font->underline_offset, text_len, th, deco);
    }
    if(font->decoration_line & litehtml::text_decoration_line_overline)
    {
        emit_decoration_line(box_x, baseline - font->ascent, text_len, th, deco);
    }
    if(font->decoration_line & litehtml::text_decoration_line_line_through)
    {
        emit_decoration_line(box_x, baseline - font->x_height * 0.5f, text_len, th, deco);
    }
}

litehtml::pixel_t Container::pt_to_px(float pt) const
{
    return litehtml::pixel_t(pt * 96.f / 72.f);
}

litehtml::pixel_t Container::get_default_font_size() const
{
    return litehtml::pixel_t(m_default_font_size);
}

const char* Container::get_default_font_name() const
{
    return m_default_font_family.c_str();
}

//
// List markers
//

void Container::draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker)
{
    if(!marker.image.empty())
    {
        litehtml::background_layer layer;
        layer.border_box = marker.pos;
        layer.clip_box   = marker.pos;
        layer.origin_box = marker.pos;
        draw_image(hdc, layer, marker.image, marker.baseurl ? marker.baseurl : std::string());
        return;
    }

    if(!visible(marker.color))
    {
        return;
    }

    const uint32_t color = pack_color(marker.color);

    switch(marker.marker_type)
    {
    case litehtml::list_style_type_disc:
    case litehtml::list_style_type_circle:
    {
        // A "circle" is the hollow variant; we approximate it with a ring made
        // of a fully rounded border.
        LhuQuad& q = push_quad(marker.marker_type == litehtml::list_style_type_disc ? LHU_QUAD_RECT : LHU_QUAD_BORDER);
        set_geometry(q, marker.pos, nullptr);
        const float rx = q.w * 0.5f;
        const float ry = q.h * 0.5f;
        for(int i = 0; i < 4; ++i)
        {
            q.rx[i] = rx;
            q.ry[i] = ry;
        }
        q.color = color;
        if(q.type == LHU_QUAD_BORDER)
        {
            const float th = std::max(1.f, q.w * 0.15f);
            for(int i = 0; i < 4; ++i)
            {
                q.border[i] = th;
            }
            // A ring is symmetric, so a single quad covering all four edges is
            // enough — draw it as the "top" edge with the wedge test disabled.
            q.params[0] = -1.f;
        }
        break;
    }

    case litehtml::list_style_type_square:
    {
        LhuQuad& q = push_quad(LHU_QUAD_RECT);
        set_geometry(q, marker.pos, nullptr);
        q.color = color;
        break;
    }

    default:
    {
        if(!marker.font)
        {
            break;
        }

        std::string label;
        switch(marker.marker_type)
        {
        case litehtml::list_style_type_lower_roman: label = roman_numeral(marker.index, false) + "."; break;
        case litehtml::list_style_type_upper_roman: label = roman_numeral(marker.index, true) + "."; break;
        case litehtml::list_style_type_lower_alpha:
        case litehtml::list_style_type_lower_latin: label = alpha_numeral(marker.index, false) + "."; break;
        case litehtml::list_style_type_upper_alpha:
        case litehtml::list_style_type_upper_latin: label = alpha_numeral(marker.index, true) + "."; break;
        case litehtml::list_style_type_none: return;
        default: label = std::to_string(marker.index) + "."; break;
        }

        // Markers are right-aligned against the content box.
        Font*             font = reinterpret_cast<Font*>(marker.font);
        const float       w    = m_fonts.text_width(font, label.c_str());
        litehtml::position pos = marker.pos;
        pos.x                  = pos.x + pos.width - litehtml::pixel_t(w);
        pos.width              = litehtml::pixel_t(w);
        pos.height             = litehtml::pixel_t(font->height);

        draw_text(hdc, label.c_str(), marker.font, marker.color, pos);
        break;
    }
    }
}

//
// Images
//

void Container::load_image(const char* src, const char* baseurl, bool /*redraw_on_ready*/)
{
    if(m_host.load_image && src)
    {
        m_host.load_image(m_host.user_data, src);
    }
    (void) baseurl;
}

void Container::get_image_size(const char* src, const char* /*baseurl*/, litehtml::size& sz)
{
    sz.width  = litehtml::pixel_t(0.f);
    sz.height = litehtml::pixel_t(0.f);

    if(!m_host.get_image_size || !src)
    {
        return;
    }

    int32_t w = 0, h = 0;
    m_host.get_image_size(m_host.user_data, src, &w, &h);
    sz.width  = litehtml::pixel_t(static_cast<float>(w));
    sz.height = litehtml::pixel_t(static_cast<float>(h));
}

void Container::draw_image(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer, const std::string& url,
                           const std::string& /*base_url*/)
{
    if(!m_host.get_image_uv || url.empty())
    {
        return;
    }

    float uv[4] = {0, 0, 1, 1};
    if(!m_host.get_image_uv(m_host.user_data, url.c_str(), uv))
    {
        return;
    }

    LhuQuad& q = push_quad(LHU_QUAD_IMAGE);
    set_geometry(q, layer.origin_box, &layer.border_radius);
    q.u0    = uv[0];
    q.v0    = uv[1];
    q.u1    = uv[2];
    q.v1    = uv[3];
    q.color = 0xFFFFFFFFu;

    // The origin box can extend past the painting area (background-position,
    // background-size); clip_box is what actually gets painted.
    const ClipRect clip = effective_clip(&layer.clip_box);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

//
// Backgrounds
//

void Container::draw_solid_fill(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                const litehtml::web_color& color)
{
    if(!visible(color))
    {
        return;
    }

    LhuQuad& q = push_quad(LHU_QUAD_RECT);
    set_geometry(q, paint_box(layer), &layer.border_radius);
    q.color = pack_color(color);

    const ClipRect clip = layer.is_root ? effective_clip(nullptr) : effective_clip(&layer.clip_box);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

litehtml::position Container::paint_box(const litehtml::background_layer& layer) const
{
    if(!layer.is_root)
    {
        return layer.border_box;
    }

    litehtml::position box = layer.border_box;
    box.x      = litehtml::pixel_t(0.f);
    box.y      = litehtml::pixel_t(0.f);
    box.width  = litehtml::pixel_t(std::max(static_cast<float>(box.width), m_viewport_w));
    box.height = litehtml::pixel_t(std::max(static_cast<float>(box.height), m_viewport_h));
    return box;
}

int Container::bake_gradient(const litehtml::background_layer::gradient_base& g)
{
    if(g.color_points.empty())
    {
        return -1;
    }

    const int row = m_grad_rows++;
    m_grad_lut.resize(static_cast<size_t>(m_grad_rows) * kGradLutWidth * 4, 0);
    ++m_grad_version;

    uint8_t* dst = m_grad_lut.data() + static_cast<size_t>(row) * kGradLutWidth * 4;

    for(int i = 0; i < kGradLutWidth; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kGradLutWidth - 1);

        // Find the segment [a, b] that contains t.
        const litehtml::background_layer::color_point* a = &g.color_points.front();
        const litehtml::background_layer::color_point* b = &g.color_points.back();

        for(size_t k = 0; k + 1 < g.color_points.size(); ++k)
        {
            if(t >= g.color_points[k].offset && t <= g.color_points[k + 1].offset)
            {
                a = &g.color_points[k];
                b = &g.color_points[k + 1];
                break;
            }
        }

        float f = 0.f;
        if(b->offset > a->offset)
        {
            f = (t - a->offset) / (b->offset - a->offset);
            f = std::min(1.f, std::max(0.f, f));

            // A colour hint shifts the midpoint of the transition.
            if(a->hint.has_value())
            {
                const float hint = (*a->hint - a->offset) / (b->offset - a->offset);
                if(hint > 0.f && hint < 1.f)
                {
                    f = std::pow(f, std::log(0.5f) / std::log(hint));
                }
            }
        }
        else if(t >= b->offset)
        {
            f = 1.f;
        }

        dst[i * 4 + 0] = static_cast<uint8_t>(std::lround(a->color.red + (b->color.red - a->color.red) * f));
        dst[i * 4 + 1] = static_cast<uint8_t>(std::lround(a->color.green + (b->color.green - a->color.green) * f));
        dst[i * 4 + 2] = static_cast<uint8_t>(std::lround(a->color.blue + (b->color.blue - a->color.blue) * f));
        dst[i * 4 + 3] = static_cast<uint8_t>(std::lround(a->color.alpha + (b->color.alpha - a->color.alpha) * f));
    }

    return row;
}

void Container::draw_linear_gradient(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                     const litehtml::background_layer::linear_gradient& gradient)
{
    const int row = bake_gradient(gradient);
    if(row < 0)
    {
        return;
    }

    LhuQuad& q = push_quad(LHU_QUAD_LINEAR_GRAD);
    set_geometry(q, paint_box(layer), &layer.border_radius);
    q.grad_row  = row;
    q.color     = 0xFFFFFFFFu;
    q.params[0] = static_cast<float>(gradient.start.x);
    q.params[1] = static_cast<float>(gradient.start.y);
    q.params[2] = static_cast<float>(gradient.end.x);
    q.params[3] = static_cast<float>(gradient.end.y);

    const ClipRect clip = layer.is_root ? effective_clip(nullptr) : effective_clip(&layer.clip_box);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

void Container::draw_radial_gradient(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                     const litehtml::background_layer::radial_gradient& gradient)
{
    const int row = bake_gradient(gradient);
    if(row < 0)
    {
        return;
    }

    LhuQuad& q = push_quad(LHU_QUAD_RADIAL_GRAD);
    set_geometry(q, paint_box(layer), &layer.border_radius);
    q.grad_row  = row;
    q.color     = 0xFFFFFFFFu;
    // litehtml only fills radius.x for the `circle <length>` form and leaves
    // radius.y at zero. Feeding that to the shader divides by ~0 and pins every
    // pixel to the far end of the ramp, so mirror the missing axis: a circle is
    // exactly the case where both radii are equal.
    float radius_x = static_cast<float>(gradient.radius.x);
    float radius_y = static_cast<float>(gradient.radius.y);

    if(radius_x <= 0.f)
    {
        radius_x = radius_y;
    }
    if(radius_y <= 0.f)
    {
        radius_y = radius_x;
    }
    if(radius_x <= 0.f)
    {
        // Fully degenerate: paint the first stop rather than divide by zero.
        radius_x = radius_y = 1.f;
    }

    q.params[0] = static_cast<float>(gradient.position.x);
    q.params[1] = static_cast<float>(gradient.position.y);
    q.params[2] = radius_x;
    q.params[3] = radius_y;

    const ClipRect clip = effective_clip(&layer.clip_box);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

void Container::draw_conic_gradient(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                    const litehtml::background_layer::conic_gradient& gradient)
{
    const int row = bake_gradient(gradient);
    if(row < 0)
    {
        return;
    }

    LhuQuad& q = push_quad(LHU_QUAD_CONIC_GRAD);
    set_geometry(q, paint_box(layer), &layer.border_radius);
    q.grad_row  = row;
    q.color     = 0xFFFFFFFFu;
    q.params[0] = static_cast<float>(gradient.position.x);
    q.params[1] = static_cast<float>(gradient.position.y);
    q.params[2] = gradient.angle;
    q.params[3] = gradient.radius;

    const ClipRect clip = layer.is_root ? effective_clip(nullptr) : effective_clip(&layer.clip_box);
    q.clip_x            = clip.x;
    q.clip_y            = clip.y;
    q.clip_w            = clip.w;
    q.clip_h            = clip.h;
    std::memcpy(q.clip_r, clip.r, sizeof(clip.r));
}

//
// Borders
//

void Container::draw_borders(litehtml::uint_ptr /*hdc*/, const litehtml::borders& borders,
                             const litehtml::position& draw_pos, bool /*root*/)
{
    const litehtml::border* edges[4] = {&borders.top, &borders.right, &borders.bottom, &borders.left};

    // One quad per visible edge. The shader carves each edge out of the ring
    // using the corner-to-corner miter diagonals, which is how the reference
    // containers split the border into four trapezoids.
    for(int i = 0; i < 4; ++i)
    {
        const litehtml::border& e = *edges[i];

        if(e.width <= litehtml::pixel_t(0.f) || e.style == litehtml::border_style_none ||
           e.style == litehtml::border_style_hidden || !visible(e.color))
        {
            continue;
        }

        LhuQuad& q = push_quad(LHU_QUAD_BORDER);
        set_geometry(q, draw_pos, &borders.radius);

        q.border[0] = static_cast<float>(borders.left.width);
        q.border[1] = static_cast<float>(borders.top.width);
        q.border[2] = static_cast<float>(borders.right.width);
        q.border[3] = static_cast<float>(borders.bottom.width);

        q.color     = pack_color(e.color);
        q.params[0] = static_cast<float>(i);
    }
}

//
// Misc document_container plumbing
//

void Container::set_caption(const char* caption)
{
    m_caption = caption ? caption : "";
}

void Container::set_base_url(const char* base_url)
{
    m_base_url = base_url ? base_url : "";
}

void Container::link(const std::shared_ptr<litehtml::document>& /*doc*/, const litehtml::element::ptr& /*el*/)
{
}

void Container::on_anchor_click(const char* url, const litehtml::element::ptr& /*el*/)
{
    if(m_host.on_anchor_click && url)
    {
        m_host.on_anchor_click(m_host.user_data, url);
    }
}

bool Container::on_element_click(const litehtml::element::ptr& el)
{
    if(!m_host.on_element_click || !el)
    {
        return false;
    }

    // Returning false lets litehtml walk up to the parent, which is what makes
    // a click on the text inside a button still reach the button.
    const char* tag = el->get_tagName();

    return m_host.on_element_click(m_host.user_data, el->get_attr("id", ""), tag ? tag : "",
                                   el->get_attr("class", ""), el->get_attr("data-action", "")) != 0;
}

void Container::on_mouse_event(const litehtml::element::ptr& /*el*/, litehtml::mouse_event /*event*/)
{
}

void Container::set_cursor(const char* cursor)
{
    const std::string next = cursor ? cursor : "auto";
    if(next == m_cursor)
    {
        return;
    }

    m_cursor = next;
    if(m_host.on_set_cursor)
    {
        m_host.on_set_cursor(m_host.user_data, m_cursor.c_str());
    }
}

void Container::transform_text(std::string& text, litehtml::text_transform tt)
{
    // ASCII-only. Locale-sensitive casing (Turkish i/I, for example) needs a
    // full Unicode case table and is deliberately out of scope here.
    switch(tt)
    {
    case litehtml::text_transform_uppercase:
        for(char& c : text)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        break;

    case litehtml::text_transform_lowercase:
        for(char& c : text)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        break;

    case litehtml::text_transform_capitalize:
    {
        bool at_word_start = true;
        for(char& c : text)
        {
            if(at_word_start)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            at_word_start = std::isspace(static_cast<unsigned char>(c)) != 0;
        }
        break;
    }

    case litehtml::text_transform_none:
    default: break;
    }
}

void Container::import_css(std::string& text, const std::string& url, std::string& baseurl)
{
    text.clear();

    if(!m_host.import_css)
    {
        return;
    }

    const char* css = m_host.import_css(m_host.user_data, url.c_str(), baseurl.c_str());
    if(css)
    {
        text = css;
    }
}

void Container::set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius)
{
    ClipRect clip;

    if(!m_clip_stack.empty())
    {
        clip = m_clip_stack.back();
    }

    intersect(clip.x, clip.y, clip.w, clip.h, static_cast<float>(pos.x), static_cast<float>(pos.y),
              static_cast<float>(pos.width), static_cast<float>(pos.height));

    // Nested rounded clips would need a per-corner intersection of two rounded
    // rects; the innermost radii are a close enough approximation in practice.
    const float radii[4] = {static_cast<float>(bdr_radius.top_left_x), static_cast<float>(bdr_radius.top_right_x),
                            static_cast<float>(bdr_radius.bottom_right_x),
                            static_cast<float>(bdr_radius.bottom_left_x)};

    if(radii[0] > 0.f || radii[1] > 0.f || radii[2] > 0.f || radii[3] > 0.f)
    {
        std::memcpy(clip.r, radii, sizeof(radii));
    }

    m_clip_stack.push_back(clip);
}

void Container::del_clip()
{
    if(!m_clip_stack.empty())
    {
        m_clip_stack.pop_back();
    }
}

void Container::get_viewport(litehtml::position& viewport) const
{
    viewport.x      = litehtml::pixel_t(0.f);
    viewport.y      = litehtml::pixel_t(0.f);
    viewport.width  = litehtml::pixel_t(m_viewport_w);
    viewport.height = litehtml::pixel_t(m_viewport_h);
}

litehtml::element::ptr Container::create_element(const char* /*tag_name*/, const litehtml::string_map& /*attributes*/,
                                                 const std::shared_ptr<litehtml::document>& /*doc*/)
{
    return nullptr;
}

void Container::get_media_features(litehtml::media_features& media) const
{
    media.type          = litehtml::media_type_screen;
    media.width         = litehtml::pixel_t(m_viewport_w);
    media.height        = litehtml::pixel_t(m_viewport_h);
    media.device_width  = litehtml::pixel_t(m_viewport_w);
    media.device_height = litehtml::pixel_t(m_viewport_h);
    media.color         = 8;
    media.monochrome    = 0;
    media.color_index   = 256;
    media.resolution    = litehtml::pixel_t(96.f * m_device_scale);
}

void Container::get_language(std::string& language, std::string& culture) const
{
    language = "en";
    culture.clear();
}

} // namespace lhu
