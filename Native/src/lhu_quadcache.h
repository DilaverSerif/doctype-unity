// Doctype — retained display list for lhu_record(). EXPERIMENT E2.
//
// WHY THIS SHAPE
//
// Measured first (tests/probe_record.cpp), on the four bench pages:
//
//   record cost      = litehtml's draw traversal  71-78%
//                    + turning callbacks into quads  22-29%
//
// So a cache that memoizes *emission* but still lets litehtml walk the tree
// cannot do better than ~1.2-1.4x, and the per-node bookkeeping would eat most
// of that. The only version worth building is one that skips the *traversal*:
// a clean subtree is not walked at all, its quads are memcpy'd into the output
// buffer, and litehtml never sees it.
//
// The unit of caching is one iteration of render_item::draw_children's child
// loop — one child, one pass. litehtml finishes a child and all of its
// recursion before moving to the next, so that iteration's quads are contiguous
// in the output buffer, which is what makes splicing sound. Ordering is
// preserved exactly: a cached run is appended at the same point in the same
// traversal the uncached run would have been.
//
// litehtml draws in three passes over the whole tree (block backgrounds, then
// floats, then inlines), so one subtree owns three separate runs. They are
// cached independently, one entry per (render item, pass).
//
// WHAT INVALIDATES WHAT — see the notes on each invalidate_*/mark_* below.
//
// FRAME MODES
//
//   fast     nothing is dirty and the output buffer still holds the retained
//            frame -> return it untouched, zero work.
//   rebuild  no usable retained frame (new document, atlas grew, too much has
//            gone dirty) -> full traversal with capture, then the frame becomes
//            the new retained snapshot.
//   partial  replay clean subtrees, re-emit dirty ones. Nothing is captured:
//            the retained snapshot and every entry offset stay exactly as they
//            were, which is what keeps offsets valid without a rebase pass.

#ifndef LHU_QUADCACHE_H
#define LHU_QUADCACHE_H

#include "lhu_container.h"
#include "lhu_types.h"

#include <litehtml.h>
#include <litehtml/render_item.h>

#include <cstdint>
#include <vector>

namespace lhu
{

class QuadCache
{
  public:
    explicit QuadCache(Container& container) :
        m_container(container)
    {
        m_hooks.ctx            = this;
        m_hooks.open           = &QuadCache::hook_open;
        m_hooks.close          = &QuadCache::hook_close;
        m_hooks.styles_changed = &QuadCache::hook_styles_changed;
        m_hooks.geometry_changed = &QuadCache::hook_geometry_changed;
    }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool on) { m_enabled = on; }

    // True once since the last call if a pseudo-class restyle changed a value
    // layout reads. Read-and-clear, because the interesting window is exactly
    // one input event: the caller brackets an on_mouse_* call with it.
    bool take_geometry_changed()
    {
        const bool v = m_geometry_changed;
        m_geometry_changed = false;
        return v;
    }

    const litehtml::document::draw_cache_hooks* hooks() const { return &m_hooks; }

    // A different document altogether: every entry belongs to render items that
    // no longer exist.
    void on_document_replaced();

    // Everything recorded so far is unusable. The two that matter:
    //
    //  * the glyph atlas grew. Atlas::reset() repacks from scratch, so every
    //    single UV in every cached quad now points at the wrong texels. This is
    //    the case most likely to serve garbage, and lhu_record()'s existing
    //    retry loop is not enough on its own -- the retry has to run with the
    //    cache emptied, or attempt 2 happily replays attempt 1's stale UVs.
    //  * lhu_set_device_scale / lhu_set_viewport / a font registration /
    //    a master-css switch, all of which move geometry or metrics wholesale.
    void invalidate_all();

    // Marks one element's render subtree stale. Used by lhu_set_text (the text
    // and its measured width changed) and by the :hover / :active restyle hook
    // (colours changed, geometry usually did not, so the geometry diff alone
    // would not have noticed).
    void mark_element_dirty(litehtml::element* el);

    // Walks the render tree and marks every subtree whose geometry moved since
    // the last walk. This is what catches an ordinary re-layout: lhu_layout()
    // re-renders the whole document every frame, and almost every node lands
    // back on the same pixel.
    //
    // Returns true when anything at all is dirty.
    bool refresh(const std::shared_ptr<litehtml::render_item>& root);

    // --- frame lifecycle, driven by lhu_record ------------------------------

    enum class Plan
    {
        Fast,    // reuse the buffer in place, do not draw
        Rebuild, // full traversal, capture everything
        Partial  // replay what is clean, re-emit what is not
    };

    Plan plan(bool anything_dirty, float doc_w, float doc_h);

    void begin_frame(Plan p);
    void end_frame(Plan p, float doc_w, float doc_h);

    // --- stats (for the bench / verifier; free when nobody asks) ------------

