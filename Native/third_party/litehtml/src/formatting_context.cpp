#include "render_item.h"
#include "types.h"
#include <optional>
#include "formatting_context.h"

// LHU EXPERIMENT E5 -- see the block comment in formatting_context.h for why
// this file diverges from upstream litehtml. Every divergence is fenced with
// `if(m_exp)`; with LHU_EXP_FLOATS=0 the original loops run verbatim.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef LHU_FLOAT_STATS
namespace litehtml
{
    float_scan_stats g_float_scan_stats;
}
#endif

// LHU E5: read once. Default on; "0" restores the original code path.
bool litehtml::formatting_context::exp_floats_enabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("LHU_EXP_FLOATS");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

// LHU E5 ---------------------------------------------------------------------
// Index maintenance.

void litehtml::formatting_context::index_rebuild(float_index& idx, std::list<floated_box>& lst, bool left_side)
{
    LHU_FS(rebuild_calls);
    idx.groups.clear();
    idx.valid = true;

    for(auto it = lst.begin(); it != lst.end(); ++it)
    {
        LHU_FS(index_rebuild);
        const float key = left_side ? (float)it->pos.right() : (float)it->pos.left();
        const float top = (float)it->pos.top();
        const float bot = (float)it->pos.bottom();

        if(idx.groups.empty() || idx.groups.back().key != key)
        {
            if(!idx.groups.empty())
            {
                // Adjacent groups must be ordered *and* far enough apart that
                // pixel_t's epsilon compare agrees with the raw ordering,
                // otherwise the list is not the concatenation of its groups.
                const pixel_t prev(idx.groups.back().key);
                const pixel_t cur(key);
                const bool    ok = left_side ? (prev > cur) : (prev < cur);
                if(!ok)
                {
                    idx.groups.clear();
                    idx.valid = false;
                    return;
                }
            }
            float_group g;
            g.key        = key;
            g.min_top    = top;
            g.max_bottom = bot;
            g.y_sorted   = true;
            g.items.push_back(float_entry { top, bot, it });
            idx.groups.push_back(std::move(g));
        } else
        {
            float_group& g = idx.groups.back();
            if(!(g.items.back().top <= top && g.items.back().bottom <= bot))
            {
                g.y_sorted = false;
            }
            g.items.push_back(float_entry { top, bot, it });
            g.min_top    = std::min(g.min_top, top);
            g.max_bottom = std::max(g.max_bottom, bot);
        }
    }
}

bool litehtml::formatting_context::index_insert(float_index& idx, std::list<floated_box>& lst, const floated_box& fb,
                                               bool left_side)
{
    if(!idx.valid)
    {
        return false;
    }

    const float key = left_side ? (float)fb.pos.right() : (float)fb.pos.left();
    const float top = (float)fb.pos.top();
    const float bot = (float)fb.pos.bottom();

    // Group keys are raw-strictly-monotone by construction, so this is a real
    // binary search. It reproduces the position the original linear scan finds.
    size_t g;
    {
        auto first = idx.groups.begin();
        auto last  = idx.groups.end();
        auto p     = left_side ? std::partition_point(first, last, [&](const float_group& x) { return x.key > key; })
                               : std::partition_point(first, last, [&](const float_group& x) { return x.key < key; });
        g = (size_t)(p - first);
        LHU_FSN(bsearch, 1);
    }

    const bool same = (g < idx.groups.size() && idx.groups[g].key == key);

    if(!same)
    {
        // A brand new group only reproduces the linear scan when pixel_t's
        // epsilon compare puts the new float strictly before groups[g].
        if(g < idx.groups.size())
        {
            const pixel_t cur(idx.groups[g].key);
            const pixel_t k(key);
            const bool    ok = left_side ? (k > cur) : (k < cur);
            if(!ok)
            {
                return false; // too close to call -- let the caller scan
            }
        }
    }

    // Where the float lands in the list.
    float_iter at;
    if(same)
    {
        at = (g + 1 < idx.groups.size()) ? idx.groups[g + 1].items.front().it : lst.end();
    } else
    {
        at = (g < idx.groups.size()) ? idx.groups[g].items.front().it : lst.end();
    }

    const float_iter it = lst.insert(at, fb);

    if(same)
    {
        float_group& grp = idx.groups[g];
        if(!(grp.items.back().top <= top && grp.items.back().bottom <= bot))
        {
            grp.y_sorted = false;
        }
        grp.items.push_back(float_entry { top, bot, it });
        grp.min_top    = std::min(grp.min_top, top);
        grp.max_bottom = std::max(grp.max_bottom, bot);
    } else
    {
        float_group grp;
        grp.key        = key;
        grp.min_top    = top;
        grp.max_bottom = bot;
        grp.y_sorted   = true;
        grp.items.push_back(float_entry { top, bot, it });
        idx.groups.insert(idx.groups.begin() + (long)g, std::move(grp));
    }
    return true;
}

