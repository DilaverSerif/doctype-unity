#ifndef LITEHTML_FORMATTING_CONTEXT_H
#define LITEHTML_FORMATTING_CONTEXT_H

#include "types.h"
#include <list>
#include <vector>

namespace litehtml
{
    // -----------------------------------------------------------------------
    // LHU EXPERIMENT E5 -- local change to litehtml. Keep across updates.
    //
    // WHY: m_floats_left / m_floats_right are flat lists that every float query
    // scans end to end, and they are block-formatting-context global. A page
    // with one float per row therefore costs O(n^2) per document::render(): on
    // the benchmark's 150-row inventory page place_to_left + place_to_right
    // alone scan 33,675 float entries per layout (2,380 at 40 rows -- 3.75x the
    // rows for 14x the work).
    //
    // WHAT: `float_index` mirrors each list, grouped by the key the list is
    // already sorted on (m_floats_left by pos.right() descending, m_floats_right
    // by pos.left() ascending). Floats sharing a key are appended, so every
    // group is in document order and therefore sorted by y; each query
    // binary-searches the y window inside each group. Matches come out in
    // exactly the list order the original loops walked, which is load bearing:
    // place_to_left/place_to_right break on the *first* intersecting float and
    // find_min_left/right sum floats, so re-ordering would change results.
    //
    // Everything here is skipped when the environment variable LHU_EXP_FLOATS
    // is "0", which restores the original loops verbatim so one binary can be
    // A/B'd.
    // -----------------------------------------------------------------------

#ifdef LHU_FLOAT_STATS
    // Float entries touched per loop. Compiled out unless LHU_FLOAT_STATS is
    // defined; used by tests/floatcount.cpp to show the quadratic term going
    // away. Never enabled in a shipping build.
    struct float_scan_stats
    {
        unsigned long long add_scan = 0, place_left = 0, place_right = 0, line_left = 0, line_right = 0,
                           floats_height = 0, left_height = 0, right_height = 0, min_left = 0, min_right = 0,
                           update = 0, rel_shift = 0, clear = 0, index_rebuild = 0, rebuild_calls = 0, bsearch = 0;
        unsigned long long total() const
        {
            return add_scan + place_left + place_right + line_left + line_right + floats_height + left_height +
                   right_height + min_left + min_right + update + rel_shift + clear + index_rebuild + bsearch;
        }
    };
    extern float_scan_stats g_float_scan_stats;
#define LHU_FS(f) (++::litehtml::g_float_scan_stats.f)
#define LHU_FSN(f, n) (::litehtml::g_float_scan_stats.f += (unsigned long long)(n))
#else
#define LHU_FS(f) ((void)0)
#define LHU_FSN(f, n) ((void)0)
#endif

    class formatting_context
    {
      public:
        struct new_position
        {
            bool    found    = false; // true if position found else use the suplied position
            bool    new_line = false; // true if element was moved to new line
            pixel_t top;              // element top position
            pixel_t left;             // element left position
            pixel_t width;            // maximum width available for element
        };

        struct el_position
        {
            margins  el_margins;      // element margins
            position el_pos;          // element position including margins
            pixel_t  container_width; // maximum width on containing block
        };

      private:
        std::list<floated_box> m_floats_left;
        std::list<floated_box> m_floats_right;
        pixel_pixel_cache      m_cache_line_left;
        pixel_pixel_cache      m_cache_line_right;
        pixel_t                m_current_top;
        pixel_t                m_current_left;

        // --- LHU E5 -------------------------------------------------------
        using float_iter = std::list<floated_box>::iterator;

        struct float_entry
        {
            float      top;    // raw pos.top()    -- raw floats, not pixel_t, so
            float      bottom; // raw pos.bottom()    ordering is a real ordering
            float_iter it;
        };

        // One group per distinct inner edge. Items are in list order, which for
        // a group is also document order.
        struct float_group
        {
            float                    key        = 0.f;
            float                    min_top    = 0.f;
            float                    max_bottom = 0.f;
            bool                     y_sorted   = true; // top[] and bottom[] both non-decreasing
            std::vector<float_entry> items;
        };

        struct float_index
        {
            std::vector<float_group> groups; // in list order
            bool                     valid = true;
        };

