#ifndef LITEHTML_DOCUMENT_CONTAINER_H
#define LITEHTML_DOCUMENT_CONTAINER_H

#include "types.h"
#include "web_color.h"
#include "background.h"
#include "borders.h"
#include "element.h"
#include "font_description.h"
#include <memory>
#include <functional>
// LHU PATCH (experiment E6, inline-style cache): needed by m_inline_style_cache below.
#include <map>
#include <string>

namespace litehtml
{
    // LHU PATCH (experiment E6): only referenced through shared_ptr/pointer here, so the
    // full definition (style.h) is not pulled into this widely included header.
    class style;
    struct list_marker
    {
        std::string     image;
        const char*     baseurl;
        list_style_type marker_type;
        web_color       color;
        position        pos;
        int             index;
        uint_ptr        font;
    };

    enum mouse_event
    {
        mouse_event_enter,
        mouse_event_leave,
    };

    // call back interface to draw text, images and other elements
    class document_container
    {
      public:
        virtual litehtml::uint_ptr create_font(const font_description& descr, const document* doc,
                                               litehtml::font_metrics* fm)                                          = 0;
        virtual void               delete_font(litehtml::uint_ptr hFont)                                            = 0;
        virtual pixel_t            text_width(const char* text, litehtml::uint_ptr hFont)                           = 0;
        virtual void               draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont,
                                             litehtml::web_color color, const litehtml::position& pos)              = 0;
        virtual pixel_t            pt_to_px(float pt) const                                                         = 0;
        virtual pixel_t            get_default_font_size() const                                                    = 0;
        virtual const char*        get_default_font_name() const                                                    = 0;
        virtual void               draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker)    = 0;
        virtual void               load_image(const char* src, const char* baseurl, bool redraw_on_ready)           = 0;
        virtual void               get_image_size(const char* src, const char* baseurl, litehtml::size& sz)         = 0;
        virtual void draw_image(litehtml::uint_ptr hdc, const background_layer& layer, const std::string& url,
                                const std::string& base_url)                                                        = 0;
        virtual void draw_solid_fill(litehtml::uint_ptr hdc, const background_layer& layer, const web_color& color) = 0;
        virtual void draw_linear_gradient(litehtml::uint_ptr hdc, const background_layer& layer,
                                          const background_layer::linear_gradient& gradient)                        = 0;
        virtual void draw_radial_gradient(litehtml::uint_ptr hdc, const background_layer& layer,
                                          const background_layer::radial_gradient& gradient)                        = 0;
        virtual void draw_conic_gradient(litehtml::uint_ptr hdc, const background_layer& layer,
                                         const background_layer::conic_gradient& gradient)                          = 0;
        virtual void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                                  const litehtml::position& draw_pos, bool root)                                    = 0;

        virtual void set_caption(const char* caption)                                                       = 0;
        virtual void set_base_url(const char* base_url)                                                     = 0;
        virtual void link(const std::shared_ptr<litehtml::document>& doc, const litehtml::element::ptr& el) = 0;
        virtual void on_anchor_click(const char* url, const litehtml::element::ptr& el)                     = 0;
        virtual bool on_element_click(const litehtml::element::ptr& /*el*/)
        {
            return false;
        };
        virtual void on_mouse_event(const litehtml::element::ptr& el, litehtml::mouse_event event)        = 0;
        virtual void set_cursor(const char* cursor)                                                       = 0;
        virtual void transform_text(std::string& text, litehtml::text_transform tt)                       = 0;
        virtual void import_css(std::string& text, const std::string& url, std::string& baseurl)          = 0;
        virtual void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius) = 0;
        virtual void del_clip()                                                                           = 0;
        virtual void get_viewport(litehtml::position& viewport) const                                     = 0;
        virtual litehtml::element::ptr create_element(const char* tag_name, const litehtml::string_map& attributes,
                                                      const std::shared_ptr<litehtml::document>& doc)     = 0;

        virtual void        get_media_features(litehtml::media_features& media) const       = 0;
        virtual void        get_language(std::string& language, std::string& culture) const = 0;
        virtual std::string resolve_color(const std::string& /*color*/) const
        {
            return {};
        }
        virtual void split_text(const char* text, const std::function<void(const char*)>& on_word,
                                const std::function<void(const char*)>& on_space);

        // LHU PATCH (experiment E6): memoized parse of an inline `style="..."` attribute.
        //
        // WHY. litehtml re-tokenizes and re-parses every style attribute from scratch in
        // html_tag::compute_styles(). A game UI emits the same few declaration blocks on
        // hundreds of elements (a 150-row inventory page has 451 styled elements built from
        // 3 distinct strings), so the same block is parsed hundreds of times per document.
        // Measured at ~122 heap allocations per styled element.
        //
        // WHY IT IS SAFE. style::add(txt, baseurl, container) reads no document state at all
        // -- it never touches document::mode(), never sees the element, and the only external
        // input is `container`, consulted solely by document_container::resolve_color() for
        // non-standard colour names. html_tag::compute_styles always passes baseurl "".
        // So the parse is a pure function of (string, container), and the cache is keyed by
        // the string and *owned by the container*, which is what makes the container part of
        // the key implicit and impossible to get wrong.
        //
        // The cached style is only ever read: callers do m_style.combine(*cached), which deep
        // copies each property_value. var() substitution (style::subst_vars) then mutates the
        // element's own copy, never this one.
        //
        // Returns nullptr when caching is disabled (env LHU_EXP_STYLECACHE=0) or when the
        // cache is full, in which case the caller must fall back to parsing directly.
        const style* cached_inline_style(const char* text);

      protected:
        virtual ~document_container() = default;

      private:
        // LHU PATCH (experiment E6). std::less<> so a lookup can be done from a string_view
        // without allocating a temporary std::string on every hit. Owned per container, i.e.
        // per LhuContext, which is documented single-threaded; a process-global cache would
        // be shared between containers with different resolve_color() behaviour and is wrong.
        std::map<std::string, std::shared_ptr<style>, std::less<>> m_inline_style_cache;
    };
} // namespace litehtml

#endif // LITEHTML_DOCUMENT_CONTAINER_H