void litehtml::formatting_context::agg_rebuild() const
{
    m_agg_bottom_left.reset();
    m_agg_bottom_right.reset();
    m_agg_bottom_all.reset();
    m_agg_top_clear_l.reset();
    m_agg_top_clear_r.reset();

    for(const auto& fb : m_floats_left)
    {
        m_agg_bottom_left.add((float)fb.pos.bottom());
        m_agg_bottom_all.add((float)fb.pos.bottom());
        if(fb.clear_floats == clear_left || fb.clear_floats == clear_both)
        {
            m_agg_top_clear_l.add((float)fb.pos.top());
        }
        if(fb.clear_floats == clear_right || fb.clear_floats == clear_both)
        {
            m_agg_top_clear_r.add((float)fb.pos.top());
        }
    }
    for(const auto& fb : m_floats_right)
    {
        m_agg_bottom_right.add((float)fb.pos.bottom());
        m_agg_bottom_all.add((float)fb.pos.bottom());
        if(fb.clear_floats == clear_left || fb.clear_floats == clear_both)
        {
            m_agg_top_clear_l.add((float)fb.pos.top());
        }
        if(fb.clear_floats == clear_right || fb.clear_floats == clear_both)
        {
            m_agg_top_clear_r.add((float)fb.pos.top());
        }
    }
    m_agg_valid = true;
}

void litehtml::formatting_context::group_window(const float_group& g, float qt, float qb, bool point, size_t& lo,
                                                size_t& hi)
{
    lo = 0;
    hi = g.items.size();
    if(hi == 0)
    {
        return;
    }

    // Group-level reject. min_top / max_bottom bound every item, and pixel_t's
    // epsilon compare is monotone in the raw value, so a group that fails here
    // cannot contain a match.
    const pixel_t qt_p(qt);
    if(!(pixel_t(g.max_bottom) > qt_p))
    {
        lo = hi = 0;
        return;
    }
    if(point)
    {
        if(!(pixel_t(g.min_top) <= qt_p))
        {
            lo = hi = 0;
            return;
        }
    } else if(!(pixel_t(g.min_top) < pixel_t(qb)))
    {
        lo = hi = 0;
        return;
    }

    if(!g.y_sorted)
    {
        return; // whole group, still walked in list order
    }

    LHU_FSN(bsearch, 2);
    lo = (size_t)(std::partition_point(g.items.begin(), g.items.end(),
                                       [&](const float_entry& e) { return !(pixel_t(e.bottom) > qt_p); }) -
                  g.items.begin());
    if(point)
    {
        hi = (size_t)(std::partition_point(g.items.begin(), g.items.end(),
                                           [&](const float_entry& e) { return pixel_t(e.top) <= qt_p; }) -
                      g.items.begin());
    } else
    {
        const pixel_t qb_p(qb);
        hi = (size_t)(std::partition_point(g.items.begin(), g.items.end(),
                                           [&](const float_entry& e) { return pixel_t(e.top) < qb_p; }) -
                      g.items.begin());
    }
    if(hi < lo)
    {
        hi = lo;
    }
}
// LHU E5 end -----------------------------------------------------------------