    long long quads_replayed() const { return m_stat_replayed; }
    long long quads_emitted() const { return m_stat_emitted; }
    long long runs_replayed() const { return m_stat_runs; }
    long long frames_fast() const { return m_stat_fast; }
    long long frames_rebuild() const { return m_stat_rebuild; }
    long long frames_partial() const { return m_stat_partial; }

    // Everything the cache holds on to, minus the ~80 bytes per render item it
    // stores on the nodes themselves. Worth watching on mobile: the retained
    // snapshot alone is 144 bytes per quad.
    long long bytes() const
    {
        return static_cast<long long>(m_retained.capacity() * sizeof(LhuQuad) + m_retained_grad.capacity() +
                                      m_entries.capacity() * sizeof(Entry) + m_open.capacity() * sizeof(OpenCapture));
    }
    void      reset_stats()
    {
        m_stat_replayed = m_stat_emitted = m_stat_runs = 0;
        m_stat_fast = m_stat_rebuild = m_stat_partial = 0;
    }

  private:
    struct Entry
    {
        // The node this entry belongs to. Checked on every lookup: a slot index
        // stored on a render item must never be able to name somebody else's
        // entry, however the entry table has been recycled since.
        const litehtml::render_item* owner = nullptr;

        uint32_t retained_gen = 0; // which retained snapshot this run lives in
        int32_t  flag         = -1;

        float ox = 0.f, oy = 0.f; // translation the run was recorded under
        ClipRect clip;            // clip in force when it was recorded

        uint32_t off = 0; // offset into m_retained
        uint32_t len = 0;

        // Gradient LUT rows this run baked, if any. The LUT is rebuilt from
        // scratch every frame in draw order, so a replayed run has to re-append
        // its rows and shift its quads' grad_row by however far the LUT has
        // moved on.
        uint32_t grad_off  = 0; // into m_retained_grad, in rows
        int32_t  grad_rows = 0;
        int32_t  grad_base = 0; // LUT row count when the run started recording

        bool cacheable = true; // false once a run turns out too small to bother
    };

    // Deliberately three words: the origin and clip are written straight into
    // the entry at open time, so this stack stays small enough to be free.
    struct OpenCapture
    {
        uint32_t slot;
        uint32_t quad_start;
        int32_t  grad_start;
    };

    static int  hook_open(void* ctx, litehtml::render_item* ri, int flag, float x, float y);
    static void hook_close(void* ctx);
    static void hook_styles_changed(void* ctx, litehtml::element* el);

    static void hook_geometry_changed(void* ctx)
    {
        static_cast<QuadCache*>(ctx)->m_geometry_changed = true;
    }

    int  open(litehtml::render_item* ri, int flag, float x, float y);
    void close();

    void walk(litehtml::render_item* ri, bool force_dirty, bool& out_any_dirty);
    void mark_render_subtree(litehtml::render_item* ri);

    Container& m_container;

    litehtml::document::draw_cache_hooks m_hooks {};

    bool m_geometry_changed = false;

    bool m_enabled = true;

    std::vector<Entry>       m_entries;
    std::vector<OpenCapture> m_open;

    // The frame every entry points into. Frozen until the next rebuild, which
    // is precisely why an entry's (off, len) never needs rebasing.
    std::vector<LhuQuad> m_retained;
    std::vector<uint8_t> m_retained_grad;

    uint32_t m_retained_gen = 0; // bumped by every rebuild and every invalidate
    uint32_t m_dirty_epoch  = 1; // bumped by every rebuild; nodes compare against it

    bool  m_have_retained  = false;
    bool  m_buffer_is_retained = false;
    float m_retained_doc_w = 0.f;
    float m_retained_doc_h = 0.f;

    bool m_capturing = false;
    bool m_replaying = false;

    // Set when the last partial frame had to re-emit too much of the document
    // to be worth continuing incrementally.
    bool m_force_rebuild = true;

    // The entry table filled up mid-frame; it is cleared from end_frame, where
    // nothing is holding an index into it.
    bool m_wipe_pending = false;

    long long m_frame_replayed = 0;

    // High-water mark of the entry table, so re-parsing the same page does not
    // pay for the same run of reallocations every time.
    size_t m_peak_entries = 0;

    long long m_stat_replayed = 0;
    long long m_stat_emitted  = 0;
    long long m_stat_runs     = 0;
    long long m_stat_fast     = 0;
    long long m_stat_rebuild  = 0;
    long long m_stat_partial  = 0;

    // A run shorter than this is not worth a cache entry: replaying it saves
    // less than the lookup costs. Measured at 3.8 ns to emit a quad and 2.3 ns
    // to copy one (probe_record.cpp), against ~8 ns for the lookup.
    static constexpr uint32_t kMinRun = 4;

    // Hard ceiling on entry-table growth for a long-lived document that keeps
    // creating render items (lhu_set_text's structural path does). Overflow
    // wipes rather than doing LRU bookkeeping; the owner check makes a wipe
    // safe even though stale slot indices survive on live render items.
    static constexpr size_t kMaxEntries = 1u << 17;
};

} // namespace lhu

#endif // LHU_QUADCACHE_H
