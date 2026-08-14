// LiteHtmlUnity — the document_container implementation.
//
// This class never rasterizes anything (except glyph coverage, which the font
// atlas handles). Every litehtml draw_* call is translated into one or more
// LhuQuad records that the GPU reconstructs analytically.

#ifndef LHU_CONTAINER_H
#define LHU_CONTAINER_H

#include "lhu_types.h"

#include <litehtml.h>

#include <string>
#include <vector>

namespace lhu
{

class FontManager;

// Rounded-rect clip, as pushed by litehtml's set_clip/del_clip.
struct ClipRect
{
    float x = 0, y = 0, w = -1, h = -1; // w < 0 means "no clip"
    float r[4] = {0, 0, 0, 0};
};

class Container : public litehtml::document_container
{
  public:
    Container(FontManager& fonts, const LhuHostCallbacks& host);
    ~Container() override = default;

    // --- recording lifecycle -------------------------------------------------

    void begin_record();
    void end_record();

    const std::vector<LhuQuad>& quads() const { return m_quads; }

    // --- retained display list support (experiment E2) ------------------------
    //
    // A cached quad run is spliced straight into the same output buffer the
    // C ABI hands back, so the host never sees that anything was reused. These
    // are the three things the cache needs and nothing else: where the current
    // run starts, how to append one, and the clip that was in force when a run
    // was captured (a run recorded under a different clip is not reusable,
    // because the clip is baked into every quad).

    size_t quad_count() const { return m_quads.size(); }

    void append_quads(const LhuQuad* src, size_t count)
    {
        m_quads.insert(m_quads.end(), src, src + count);
    }

    // Quads already in the buffer, writable, so the cache can rebase the
    // gradient LUT row indices of a spliced run.
    LhuQuad* quads_at(size_t index) { return m_quads.data() + index; }

    const ClipRect& active_clip_rect() const
    {
        static const ClipRect kNone;
        return m_clip_stack.empty() ? kNone : m_clip_stack.back();
    }

    int  grad_rows_recorded() const { return m_grad_rows; }
    void append_grad_rows(const uint8_t* rows, int count);

    // lhu_record() bumps this once per frame through begin_record(); the
    // whole-document fast path skips begin_record() entirely and has to bump it
    // by hand so the version the host sees is identical in both modes.
    void bump_grad_version() { ++m_grad_version; }

    const std::vector<uint8_t>& grad_lut() const { return m_grad_lut; }
    int                         grad_lut_rows() const { return m_grad_rows; }
    int                         grad_lut_version() const { return m_grad_version; }
    static constexpr int        kGradLutWidth = 256;

    void set_viewport(float w, float h)
    {
        m_viewport_w = w;
        m_viewport_h = h;
    }

    void set_device_scale(float s) { m_device_scale = s > 0.f ? s : 1.f; }

    void set_default_font(const std::string& family, float size)
    {
        m_default_font_family = family;
        m_default_font_size   = size;
    }

    const std::string& base_url() const { return m_base_url; }
    const std::string& caption() const { return m_caption; }
    const std::string& cursor() const { return m_cursor; }

    // --- document_container --------------------------------------------------

    litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document* doc,
                                   litehtml::font_metrics* fm) override;
    void               delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t  text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont, litehtml::web_color color,
                   const litehtml::position& pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char*       get_default_font_name() const override;

    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;

    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override;
    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer, const std::string& url,
                    const std::string& base_url) override;

    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                         const litehtml::web_color& color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::linear_gradient& gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::radial_gradient& gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                             const litehtml::background_layer::conic_gradient& gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos,
                      bool root) override;

    void set_caption(const char* caption) override;
    void set_base_url(const char* base_url) override;
    void link(const std::shared_ptr<litehtml::document>& doc, const litehtml::element::ptr& el) override;
    void on_anchor_click(const char* url, const litehtml::element::ptr& el) override;
    bool on_element_click(const litehtml::element::ptr& el) override;
    void on_mouse_event(const litehtml::element::ptr& el, litehtml::mouse_event event) override;
    void set_cursor(const char* cursor) override;
    void transform_text(std::string& text, litehtml::text_transform tt) override;
    void import_css(std::string& text, const std::string& url, std::string& baseurl) override;
    void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position& viewport) const override;
    litehtml::element::ptr create_element(const char* tag_name, const litehtml::string_map& attributes,
                                          const std::shared_ptr<litehtml::document>& doc) override;
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(std::string& language, std::string& culture) const override;

  private:
    LhuQuad& push_quad(int type);

    // Fills in geometry, radii and the currently active clip.
    void set_geometry(LhuQuad& q, const litehtml::position& pos, const litehtml::border_radiuses* radii);

    // Intersects the active clip with a background layer's clip_box.
    ClipRect effective_clip(const litehtml::position* extra) const;

    // Bakes a gradient's colour stops into a LUT row, returns the row index.
    int bake_gradient(const litehtml::background_layer::gradient_base& g);

    // The root element's background paints the whole canvas, not just its own
    // box -- the same rule browsers apply to <html>/<body>. litehtml signals
    // this with background_layer::is_root.
    litehtml::position paint_box(const litehtml::background_layer& layer) const;

    void emit_decoration_line(float x, float y, float w, float thickness, uint32_t color);

    FontManager&     m_fonts;
    LhuHostCallbacks m_host {};

    std::vector<LhuQuad> m_quads;
    std::vector<ClipRect> m_clip_stack;

    std::vector<uint8_t> m_grad_lut;
    int                  m_grad_rows    = 0;
    int                  m_grad_version = 0;

    std::string m_base_url;
    std::string m_caption;
    std::string m_cursor = "auto";
    std::string m_import_buffer;

    std::string m_default_font_family = "sans-serif";
    float       m_default_font_size   = 16.f;

    float m_viewport_w   = 800.f;
    float m_viewport_h   = 600.f;
    float m_device_scale = 1.f;
};

} // namespace lhu

#endif // LHU_CONTAINER_H