void litehtml::formatting_context::add_float(const std::shared_ptr<render_item>& el, pixel_t min_width, int context)
{
    floated_box fb;
    fb.pos.x        = el->left() + m_current_left;
    fb.pos.y        = el->top() + m_current_top;
    fb.pos.width    = el->width();
    fb.pos.height   = el->height();
    fb.float_side   = el->src_el()->css().get_float();
    fb.clear_floats = el->src_el()->css().get_clear();
    fb.el           = el;
    fb.context      = context;
    fb.min_width    = min_width;

    // LHU E5: the fallback insertion path moves out of `fb`, so keep what the
    // cached maxima need before anything can touch it.
    const element_float exp_side = fb.float_side;
    const element_clear exp_clr  = fb.clear_floats;
    const float         exp_top  = (float)fb.pos.top();
    const float         exp_bot  = (float)fb.pos.bottom();

    if(fb.float_side == float_left)
    {
        // LHU E5: O(log n) placement through the group index; falls through to
        // the original scan when the index cannot represent this float.
        bool indexed = m_exp && index_insert(m_index_left, m_floats_left, fb, true);
        if(!indexed)
        {
            if(m_exp)
            {
                m_index_left.groups.clear();
                m_index_left.valid = false;
            }
            if(m_floats_left.empty())
            {
                m_floats_left.push_back(fb);
            } else
            {
                bool inserted = false;
                for(auto i = m_floats_left.begin(); i != m_floats_left.end(); i++)
                {
                    LHU_FS(add_scan);
                    if(fb.pos.right() > i->pos.right())
                    {
                        m_floats_left.insert(i, std::move(fb));
                        fb       = {};
                        inserted = true;
                        break;
                    }
                }
                if(!inserted)
                {
                    m_floats_left.push_back(std::move(fb));
                }
            }
        }
        m_cache_line_left.invalidate();
    } else if(fb.float_side == float_right)
    {
        bool indexed = m_exp && index_insert(m_index_right, m_floats_right, fb, false);
        if(!indexed)
        {
            if(m_exp)
            {
                m_index_right.groups.clear();
                m_index_right.valid = false;
            }
            if(m_floats_right.empty())
            {
                m_floats_right.push_back(std::move(fb));
            } else
            {
                bool inserted = false;
                for(auto i = m_floats_right.begin(); i != m_floats_right.end(); i++)
                {
                    LHU_FS(add_scan);
                    if(fb.pos.left() < i->pos.left())
                    {
                        m_floats_right.insert(i, std::move(fb));
                        fb       = {};
                        inserted = true;
                        break;
                    }
                }
                if(!inserted)
                {
                    m_floats_right.push_back(fb);
                }
            }
        }
        m_cache_line_right.invalidate();
    }

    // LHU E5: keep the cached maxima up to date instead of rescanning.
    if(m_exp && m_agg_valid && (exp_side == float_left || exp_side == float_right))
    {
        if(exp_side == float_left)
        {
            m_agg_bottom_left.add(exp_bot);
        } else
        {
            m_agg_bottom_right.add(exp_bot);
        }
        m_agg_bottom_all.add(exp_bot);
        if(exp_clr == clear_left || exp_clr == clear_both)
        {
            m_agg_top_clear_l.add(exp_top);
        }
        if(exp_clr == clear_right || exp_clr == clear_both)
        {
            m_agg_top_clear_r.add(exp_top);
        }
    }
}

litehtml::pixel_t litehtml::formatting_context::get_floats_height(element_float el_float) const
{
    // LHU E5: cached maxima, used only when the fold cannot depend on the order
    // the lists are walked.
    if(m_exp)
    {
        agg_ensure();
        const max_fold& f = (el_float == float_none)  ? m_agg_bottom_all
                            : (el_float == float_left) ? m_agg_top_clear_l
                                                       : m_agg_top_clear_r;
        if(f.separable())
        {
            pixel_t h = m_current_top;
            if(f.has && pixel_t(f.best) > h)
            {
                h = pixel_t(f.best);
            }
            return h - m_current_top;
        }
    }

    pixel_t h = m_current_top;

    for(const auto& fb : m_floats_left)
    {
        LHU_FS(floats_height);
        bool process = false;
        switch(el_float)
        {
        case float_none:
            process = true;
            break;
        case float_left:
            if(fb.clear_floats == clear_left || fb.clear_floats == clear_both)
            {
                process = true;
            }
            break;
        case float_right:
            if(fb.clear_floats == clear_right || fb.clear_floats == clear_both)
            {
                process = true;
            }
            break;
        }
        if(process)
        {
            if(el_float == float_none)
            {
                h = std::max(h, fb.pos.bottom());
            } else
            {
                h = std::max(h, fb.pos.top());
            }
        }
    }

    for(const auto& fb : m_floats_right)
    {
        LHU_FS(floats_height);
        bool process = false;
        switch(el_float)
        {
        case float_none:
            process = true;
            break;
        case float_left:
            if(fb.clear_floats == clear_left || fb.clear_floats == clear_both)
            {
                process = true;
            }
            break;
        case float_right:
            if(fb.clear_floats == clear_right || fb.clear_floats == clear_both)
            {
                process = true;
            }
            break;
        }
        if(process)
        {
            if(el_float == float_none)
            {
                h = std::max(h, fb.pos.bottom());
            } else
            {
                h = std::max(h, fb.pos.top());
            }
        }
    }

    return h - m_current_top;
}