        // Order-independent maximum tracker.
        //
        // pixel_t's operator< is an epsilon compare and is therefore not
        // transitive, so `h = std::max(h, v)` over a list depends on the order
        // the list is walked. The cached result may only be used when the
        // largest value is separated from every other value by at least that
        // epsilon -- then the fold lands on it whatever the order. Otherwise
        // callers fall back to the original loop.
        struct max_fold
        {
            float best   = 0.f;
            float second = 0.f;
            bool  has    = false;
            bool  has2   = false;

            void reset()
            {
                *this = max_fold();
            }
            void add(float v)
            {
                if(!has)
                {
                    best = v;
                    has  = true;
                    return;
                }
                if(v > best)
                {
                    if(!has2 || best > second)
                    {
                        second = best;
                        has2   = true;
                    }
                    best = v;
                } else if(v < best)
                {
                    if(!has2 || v > second)
                    {
                        second = v;
                        has2   = true;
                    }
                }
            }
            // True when the fold result cannot depend on iteration order.
            bool separable() const
            {
                return !has || !has2 || (best - second) >= 0.0001f;
            }
        };

        bool        m_exp;              // LHU_EXP_FLOATS
        float_index m_index_left;
        float_index m_index_right;

        mutable bool     m_agg_valid = true;
        mutable max_fold m_agg_bottom_left;  // pos.bottom() over m_floats_left
        mutable max_fold m_agg_bottom_right; // pos.bottom() over m_floats_right
        mutable max_fold m_agg_bottom_all;   // pos.bottom() over both, in list order
        mutable max_fold m_agg_top_clear_l;  // pos.top() where clear is left|both
        mutable max_fold m_agg_top_clear_r;  // pos.top() where clear is right|both

        static bool exp_floats_enabled();

        // Insert `it` into `idx`, or invalidate the index if the float's key
        // does not fall cleanly into the group layout. Returns false when the
        // caller must fall back to the original linear insertion scan.
        bool index_insert(float_index& idx, std::list<floated_box>& lst, const floated_box& fb, bool left_side);
        static void index_rebuild(float_index& idx, std::list<floated_box>& lst, bool left_side);
        void        agg_rebuild() const;
        void        agg_ensure() const
        {
            if(!m_agg_valid)
            {
                agg_rebuild();
            }
        }

        // [lo, hi) of `g.items` that can satisfy the caller's y test. `point`
        // selects the `y >= top && y < bottom` form used by the line queries;
        // otherwise the `bottom > qt && top < qb` form used by on_same_line.
        static void group_window(const float_group& g, float qt, float qb, bool point, size_t& lo, size_t& hi);

      public:
        formatting_context() :
            m_current_top(0),
            m_current_left(0),
            m_exp(exp_floats_enabled())
        {
        }

        void push_position(pixel_t x, pixel_t y)
        {
            m_current_left += x;
            m_current_top  += y;
        }
        void pop_position(pixel_t x, pixel_t y)
        {
            m_current_left -= x;
            m_current_top  -= y;
        }

        void         add_float(const std::shared_ptr<render_item>& el, pixel_t min_width, int context);
        void         clear_floats(int context);
        new_position place_to_left(const el_position& el_pos) const;
        new_position place_to_right(const el_position& el_pos) const;
        pixel_t      get_floats_height(element_float el_float = float_none) const;
        pixel_t      get_left_floats_height() const;
        pixel_t      get_right_floats_height() const;
        pixel_t      get_line_left(pixel_t y);
        void         get_line_left_right(pixel_t y, pixel_t def_right, pixel_t& ln_left, pixel_t& ln_right)
        {
            ln_left  = get_line_left(y);
            ln_right = get_line_right(y, def_right);
        }
        pixel_t get_line_right(pixel_t y, pixel_t def_right);
        pixel_t get_cleared_top(const std::shared_ptr<render_item>& el, pixel_t line_top) const;
        void    update_floats(pixel_t dy, const std::shared_ptr<render_item>& parent);
        void    apply_relative_shift(const containing_block_context& containing_block_size);
        pixel_t find_min_left(pixel_t y, int context_idx);
        pixel_t find_min_right(pixel_t y, pixel_t right, int context_idx);
    };
} // namespace litehtml

#endif // LITEHTML_FORMATTING_CONTEXT_H
