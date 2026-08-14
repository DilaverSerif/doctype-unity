#include "lhu_quadcache.h"

#include <cstring>

namespace lhu
{

namespace
{
constexpr int kGradRowBytes = Container::kGradLutWidth * 4;

inline bool same_clip(const ClipRect& a, const ClipRect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && a.r[0] == b.r[0] && a.r[1] == b.r[1] &&
           a.r[2] == b.r[2] && a.r[3] == b.r[3];
}
} // namespace

//
// Hooks
//

int QuadCache::hook_open(void* ctx, litehtml::render_item* ri, int flag, float x, float y)
{
    return static_cast<QuadCache*>(ctx)->open(ri, flag, x, y);
}

void QuadCache::hook_close(void* ctx)
{
    static_cast<QuadCache*>(ctx)->close();
}

void QuadCache::hook_styles_changed(void* ctx, litehtml::element* el)
{
    static_cast<QuadCache*>(ctx)->mark_element_dirty(el);
}

//
// Invalidation
//

void QuadCache::on_document_replaced()
{
    m_peak_entries = m_peak_entries > m_entries.size() ? m_peak_entries : m_entries.size();
    m_entries.clear();
    m_entries.reserve(m_peak_entries);
    m_open.clear();
    m_retained.clear();
    m_retained_grad.clear();
    ++m_retained_gen;
    ++m_dirty_epoch;
    m_have_retained      = false;
    m_buffer_is_retained = false;
    m_force_rebuild      = true;
    m_retained_doc_w     = 0.f;
    m_retained_doc_h     = 0.f;
}

void QuadCache::invalidate_all()
{
    ++m_retained_gen;
    m_have_retained      = false;
    m_buffer_is_retained = false;
    m_force_rebuild      = true;
}

void QuadCache::mark_render_subtree(litehtml::render_item* ri)
{
    if(!ri)
    {
        return;
    }
    ri->dc_force_subtree = true;
    ri->dc_dirty_epoch   = m_dirty_epoch;
}

void QuadCache::mark_element_dirty(litehtml::element* el)
{
    if(!el)
    {
        return;
    }

    el->run_on_renderers([this](const std::shared_ptr<litehtml::render_item>& r) {
        mark_render_subtree(r.get());
        return true; // false means "stop"; an inline can own several render items
    });
}

//
// Post-layout dirty walk
//
// lhu_layout() re-renders the whole document, so the only way to know which
// subtrees actually moved is to look. This is a single flat pass reading four
// small structs per node -- measured at 0.009 ms against a 0.107 ms record on
// the 150-row page (8%), against the 0.076 ms traversal it lets us skip.
//
// Dirtiness is sticky: a node stays dirty until a rebuild refreshes the
// retained snapshot, because until then the run in the snapshot really is
// stale. m_dirty_epoch makes clearing every node O(1).
//

void QuadCache::walk(litehtml::render_item* ri, bool force_dirty, bool& out_any_dirty)
{
    const litehtml::position& p  = ri->pos();
    const litehtml::margins&  pd = ri->get_paddings();
    const litehtml::margins&  bd = ri->get_borders();

    const float g[14] = {static_cast<float>(p.x),         static_cast<float>(p.y),
                         static_cast<float>(p.width),     static_cast<float>(p.height),
                         static_cast<float>(pd.left),     static_cast<float>(pd.top),
                         static_cast<float>(pd.right),    static_cast<float>(pd.bottom),
                         static_cast<float>(bd.left),     static_cast<float>(bd.top),
                         static_cast<float>(bd.right),    static_cast<float>(bd.bottom),
                         static_cast<float>(ri->get_scroll_left()), static_cast<float>(ri->get_scroll_top())};

    const uint32_t child_count = static_cast<uint32_t>(ri->children().size());

    bool dirty = force_dirty || !ri->dc_geom_valid || ri->dc_child_count != child_count ||
                 std::memcmp(g, ri->dc_geom, sizeof(g)) != 0;

    if(ri->dc_force_subtree)
    {
        ri->dc_force_subtree = false;
        force_dirty          = true;
        dirty                = true;
    }

    std::memcpy(ri->dc_geom, g, sizeof(g));
    ri->dc_child_count = child_count;
    ri->dc_geom_valid  = true;

    // Sticky from a previous frame that has not been rebuilt since.
    if(ri->dc_dirty_epoch == m_dirty_epoch)
    {
        dirty = true;
    }

    for(const auto& child : ri->children())
    {
        walk(child.get(), force_dirty, out_any_dirty);
        if(child->dc_dirty_epoch == m_dirty_epoch)
        {
            dirty = true;
        }
    }

    if(dirty)
    {
        ri->dc_dirty_epoch = m_dirty_epoch;
        out_any_dirty      = true;
    }
}

bool QuadCache::refresh(const std::shared_ptr<litehtml::render_item>& root)
{
    if(!root)
    {
        return true;
    }

    bool any = false;
    walk(root.get(), false, any);
    return any;
}

//
// Frame planning
//

QuadCache::Plan QuadCache::plan(bool anything_dirty, float doc_w, float doc_h)
{
    const bool dims_match = m_have_retained && doc_w == m_retained_doc_w && doc_h == m_retained_doc_h;

    if(m_force_rebuild || !m_have_retained || !dims_match)
    {
        return Plan::Rebuild;
    }

    if(!anything_dirty && m_buffer_is_retained)
    {
        return Plan::Fast;
    }

    return Plan::Partial;
}

void QuadCache::begin_frame(Plan p)
{
    m_open.clear();
    m_frame_replayed = 0;

    switch(p)
    {
    case Plan::Fast:
        m_capturing = false;
        m_replaying = false;
        ++m_stat_fast;
        break;
    case Plan::Rebuild:
        // Entries recorded from here on belong to the snapshot this frame is
        // about to become; anything older stops validating.
        ++m_retained_gen;
        m_capturing = true;
        m_replaying = false;
        ++m_stat_rebuild;
        break;
    case Plan::Partial:
        m_capturing = false;
        m_replaying = true;
        ++m_stat_partial;
        break;
    }
}

void QuadCache::end_frame(Plan p, float doc_w, float doc_h)
{
    const size_t total = m_container.quad_count();

    switch(p)
    {
    case Plan::Fast:
        // Nothing was drawn, so the whole frame came out of the cache. Count it
        // that way rather than reporting a suspiciously empty frame.
        m_stat_replayed += static_cast<long long>(total);
        break;

    case Plan::Rebuild:
        m_retained.assign(m_container.quads().begin(), m_container.quads().end());
        m_retained_grad.assign(m_container.grad_lut().begin(), m_container.grad_lut().end());
        m_retained_doc_w     = doc_w;
        m_retained_doc_h     = doc_h;
        m_have_retained      = true;
        m_buffer_is_retained = true;
        m_force_rebuild      = false;
        // Every node's cached run now matches the snapshot again.
        ++m_dirty_epoch;
        m_stat_emitted += static_cast<long long>(total);
        break;

    case Plan::Partial:
    {
        m_buffer_is_retained = false;
        const long long emitted = static_cast<long long>(total) - m_frame_replayed;
        m_stat_emitted  += emitted;
        m_stat_replayed += m_frame_replayed;

        // Once a partial frame is re-emitting most of the page there is nothing
        // left to save, and the accumulated dirty flags will never clear on
        // their own. Refresh the snapshot next frame instead.
        if(total > 0 && emitted * 5 > static_cast<long long>(total) * 2)
        {
            m_force_rebuild = true;
        }
        break;
    }
    }

    m_capturing = false;
    m_replaying = false;
    m_open.clear();

    if(m_wipe_pending)
    {
        // Wipe rather than do LRU bookkeeping. Live render items keep their now
        // meaningless slot indices, which is safe only because every lookup
        // checks Entry::owner before believing one.
        m_wipe_pending = false;
        m_entries.clear();
        ++m_retained_gen;
        m_have_retained      = false;
        m_buffer_is_retained = false;
        m_force_rebuild      = true;
    }
}

//
// The hot path
//

int QuadCache::open(litehtml::render_item* ri, int flag, float x, float y)
{
    const int slot_index = flag - static_cast<int>(litehtml::draw_block);
    if(slot_index < 0 || slot_index > 2)
    {
        return 0;
    }

    const uint32_t s = ri->dc_slot[slot_index];

    if(m_replaying)
    {
        if(s >= 2 && s < m_entries.size())
        {
            const Entry& e = m_entries[s];
            if(e.owner == ri && e.retained_gen == m_retained_gen && e.flag == flag &&
               ri->dc_dirty_epoch != m_dirty_epoch && e.ox == x && e.oy == y &&
               same_clip(e.clip, m_container.active_clip_rect()) && e.off + e.len <= m_retained.size())
            {
                const size_t dst = m_container.quad_count();
                m_container.append_quads(m_retained.data() + e.off, e.len);

                if(e.grad_rows > 0)
                {
                    const int base = m_container.grad_rows_recorded();
                    m_container.append_grad_rows(m_retained_grad.data() +
                                                     static_cast<size_t>(e.grad_off) * kGradRowBytes,
                                                 e.grad_rows);
                    const int delta = base - e.grad_base;
                    if(delta != 0)
                    {
                        LhuQuad* q = m_container.quads_at(dst);
                        for(uint32_t i = 0; i < e.len; ++i)
                        {
                            if(q[i].grad_row >= 0)
                            {
                                q[i].grad_row += delta;
                            }
                        }
                    }
                }

                m_frame_replayed += e.len;
                ++m_stat_runs;
                return 1;
            }
        }
        return 0;
    }

    if(!m_capturing)
    {
        return 0;
    }

    // Leaves are not worth an entry. A text run or an empty box produces a
    // handful of quads that its parent's entry already covers, and the capture
    // is pure overhead on the one frame that pays for the whole cache -- the
    // first record of a freshly parsed document, where every entry is a miss.
    // Measured: skipping leaves removes about half the open/close traffic on
    // the 150-row page.
    if(ri->children().empty())
    {
        return 0;
    }

    // A run that turned out to be too short to be worth an entry stays that way
    // -- re-checking it every rebuild would cost more than it saves.
    if(s >= 2 && s < m_entries.size() && m_entries[s].owner == ri && !m_entries[s].cacheable)
    {
        return 0;
    }

    uint32_t slot = s;
    if(slot < 2 || slot >= m_entries.size() || m_entries[slot].owner != ri || m_entries[slot].flag != flag)
    {
        if(m_entries.size() >= kMaxEntries)
        {
            // Out of room. Do NOT wipe here: m_open holds indices into this very
            // table. Stop capturing for the rest of the frame and clear it from
            // end_frame instead.
            m_wipe_pending = true;
            return 0;
        }
        if(m_entries.size() < 2)
        {
            m_entries.resize(2); // 0 and 1 are the "never seen" sentinels
        }
        slot = static_cast<uint32_t>(m_entries.size());
        m_entries.emplace_back();
        m_entries[slot].owner = ri;
        m_entries[slot].flag  = flag;
        ri->dc_slot[slot_index] = slot;
    }

    Entry& e = m_entries[slot];
    e.ox     = x;
    e.oy     = y;
    e.clip   = m_container.active_clip_rect();

    m_open.push_back(
        OpenCapture {slot, static_cast<uint32_t>(m_container.quad_count()), m_container.grad_rows_recorded()});
    return 2;
}

void QuadCache::close()
{
    if(m_open.empty())
    {
        return;
    }

    const OpenCapture oc = m_open.back();
    m_open.pop_back();

    Entry& e = m_entries[oc.slot];

    const uint32_t end = static_cast<uint32_t>(m_container.quad_count());
    e.off              = oc.quad_start;
    e.len              = end - oc.quad_start;

    const int grad_end = m_container.grad_rows_recorded();
    e.grad_base        = oc.grad_start;
    e.grad_off         = static_cast<uint32_t>(oc.grad_start);
    e.grad_rows        = grad_end - oc.grad_start;

    e.retained_gen = m_retained_gen;
    e.cacheable    = e.len >= kMinRun;
}

} // namespace lhu