litehtml::pixel_t litehtml::formatting_context::get_left_floats_height() const
{
    pixel_t h = 0_px;
    if(!m_floats_left.empty())
    {
        // LHU E5
        if(m_exp)
        {
            agg_ensure();
            if(m_agg_bottom_left.separable())
            {
                if(m_agg_bottom_left.has && pixel_t(m_agg_bottom_left.best) > h)
                {
                    h = pixel_t(m_agg_bottom_left.best);
                }
                return h - m_current_top;
            }
        }
        for(const auto& fb : m_floats_left)
        {
            LHU_FS(left_height);
            h = std::max(h, fb.pos.bottom());
        }
    }
    return h - m_current_top;
}

litehtml::pixel_t litehtml::formatting_context::get_right_floats_height() const
{
    pixel_t h = 0_px;
    if(!m_floats_right.empty())
    {
        // LHU E5
        if(m_exp)
        {
            agg_ensure();
            if(m_agg_bottom_right.separable())
            {
                if(m_agg_bottom_right.has && pixel_t(m_agg_bottom_right.best) > h)
                {
                    h = pixel_t(m_agg_bottom_right.best);
                }
                return h - m_current_top;
            }
        }
        for(const auto& fb : m_floats_right)
        {
            LHU_FS(right_height);
            h = std::max(h, fb.pos.bottom());
        }
    }
    return h - m_current_top;
}

litehtml::pixel_t litehtml::formatting_context::get_line_left(pixel_t y)
{
    y += m_current_top;

    if(m_cache_line_left.is_valid && m_cache_line_left.hash == y)
    {
        if(m_cache_line_left.val - m_current_left < 0_px)
        {
            return 0_px;
        }
        return m_cache_line_left.val - m_current_left;
    }

    pixel_t w = 0_px;
    // LHU E5
    if(m_exp && m_index_left.valid)
    {
        for(const auto& g : m_index_left.groups)
        {
            size_t lo, hi;
            group_window(g, (float)y, 0.f, true, lo, hi);
            for(size_t i = lo; i < hi; ++i)
            {
                LHU_FS(line_left);
                const floated_box& fb = *g.items[i].it;
                if(y >= fb.pos.top() && y < fb.pos.bottom())
                {
                    w = std::max(w, fb.pos.right());
                }
            }
        }
    } else
    {
        for(const auto& fb : m_floats_left)
        {
            LHU_FS(line_left);
            if(y >= fb.pos.top() && y < fb.pos.bottom())
            {
                w = std::max(w, fb.pos.right());
                if(w < fb.pos.right())
                {
                    break;
                }
            }
        }
    }
    m_cache_line_left.set_value(y, w);
    w -= m_current_left;
    if(w < 0_px)
    {
        return 0_px;
    }
    return w;
}

litehtml::pixel_t litehtml::formatting_context::get_line_right(pixel_t y, pixel_t def_right)
{
    y         += m_current_top;
    def_right += m_current_left;
    if(m_cache_line_right.is_valid && m_cache_line_right.hash == y)
    {
        if(m_cache_line_right.is_default)
        {
            return def_right - m_current_left;
        }
        pixel_t w = std::min(m_cache_line_right.val, def_right) - m_current_left;
        if(w < 0_px)
        {
            return 0_px;
        }
        return w;
    }

    pixel_t w                     = def_right;
    m_cache_line_right.is_default = true;
    // LHU E5
    if(m_exp && m_index_right.valid)
    {
        for(const auto& g : m_index_right.groups)
        {
            size_t lo, hi;
            group_window(g, (float)y, 0.f, true, lo, hi);
            for(size_t i = lo; i < hi; ++i)
            {
                LHU_FS(line_right);
                const floated_box& fb = *g.items[i].it;
                if(y >= fb.pos.top() && y < fb.pos.bottom())
                {
                    w                             = std::min(w, fb.pos.left());
                    m_cache_line_right.is_default = false;
                }
            }
        }
    } else
    {
        for(const auto& fb : m_floats_right)
        {
            LHU_FS(line_right);
            if(y >= fb.pos.top() && y < fb.pos.bottom())
            {
                w                             = std::min(w, fb.pos.left());
                m_cache_line_right.is_default = false;
                if(w > fb.pos.left())
                {
                    break;
                }
            }
        }
    }
    m_cache_line_right.set_value(y, w);
    w -= m_current_left;
    if(w < 0_px)
    {
        return 0_px;
    }
    return w;
}

