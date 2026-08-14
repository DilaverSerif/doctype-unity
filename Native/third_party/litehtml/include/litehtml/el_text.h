#ifndef LITEHTML_EL_TEXT_H
#define LITEHTML_EL_TEXT_H

#include "element.h"
#include "document.h"

namespace litehtml
{
    class el_text : public element
    {
      protected:
        std::string m_text;
        std::string m_transformed_text;
        size        m_size;
        bool        m_use_transformed;
        bool        m_draw_spaces;

      public:
        el_text(const char* text, const document::ptr& doc);

        void get_text(std::string& text) const override;

        // Replaces the node's text. The base class leaves set_data() empty, so
        // without this override there is no way to change a text node after the
        // document has been parsed. The new text is not measured here:
        // compute_styles() is what recomputes m_size (and m_transformed_text),
        // and it has to run before the next render or the node keeps the old
        // width and the line box is laid out against stale geometry.
        void set_data(const char* data) override;

        // The node's raw (untransformed) text, so a caller can tell whether a
        // set_data() would actually change anything.
        const std::string& text() const
        {
            return m_text;
        }

        // The measurement compute_styles() produced for the current text. This
        // is the *only* geometry a text node contributes to layout: the inline
        // formatting context reads it through get_content_size() and everything
        // downstream -- line breaking, float placement, block heights -- is a
        // function of it. Exposing it lets a caller compare the measurement
        // before and after a set_data() and discover that a mutation changed no
        // geometry at all, which is what makes skipping a re-render sound.
        const litehtml::size& measured_size() const
        {
            return m_size;
        }

        void compute_styles(bool recursive) override;
        bool is_text() const override
        {
            return true;
        }

        void        draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                         const std::shared_ptr<render_item>& ri) override;
        std::string dump_get_name() override;

        std::vector<std::tuple<std::string, std::string>> dump_get_attrs() override;

      protected:
        void get_content_size(size& sz, pixel_t max_width) override;
    };
} // namespace litehtml

#endif // LITEHTML_EL_TEXT_H