void litehtml::formatting_context::clear_floats(int context)
{
    bool changed = false;

    auto iter = m_floats_left.begin();
    while(iter != m_floats_left.end())
    {
        LHU_FS(clear);
        if(iter->context >= context)
        {
            iter = m_floats_left.erase(iter);
            m_cache_line_left.invalidate();
            changed = true;
        } else
        {
            iter++;
        }
    }

    iter = m_floats_right.begin();
    while(iter != m_floats_right.end())
    {
        LHU_FS(clear);
        if(iter->context >= context)
        {
            iter = m_floats_right.erase(iter);
            m_cache_line_right.invalidate();
            changed = true;
        } else
        {
            iter++;
        }
    }

    // LHU E5: erasing invalidates the stored iterators, so rebuild.
    if(m_exp && changed)
    {
        index_rebuild(m_index_left, m_floats_left, true);
        index_rebuild(m_index_right, m_floats_right, false);
        m_agg_valid = false;
    }
}

litehtml::pixel_t litehtml::formatting_context::get_cleared_top(const std::shared_ptr<render_item>& el,
                                                                pixel_t                             line_top) const
{
    switch(el->src_el()->css().get_clear())
    {
    case clear_left:
        {
            pixel_t fh = get_left_floats_height();
            if(fh != 0_px && fh > line_top)
            {
                line_top = fh;
            }
        }
        break;
    case clear_right:
        {
            pixel_t fh = get_right_floats_height();
            if(fh != 0_px && fh > line_top)
            {
                line_top = fh;
            }
        }
        break;
    case clear_both:
        {
            pixel_t fh = get_floats_height(float_none);
            if(fh != 0_px && fh > line_top)
            {
                line_top = fh;
            }
        }
        break;
    default:
        if(el->src_el()->css().get_float() != float_none)
        {
            pixel_t fh = get_floats_height(el->src_el()->css().get_float());
            if(fh != 0_px && fh > line_top)
            {
                line_top = fh;
            }
        }
        break;
    }
    return line_top;
}

litehtml::formatting_context::new_position litehtml::formatting_context::place_to_left(const el_position& el_pos) const
{
    position pos_el   = el_pos.el_pos;
    pos_el.x         += m_current_left + el_pos.el_margins.left;
    pos_el.y         += m_current_top + el_pos.el_margins.top;
    pos_el.width     -= el_pos.el_margins.left + el_pos.el_margins.right;
    auto max_right    = el_pos.container_width + m_current_left;
    bool was_changed  = false;
    bool next_line    = false;
    bool left_side    = false; // true if floating block at the left side
    bool right_side   = false; // true if floating block at the right side

    // LHU E5
    const bool use_l = m_exp && m_index_left.valid;
    const bool use_r = m_exp && m_index_right.valid;

    while(true)
    {
        std::optional<position> max_left_pos;
        bool                    found    = false;
        pixel_t                 max_left = m_current_left;
        left_side                        = false;
        // check intersection with left floats
        if(use_l)
        {
            const float qt = (float)pos_el.top();
            const float qb = (float)pos_el.bottom();
            for(const auto& g : m_index_left.groups)
            {
                size_t lo, hi;
                group_window(g, qt, qb, false, lo, hi);
                for(size_t i = lo; i < hi; ++i)
                {
                    LHU_FS(place_left);
                    const floated_box& fb = *g.items[i].it;
                    if(fb.pos.height == 0_px)
                    {
                        continue;
                    }
                    if(fb.pos.on_same_line(pos_el, true))
                    {
                        left_side = true;
                        max_left  = std::max(max_left, fb.pos.right());
                        if(pos_el.x < fb.pos.right())
                        {
                            pos_el.x     = fb.pos.right();
                            max_left_pos = fb.pos;
                            found        = true;
                            was_changed  = true;
                        }
                    }
                }
            }
        } else
        {
            for(const auto& fb : m_floats_left)
            {
                LHU_FS(place_left);
                if(fb.pos.height == 0_px)
                {
                    continue;
                }
                if(fb.pos.on_same_line(pos_el, true))
                {
                    left_side = true;
                    max_left  = std::max(max_left, fb.pos.right());
                    if(pos_el.x < fb.pos.right())
                    {
                        pos_el.x     = fb.pos.right();
                        max_left_pos = fb.pos;
                        found        = true;
                        was_changed  = true;
                    }
                }
            }
        }
        if(pos_el.right() > max_right && found)
        {
            // move to next line
            next_line = true;
            pos_el.x  = m_current_left + el_pos.el_margins.left;
            pos_el.y  = max_left_pos->bottom();
        } else
        {
            found             = false;
            pixel_t min_right = max_right;
            right_side        = false;
            // check intersection with right floats
            if(use_r)
            {
                const float qt = (float)pos_el.top();
                const float qb = (float)pos_el.bottom();
                for(const auto& g : m_index_right.groups)
                {
                    size_t lo, hi;
                    group_window(g, qt, qb, false, lo, hi);
                    for(size_t i = lo; i < hi && !found; ++i)
                    {
                        LHU_FS(place_left);
                        const floated_box& fb = *g.items[i].it;
                        if(fb.pos.height == 0_px)
                        {
                            continue;
                        }
                        if(fb.pos.on_same_line(pos_el, true))
                        {
                            right_side = true;
                            min_right  = std::min(min_right, fb.pos.left());
                        }
                        if(fb.pos.does_intersect(&pos_el, true))
                        {
                            right_side = false;
                            pos_el.x   = m_current_left + el_pos.el_margins.left;
                            pos_el.y   = max_left_pos.has_value()
                                             ? std::min(max_left_pos->bottom(), fb.pos.bottom())
                                             : fb.pos.bottom();
                            found       = true;
                            next_line   = true;
                            was_changed = true;
                        }
                    }
                    if(found)
                    {
                        break;
                    }
                }
            } else
            {
                for(const auto& fb : m_floats_right)
                {
                    LHU_FS(place_left);
                    if(fb.pos.height == 0_px)
                    {
                        continue;
                    }
                    // calculate minimum right position
                    if(fb.pos.on_same_line(pos_el, true))
                    {
                        right_side = true;
                        min_right  = std::min(min_right, fb.pos.left());
                    }
                    // if element intersects float box move it to the next line
                    if(fb.pos.does_intersect(&pos_el, true))
                    {
                        right_side = false;
                        pos_el.x   = m_current_left + el_pos.el_margins.left;
                        pos_el.y   = max_left_pos.has_value() ? std::min(max_left_pos->bottom(), fb.pos.bottom())
                                                              : fb.pos.bottom();
                        found       = true;
                        next_line   = true;
                        was_changed = true;
                        break;
                    }
                }
            }
            if(!found)
            {
                // position found
                new_position pos;
                pos.found    = was_changed;
                pos.new_line = next_line;
                pos.top      = pos_el.y - m_current_top - el_pos.el_margins.top;
                pos.left     = pos_el.x - m_current_left - el_pos.el_margins.left;
                pos.width    = min_right - max_left;

                if(left_side)
                {
                    pos.width += el_pos.el_margins.left;
                }
                if(right_side)
                {
                    pos.width += el_pos.el_margins.right;
                }

                return pos;
            }
        }
    }
    // position not found
    new_position pos;
    pos.found = false;
    return pos;
}

litehtml::formatting_context::new_position litehtml::formatting_context::place_to_right(const el_position& el_pos) const
{
    position pos_el   = el_pos.el_pos;
    pos_el.x         += m_current_left + el_pos.el_margins.left;
    pos_el.y         += m_current_top + el_pos.el_margins.top;
    pos_el.width     -= el_pos.el_margins.left + el_pos.el_margins.right;
    auto max_right    = el_pos.container_width + m_current_left;
    bool was_changed  = false;
    bool next_line    = false;
    bool left_side    = false; // true if floating block at the left side
    bool right_side   = false; // true if floating block at the right side

    // LHU E5
    const bool use_l = m_exp && m_index_left.valid;
    const bool use_r = m_exp && m_index_right.valid;

    while(true)
    {
        std::optional<position> min_right_pos;
        bool                    found     = false;
        pixel_t                 min_right = max_right;
        right_side                        = false;
        // check intersection with right floats
        if(use_r)
        {
            const float qt = (float)pos_el.top();
            const float qb = (float)pos_el.bottom();
            for(const auto& g : m_index_right.groups)
            {
                size_t lo, hi;
                group_window(g, qt, qb, false, lo, hi);
                for(size_t i = lo; i < hi; ++i)
                {
                    LHU_FS(place_right);
                    const floated_box& fb = *g.items[i].it;
                    if(fb.pos.height == 0_px)
                    {
                        continue;
                    }
                    if(fb.pos.on_same_line(pos_el, true))
                    {
                        right_side = true;
                        min_right  = std::min(min_right, fb.pos.left());
                        if(pos_el.right() > fb.pos.left())
                        {
                            pos_el.x      = fb.pos.left() - pos_el.width;
                            min_right_pos = fb.pos;
                            found         = true;
                            was_changed   = true;
                        }
                    }
                }
            }
        } else
        {
            for(const auto& fb : m_floats_right)
            {
                LHU_FS(place_right);
                if(fb.pos.height == 0_px)
                {
                    continue;
                }
                // if element intersects float box move it to the left of float box
                if(fb.pos.on_same_line(pos_el, true))
                {
                    right_side = true;
                    min_right  = std::min(min_right, fb.pos.left());
                    if(pos_el.right() > fb.pos.left())
                    {
                        pos_el.x      = fb.pos.left() - pos_el.width;
                        min_right_pos = fb.pos;
                        found         = true;
                        was_changed   = true;
                    }
                }
            }
        }
        if(pos_el.left() < m_current_left && found)
        {
            // move to next line
            next_line = true;
            pos_el.x  = max_right - pos_el.width - el_pos.el_margins.left;
            pos_el.y  = min_right_pos->bottom();
        } else
        {
            found            = false;
            pixel_t max_left = m_current_left;
            left_side        = false;
            // check intersection with left floats
            if(use_l)
            {
                const float qt = (float)pos_el.top();
                const float qb = (float)pos_el.bottom();
                for(const auto& g : m_index_left.groups)
                {
                    size_t lo, hi;
                    group_window(g, qt, qb, false, lo, hi);
                    for(size_t i = lo; i < hi && !found; ++i)
                    {
                        LHU_FS(place_right);
                        const floated_box& fb = *g.items[i].it;
                        if(fb.pos.height == 0_px)
                        {
                            continue;
                        }
                        if(fb.pos.on_same_line(pos_el, true))
                        {
                            left_side = true;
                            max_left  = std::max(max_left, fb.pos.right());
                        }
                        if(fb.pos.does_intersect(&pos_el, true))
                        {
                            left_side   = false;
                            pos_el.x    = max_right - pos_el.width - el_pos.el_margins.left;
                            pos_el.y    = min_right_pos.has_value()
                                              ? std::min(min_right_pos->bottom(), fb.pos.bottom())
                                              : fb.pos.bottom();
                            found       = true;
                            next_line   = true;
                            was_changed = true;
                        }
                    }
                    if(found)
                    {
                        break;
                    }
                }
            } else
            {
                for(const auto& fb : m_floats_left)
                {
                    LHU_FS(place_right);
                    if(fb.pos.height == 0_px)
                    {
                        continue;
                    }
                    // calculate maximum left position
                    if(fb.pos.on_same_line(pos_el, true))
                    {
                        left_side = true;
                        max_left  = std::max(max_left, fb.pos.right());
                    }
                    // if element intersects float box move it to the next line
                    if(fb.pos.does_intersect(&pos_el, true))
                    {
                        left_side   = false;
                        pos_el.x    = max_right - pos_el.width - el_pos.el_margins.left;
                        pos_el.y    = min_right_pos.has_value() ? std::min(min_right_pos->bottom(), fb.pos.bottom())
                                                                : fb.pos.bottom();
                        found       = true;
                        next_line   = true;
                        was_changed = true;
                        break;
                    }
                }
            }
            if(!found)
            {
                // position found
                new_position pos;
                pos.found    = was_changed;
                pos.new_line = next_line;
                pos.top      = pos_el.y - m_current_top - el_pos.el_margins.top;
                pos.left     = pos_el.x - m_current_left - el_pos.el_margins.left;
                pos.width    = min_right - max_left;

                if(left_side)
                {
                    pos.width += el_pos.el_margins.left;
                }
                if(right_side)
                {
                    pos.width += el_pos.el_margins.right;
                }

                return pos;
            }
        }
    }
    // position not found
    new_position pos;
    pos.found = false;
    return pos;
}

void litehtml::formatting_context::update_floats(pixel_t dy, const std::shared_ptr<render_item>& parent)
{
    bool reset_cache = false;
    for(auto fb = m_floats_left.rbegin(); fb != m_floats_left.rend(); fb++)
    {
        LHU_FS(update);
        if(fb->el->src_el()->is_ancestor(parent->src_el()))
        {
            reset_cache  = true;
            fb->pos.y   += dy;
        }
    }
    if(reset_cache)
    {
        m_cache_line_left.invalidate();
        // LHU E5: the cached y values in the index are now stale.
        if(m_exp)
        {
            index_rebuild(m_index_left, m_floats_left, true);
            m_agg_valid = false;
        }
    }
    reset_cache = false;
    for(auto fb = m_floats_right.rbegin(); fb != m_floats_right.rend(); fb++)
    {
        LHU_FS(update);
        if(fb->el->src_el()->is_ancestor(parent->src_el()))
        {
            reset_cache  = true;
            fb->pos.y   += dy;
        }
    }
    if(reset_cache)
    {
        m_cache_line_right.invalidate();
        if(m_exp)
        {
            index_rebuild(m_index_right, m_floats_right, false);
            m_agg_valid = false;
        }
    }
}

void litehtml::formatting_context::apply_relative_shift(const containing_block_context& containing_block_size)
{
    for(const auto& fb : m_floats_left)
    {
        LHU_FS(rel_shift);
        fb.el->apply_relative_shift(containing_block_size);
    }
}

litehtml::pixel_t litehtml::formatting_context::find_min_left(pixel_t y, int context_idx)
{
    y                += m_current_top;
    pixel_t min_left  = m_current_left;
    // LHU E5
    if(m_exp && m_index_left.valid)
    {
        for(const auto& g : m_index_left.groups)
        {
            size_t lo, hi;
            group_window(g, (float)y, 0.f, true, lo, hi);
            for(size_t i = lo; i < hi; ++i)
            {
                LHU_FS(min_left);
                const floated_box& fb = *g.items[i].it;
                if(y >= fb.pos.top() && y < fb.pos.bottom() && fb.context == context_idx)
                {
                    min_left += fb.min_width;
                }
            }
        }
    } else
    {
        for(const auto& fb : m_floats_left)
        {
            LHU_FS(min_left);
            if(y >= fb.pos.top() && y < fb.pos.bottom() && fb.context == context_idx)
            {
                min_left += fb.min_width;
            }
        }
    }
    if(min_left < m_current_left)
    {
        return 0_px;
    }
    return min_left - m_current_left;
}

litehtml::pixel_t litehtml::formatting_context::find_min_right(pixel_t y, pixel_t right, int context_idx)
{
    y                 += m_current_top;
    pixel_t min_right  = right + m_current_left;
    // LHU E5
    if(m_exp && m_index_right.valid)
    {
        for(const auto& g : m_index_right.groups)
        {
            size_t lo, hi;
            group_window(g, (float)y, 0.f, true, lo, hi);
            for(size_t i = lo; i < hi; ++i)
            {
                LHU_FS(min_right);
                const floated_box& fb = *g.items[i].it;
                if(y >= fb.pos.top() && y < fb.pos.bottom() && fb.context == context_idx)
                {
                    min_right -= fb.min_width;
                }
            }
        }
    } else
    {
        for(const auto& fb : m_floats_right)
        {
            LHU_FS(min_right);
            if(y >= fb.pos.top() && y < fb.pos.bottom() && fb.context == context_idx)
            {
                min_right -= fb.min_width;
            }
        }
    }
    if(min_right < m_current_left)
    {
        return 0_px;
    }
    return min_right - m_current_left;
}
