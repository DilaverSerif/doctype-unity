#include "lhu_api.h"
#include "lhu_container.h"
#include "lhu_font.h"
#include "lhu_master_css.h"
#include "lhu_quadcache.h"

#include <litehtml.h>
#include <litehtml/el_space.h>
#include <litehtml/el_text.h>
#include <litehtml/render_inline.h>
#include <litehtml/render_item.h>

#include <cstring>
#include <cmath>
#include <algorithm>
#include <functional> // libstdc++ does not include it transitively; GCC needs it
#include <memory>
#include <string>
#include <vector>

#define LHU_VERSION_STRING "0.1.0"

// EXPERIMENT E2: the retained display list is switched on and off at runtime,
// not at build time. Adding a pass to the benchmark has already been shown to
// move the timings of untouched code by 10-28% on this project, so an A/B
// between two separately linked binaries proves nothing. One binary, one env
// var, read once -- the same reasoning as LHU_EXP_SUBTREE above it.
//
//   LHU_EXP_QUADCACHE=0   off  -- lhu_record() behaves exactly as before
//   anything else / unset on   -- default
static bool quadcache_enabled_by_env()
{
    const char* v = std::getenv("LHU_EXP_QUADCACHE");
    return !(v && std::strcmp(v, "0") == 0);
}

struct LhuContext
{
    lhu::FontManager fonts;
    lhu::Container   container;
    lhu::QuadCache   cache;

    litehtml::document::ptr doc;

    float       doc_width  = 0.f;
    float       doc_height = 0.f;
    std::string last_error;
    int32_t     master_css_mode = LHU_MASTER_CSS_GAME_UI;

    // --- incremental layout state ------------------------------------------
    //
    // exp_subtree      the runtime toggle; see lhu_exp_set_enabled().
    // layout_clean     the render tree's geometry currently matches the DOM,
    //                  i.e. a full render has run and nothing has invalidated
    //                  it since. False until the first lhu_layout().
    // pending_mutation lhu_set_text() has changed something since that render.
    //                  Without this, two lhu_layout() calls in a row would make
    //                  the second one free, which is true but would quietly
    //                  change what the benchmark's "layout+record only" line
    //                  measures. The short-circuit is deliberately limited to
    //                  the mutation case it was built for.
    // layout_width     the max_width the current layout was produced at. A
    //                  different width is a different layout, always.
    bool    exp_subtree      = true;
    bool    layout_clean     = false;
    bool    pending_mutation = false;

    // A vw/vh length is resolved when a style is *computed*, which happens once
    // at parse time -- so a viewport change after that leaves every viewport
    // relative length answering for the size the page was loaded at. Recomputing
    // eagerly inside lhu_set_viewport() would pay for a style pass per call
    // during a resize drag, so the viewport marks and the next layout collects.
    bool    styles_stale     = false;
    float   layout_width     = 0.f;
    int32_t stat_skipped     = 0;
    int32_t stat_rendered    = 0;

    // --- dirty region --------------------------------------------------------
    //
    // A copy of the last recorded frame, kept so lhu_record can say which part
    // of the new one actually differs. A copy rather than a pointer into the
    // container's buffer, because the next record overwrites that buffer in
    // place. ~150 KB for a busy page; the GPU work it saves is measured in
    // milliseconds per frame.
    std::vector<LhuQuad>  prev_quads;
    std::vector<uint8_t>  prev_grad_lut;
    float                 prev_doc_w = -1.f;
    float                 prev_doc_h = -1.f;
    bool                  have_prev_frame = false;

    // The one change the byte-diff cannot see: the host saying its image atlas
    // texels moved under UVs that stayed put (LHU_QC_INVALIDATE). Everything
    // else -- restyles, rebuilds, atlas repacks -- lands in the quad bytes and
    // the diff catches it on its own.
    bool                  dirty_suspect = false;

    // --- scroll fast path ----------------------------------------------------
    //
    // Net scroll delta applied since the last record, kept as a HYPOTHESIS for
    // compute_dirty_region: "this frame might be the previous one translated".
    // The diff then verifies every changed quad against the translation and
    // falls back to the plain rect diff on the first quad it cannot explain,
    // so a wrong or stale hint can only cost speed, never pixels. One scroll
    // view only: two views scrolling between the same pair of records is not a
    // single translation, and `multi` disarms the attempt.
    bool  scroll_hint_any   = false;
    bool  scroll_hint_multi = false;
    float scroll_hint_box_x = 0.f, scroll_hint_box_y = 0.f;
    float scroll_hint_dx    = 0.f, scroll_hint_dy    = 0.f;

    void clear_scroll_hint()
    {
        scroll_hint_any   = false;
        scroll_hint_multi = false;
        scroll_hint_dx    = 0.f;
        scroll_hint_dy    = 0.f;
    }

    // --- retained display list state (experiment E2) -------------------------
    //
    // atlas_epoch  the last font-atlas epoch the cache was reconciled with. A
    //              change means the atlas was repacked and every cached UV is
    //              pointing at the wrong texels.
    int atlas_epoch = -1;

    // --- inline style mutation state (experiment E7) -------------------------
    //
    // exp_setstyle       the runtime toggle; see lhu_exp_setstyle_set_enabled().
    // stat_style_applied lhu_set_style() calls that actually restyled something.
    // stat_style_trees   of those, the ones that had to rebuild the render tree
    //                    because display/float/position moved under it.
    // style_deep_refresh is a measurement knob, not a feature: it makes
    // lhu_set_style() run litehtml's own recursive refresh_styles() over the
    // whole subtree instead of the self-only variant, so the cost of the
    // recursion can be A/B'd in one binary. Set LHU_SETSTYLE_DEEP=1 to enable.
    // See lhu_api.h for what the difference actually is.
    // style_memoize_inline is the other measurement knob: leaving it on makes
    // lhu_set_style() feed every one-shot style string it parses into the E6
    // inline-style memo, which is what happens if that interaction is not
    // handled at all. Set LHU_SETSTYLE_MEMO=1 to measure the difference.
    bool    exp_setstyle         = true;
    bool    style_deep_refresh   = false;
    bool    style_memoize_inline = false;
    int32_t stat_style_applied = 0;
    int32_t stat_style_trees   = 0;

    // --- subtree relayout state (experiment E8) ------------------------------
    //
    // When a mutation dirtied geometry inside exactly one element, that
    // element is a CANDIDATE: lhu_layout() may re-lay just its nearest block
    // ancestor in place and verify the footprint did not move, instead of
    // rendering the document. Everything else that can invalidate layout goes
    // through invalidate_layout(), which drops the candidate -- so a live
    // candidate *means* "the only stale geometry is inside this subtree".
    //
    // doc_has_floats / doc_has_positioned are one-time facts about the loaded
    // document: floats make formatting contexts leak across subtree edges,
    // and absolutely positioned boxes are laid out by a separate document
    // pass a subtree render does not run. Either one disarms E8 wholesale
    // rather than reasoning about where they are.
    bool                             exp_sublayout = true;
    std::weak_ptr<litehtml::element> sublayout_candidate;
    bool                             sublayout_multi   = false;
    bool                             doc_has_floats     = false;
    bool                             doc_has_positioned = false;
    int32_t                          stat_sub_layouts   = 0;
    int32_t                          stat_sub_fallbacks = 0;

    void note_sublayout_candidate(const litehtml::element::ptr& el)
    {
        const auto current = sublayout_candidate.lock();
        if(current && current != el)
        {
            sublayout_multi = true;
            return;
        }
        sublayout_candidate = el;
    }

    explicit LhuContext(const LhuHostCallbacks& host) :
        container(fonts, host),
        cache(container)
    {
        const char* env = std::getenv("LHU_EXP_SUBTREE");
        exp_subtree     = !(env && std::strcmp(env, "0") == 0);

        const char* env_style = std::getenv("LHU_EXP_SETSTYLE");
        exp_setstyle          = !(env_style && std::strcmp(env_style, "0") == 0);

        const char* env_deep = std::getenv("LHU_SETSTYLE_DEEP");
        style_deep_refresh   = env_deep && std::strcmp(env_deep, "0") != 0;

        const char* env_memo = std::getenv("LHU_SETSTYLE_MEMO");
        style_memoize_inline = env_memo && std::strcmp(env_memo, "0") != 0;

        const char* env_sub = std::getenv("LHU_EXP_SUBLAYOUT");
        exp_sublayout       = !(env_sub && std::strcmp(env_sub, "0") == 0);

        cache.set_enabled(quadcache_enabled_by_env());
    }

    // Hooks live on the document, so they have to be reattached to each new one.
    void attach_cache()
    {
        if(doc)
        {
            doc->set_draw_cache(cache.enabled() ? cache.hooks() : nullptr);
        }
    }

    // Anything that can move geometry without going through lhu_set_text() has
    // to land here. Erring towards calling it costs one full render; forgetting
    // to call it draws a frame with stale positions, so every entry point that
    // touches the document, the viewport, the fonts or the hover/scroll state
    // invalidates unconditionally rather than trying to prove it did nothing.
    void invalidate_layout()
    {
        layout_clean     = false;
        pending_mutation = false;

        // Whatever called this dirtied more than one element's subtree (a
        // viewport, a scroll, a hover), so the candidate no longer describes
        // all the stale geometry.
        sublayout_candidate.reset();
        sublayout_multi = false;
    }

    // Deliberately NOT folded into invalidate_layout(). The two experiments
    // need different sets: E1 has to invalidate on every mouse and scroll event
    // that reports a change, because a :hover rule can resize a box and E1 has
    // no way to see that. E2 can see it -- element::find_styles_changes calls
    // the styles_changed hook for exactly the elements that were restyled, and
    // a scroll shows up in the render item's scroll offsets, which are two of
    // the fourteen floats the post-layout diff walk compares. Calling
    // invalidate_all() from those paths would throw the retained frame away on
    // every hover and give back the whole hover speedup for nothing.
    void invalidate_layout_and_cache()
    {
        invalidate_layout();
        cache.invalidate_all();
    }
};

namespace
{
// litehtml's hit-test entry points want a redraw-box sink; we repaint the whole
// view every frame, so we only care whether anything changed at all.
const std::function<void(const litehtml::position&)> kIgnoreRedraw = [](const litehtml::position&) {};

// One node's worth of text, as the HTML parser would have produced it.
//
// litehtml does not keep an element's text in one node: document::create_node()
// runs it through document_container::split_text(), which emits an alternating
// run of words (el_text) and single whitespace characters (el_space). Line
// breaking is driven entirely by that split -- a line box can only break
// between nodes -- so a mutation that collapsed "Esya 12" into a single node
// would silently stop wrapping and lay out differently from the same text
// coming out of the parser. lhu_set_text therefore reproduces the split exactly.
struct TextNodeSpec
{
    std::string text;
    bool        is_space = false;
};

void split_like_parser(litehtml::document_container& container, const char* utf8, std::vector<TextNodeSpec>& out)
{
    container.split_text(
        utf8, [&out](const char* word) { out.push_back(TextNodeSpec {std::string(word), false}); },
        [&out](const char* space) { out.push_back(TextNodeSpec {std::string(space), true}); });
}

// --- experiment E7: what a style change can move that layout cannot fix -------
//
// document::render() walks the render tree; it does not build it. The tree is
// built once, in createFromString(), and every node's *class* was chosen from
// the computed `display` of the element it wraps -- render_item_block,
// render_item_inline, render_item_flex, render_item_table, or no node at all for
// display:none. render_item::init() then reads the same values again to split
// inlines around block children, wrap loose inlines in anonymous boxes and build
// the table boxes.
//
// So a style change that moves an element between those categories invalidates
// the *shape* of the render tree, and no amount of re-rendering will notice:
// litehtml re-lays-out the tree it has. That is the one class of inline-style
// change lhu_set_style() cannot serve by recomputing styles alone, and this
// fingerprint is how it is detected -- taken over the mutated element and every
// descendant, because `display: inherit` and a font-size-driven change can move
// one of them without moving the element itself.
//
// `float` and `position` are in the key for safety rather than from a proven
// failure: fetch_positioned() is re-run on every render and floats are resolved
// during rendering, so both are believed to be handled without a rebuild. They
// cost one byte of the key each and a wrong guess there would be a silent
// mislayout, which is not a trade worth taking.
// The node's identity travels with the key. Restyling is not supposed to touch the
// DOM at all, but one path does: el_before_after_base::add_style() re-reads `content`
// and REPLACES the pseudo element's children with new nodes, so re-applying a
// ::before selector detaches the generated content from the render items that lay it
// out. refresh_styles_self() avoids that path entirely; the pointer is here so that
// if anything ever takes it -- LHU_SETSTYLE_DEEP=1 does, by design -- the mismatch is
// caught and answered with a render-tree rebuild instead of a stale frame.
//
// The snapshot holds a strong reference, not a bare address. If it held an address, a
// node destroyed by the restyle could be replaced by a new node that the allocator
// happened to put at the same address, and the comparison would call a rebuilt subtree
// unchanged. Keeping the old nodes alive until both vectors have been compared makes
// pointer equality mean what it reads as. One refcount per element in the subtree,
// against a compute_styles() over the same subtree.
struct StructuralKey
{
    litehtml::element::ptr node;
    uint32_t               key = 0;

    bool operator!=(const StructuralKey& o) const
    {
        return node != o.node || key != o.key;
    }
};

void collect_structure(const litehtml::element::ptr& el, std::vector<StructuralKey>& out)
{
    const litehtml::css_properties& c = el->css();
    out.push_back(StructuralKey {el, static_cast<uint32_t>(c.get_display()) |
                                         (static_cast<uint32_t>(c.get_float()) << 8) |
                                         (static_cast<uint32_t>(c.get_position()) << 16)});
    for(const auto& child : el->children())
    {
        collect_structure(child, out);
    }
}

bool structure_changed(const std::vector<StructuralKey>& a, const std::vector<StructuralKey>& b)
{
    if(a.size() != b.size())
    {
        return true;
    }
    for(size_t i = 0; i < a.size(); ++i)
    {
        if(a[i] != b[i])
        {
            return true;
        }
    }
    return false;
}

} // namespace

// --- C ABI exception boundary ----------------------------------------------
//
// No C++ exception may unwind through the extern "C" functions below: the
// caller is a P/Invoke frame, and an exception crossing it is undefined
// behaviour that surfaces on Android as an unexplained process abort with a
// foreign stack. Every export whose body does more than read a field runs as
// a function-try-block closed by one of these handlers, which turn a throw
// into the function's failure return plus lhu_last_error().
//
// The rule for new exports: end the body with LHU_API_CATCH(ctx, fallback)
// (or the _VOID variant). Only a body that provably cannot throw -- returning
// a constant or copying scalars -- may go without.

namespace
{
// The handler itself must not throw: an exception leaving a catch clause of a
// function-try-block propagates to the caller, which is exactly the hole this
// boundary closes. The assignment can allocate; if that fails too, fall back
// to clear(), which cannot.
void set_last_error_noexcept(LhuContext* ctx, const char* what) noexcept
{
    if(!ctx)
    {
        return;
    }
    try
    {
        ctx->last_error = what;
    }
    catch(...)
    {
        ctx->last_error.clear();
    }
}
} // namespace

#define LHU_API_CATCH(ctx, fallback)                                    \
    catch(const std::exception& e)                                      \
    {                                                                   \
        set_last_error_noexcept(ctx, e.what());                         \
        return fallback;                                                \
    }                                                                   \
    catch(...)                                                          \
    {                                                                   \
        set_last_error_noexcept(ctx, "unknown native exception");       \
        return fallback;                                                \
    }

#define LHU_API_CATCH_VOID(ctx)                                         \
    catch(const std::exception& e)                                      \
    {                                                                   \
        set_last_error_noexcept(ctx, e.what());                         \
    }                                                                   \
    catch(...)                                                          \
    {                                                                   \
        set_last_error_noexcept(ctx, "unknown native exception");       \
    }

extern "C" {

const char* lhu_version(void)
{
    return LHU_VERSION_STRING;
}

int32_t lhu_quad_size(void)
{
    return static_cast<int32_t>(sizeof(LhuQuad));
}

LhuContext* lhu_create(const LhuHostCallbacks* host)
try
{
    LhuHostCallbacks cb {};
    if(host)
    {
        cb = *host;
    }
    // nothrow covers the allocation itself; the try covers the constructor,
    // which builds the font manager and container and is not proven
    // throw-free. There is no context to carry lhu_last_error() yet, so a
    // failed create is just null.
    return new(std::nothrow) LhuContext(cb);
}
catch(...)
{
    return nullptr;
}

void lhu_destroy(LhuContext* ctx)
try
{
    delete ctx;
}
catch(...)
{
    // Destructors are noexcept by default, so this is expected to be
    // unreachable -- but "expected" is not a boundary guarantee.
}

int32_t lhu_register_font(LhuContext* ctx, const char* family, int32_t weight, int32_t italic, const uint8_t* data,
                          int32_t len)
try
{
    if(!ctx || len <= 0)
    {
        return 0;
    }

    // A new face can change metrics and will certainly repack the atlas the
    // first time it is used.
    ctx->invalidate_layout_and_cache();

    const bool ok = ctx->fonts.register_font(family, weight, italic != 0, data, static_cast<size_t>(len));
    if(!ok)
    {
        ctx->last_error = std::string("failed to parse font: ") + (family ? family : "(null)");
    }
    return ok ? 1 : 0;
}
LHU_API_CATCH(ctx, 0)

void lhu_set_default_font(LhuContext* ctx, const char* family, float size)
try
{
    if(!ctx)
    {
        return;
    }

    ctx->invalidate_layout_and_cache();
    ctx->fonts.set_default_family(family);
    ctx->container.set_default_font(family ? family : "sans-serif", size > 0.f ? size : 16.f);
}
LHU_API_CATCH_VOID(ctx)

void lhu_set_master_css(LhuContext* ctx, int32_t mode)
try
{
    if(ctx)
    {
        ctx->invalidate_layout_and_cache();
        ctx->master_css_mode = mode;
    }
}
LHU_API_CATCH_VOID(ctx)

void lhu_set_viewport(LhuContext* ctx, float width, float height)
try
{
    if(ctx && ctx->container.set_viewport(width, height))
    {
        // vw/vh units and the containing block's height both come from here;
        // for the cache, the viewport also feeds media queries and the root
        // background's paint box, so a change can move anything.
        ctx->invalidate_layout_and_cache();
        ctx->styles_stale = true;
    }
}
LHU_API_CATCH_VOID(ctx)

void lhu_set_device_scale(LhuContext* ctx, float scale)
try
{
    // The same-value guard is load-bearing: the host re-sends the scale on
    // every layout, and treating each of those as a change invalidated the
    // layout AND the retained quad cache once per frame -- which quietly
    // disarmed every layout fast path and made the cache rebuild 300 times
    // per benchmark scenario. The stats column was saying so all along.
    if(ctx && ctx->container.set_device_scale(scale))
    {
        ctx->invalidate_layout_and_cache();
    }
}
LHU_API_CATCH_VOID(ctx)

int32_t lhu_load_html(LhuContext* ctx, const char* html, const char* user_css)
try
{
    if(!ctx || !html)
    {
        return 0;
    }

    ctx->invalidate_layout();

    // A scroll hint describes the outgoing document's viewport; the new
    // document must not inherit it.
    ctx->clear_scroll_hint();

    if(!ctx->fonts.has_fonts())
    {
        ctx->last_error = "no fonts registered; call lhu_register_font first";
        return 0;
    }

    try
    {
        const litehtml::estring source(std::string(html), litehtml::encoding::utf_8);

        const char* master = ctx->master_css_mode == LHU_MASTER_CSS_FULL ? litehtml::master_css
                                                                        : lhu::trimmed_master_css();

        ctx->doc = litehtml::document::createFromString(source, &ctx->container, master,
                                                        user_css ? std::string(user_css) : std::string());
    }
    catch(const std::exception& e)
    {
        ctx->last_error = e.what();
        ctx->doc.reset();
        return 0;
    }

    if(!ctx->doc)
    {
        ctx->last_error = "document parse returned null";
        return 0;
    }

    // Every cache entry belonged to render items of the document that just went
    // away, and the hooks live on the document object, so both have to be redone
    // here. (A parse that threw leaves ctx->doc null, so nothing can consult a
    // stale entry before the next successful load gets here.)
    ctx->cache.on_document_replaced();
    ctx->attach_cache();

    // createFromString() computed every style against the viewport as it stands
    // now, so whatever marked them stale has just been answered.
    ctx->styles_stale = false;

    // One-time facts the subtree relayout (E8) gates on; see LhuContext.
    ctx->doc_has_floats     = false;
    ctx->doc_has_positioned = false;
    if(const auto root = ctx->doc->root())
    {
        const std::function<void(const litehtml::element::ptr&)> scan = [&](const litehtml::element::ptr& e) {
            if(e->css().get_float() != litehtml::float_none)
            {
                ctx->doc_has_floats = true;
            }
            if(e->css().get_position() == litehtml::element_position_absolute ||
               e->css().get_position() == litehtml::element_position_fixed)
            {
                ctx->doc_has_positioned = true;
            }
            for(const auto& c : e->children())
            {
                scan(c);
            }
        };
        scan(root);
    }

    ctx->last_error.clear();
    return 1;
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_set_text(LhuContext* ctx, const char* selector, const char* utf8)
try
{
    if(!ctx || !ctx->doc || !selector || !utf8)
    {
        return 0;
    }

    const litehtml::element::ptr root = ctx->doc->root();
    if(!root)
    {
        ctx->last_error = "set_text: no document";
        return 0;
    }

    const litehtml::element::ptr el = root->select_one(std::string(selector));
    if(!el)
    {
        ctx->last_error = std::string("set_text: no element matched '") + selector + "'";
        return 0;
    }

    // Refuse to touch an element that holds anything other than text. Replacing
    // its contents would destroy child elements, and the render-item surgery
    // below is only sound for a leaf run of text nodes.
    for(const auto& child : el->children())
    {
        if(!child->is_text())
        {
            ctx->last_error = std::string("set_text: '") + selector + "' contains non-text children";
            return 0;
        }
    }

    // The render tree is not a copy of the DOM: render_item_block::init()
    // replaces itself with an inline/block context, splits inline elements that
    // straddle a block child, and wraps loose inlines in anonymous boxes; flex
    // and table containers do more of the same. Rather than trying to predict
    // which of those applies, assert the one shape this call knows how to
    // update -- a single render item whose children are exactly this element's
    // text nodes, in order, each of them a leaf -- and bail out otherwise.
    std::shared_ptr<litehtml::render_item> ri;
    int                                    render_count = 0;
    el->run_on_renderers([&](const std::shared_ptr<litehtml::render_item>& r) {
        ++render_count;
        ri = r;
        return true;
    });

    if(render_count != 1 || !ri)
    {
        ctx->last_error = std::string("set_text: '") + selector + "' has " + std::to_string(render_count) +
                          " render items, expected 1";
        return 0;
    }

    {
        auto& render_children = ri->children();
        if(render_children.size() != el->children().size())
        {
            ctx->last_error = std::string("set_text: '") + selector + "' render children do not map 1:1 onto its DOM children";
            return 0;
        }

        auto dom_it = el->children().begin();
        for(const auto& render_child : render_children)
        {
            if(render_child->src_el() != *dom_it || !render_child->children().empty())
            {
                ctx->last_error =
                    std::string("set_text: '") + selector + "' render children do not map 1:1 onto its DOM children";
                return 0;
            }
            ++dom_it;
        }
    }

    std::vector<TextNodeSpec> spec;
    split_like_parser(ctx->container, utf8, spec);

    // Same node count and the same word/space pattern? Then nothing structural
    // changes and the existing nodes can simply be rewritten in place. This is
    // the path a counter takes ("Esya 12" -> "Esya 13"): no allocation, no
    // render-item churn, just a re-measure of the nodes whose text differs.
    bool same_shape = spec.size() == el->children().size();
    if(same_shape)
    {
        size_t i = 0;
        for(const auto& child : el->children())
        {
            if(child->is_space() != spec[i].is_space)
            {
                same_shape = false;
                break;
            }
            ++i;
        }
    }

    ctx->last_error.clear();

    if(same_shape)
    {
        bool   changed          = false;
        bool   geometry_changed = false;
        size_t i                = 0;
        for(const auto& child : el->children())
        {
            auto* text_node = static_cast<litehtml::el_text*>(child.get());
            if(text_node->text() != spec[i].text)
            {
                const litehtml::size before       = text_node->measured_size();
                const bool           before_break = text_node->is_break();

                text_node->set_data(spec[i].text.c_str());
                // compute_styles() is what turns the new string into a width via
                // container()->text_width(). Skipping it leaves m_size holding the
                // old measurement, and the next render would place a new string
                // inside the old string's box.
                text_node->compute_styles(false);
                changed = true;

                // The whole incremental-layout argument rests on this
                // comparison, so it is exact -- no epsilon. A text node feeds
                // layout through get_content_size(), which returns m_size and
                // nothing else, so two runs with bit-identical m_size for every
                // node take bit-identical decisions in can_hold(), new_box(),
                // place_to_left() and every block height derived from them.
                // is_break() is checked alongside it because the inline context
                // branches on it directly (a break ends a line box regardless of
                // its width), and it can flip under white-space:pre when a
                // newline replaces a plain space.
                const litehtml::size after = text_node->measured_size();
                if(after.width != before.width || after.height != before.height ||
                   text_node->is_break() != before_break)
                {
                    geometry_changed = true;
                }
            }
            ++i;
        }

        if(changed)
        {
            // E1 x E2, and the single most dangerous line in this file.
            //
            // When the text changed but nothing moved, E1 above skips
            // document::render() entirely -- correctly, because no geometry
            // moved. E2's dirtiness detection is a post-layout diff of fourteen
            // floats per render item, and by construction it will find nothing:
            // the box is in the same place, the same size, with the same
            // padding and borders. Only the *glyphs* inside it are different.
            //
            // Without this call the two fast paths compose into a silent bug:
            // layout is skipped because nothing moved, the cache replays last
            // frame's quads because nothing moved, and the number on screen
            // never changes again. Marking the element dirty here is what makes
            // the record re-emit this subtree, and it has to happen on `changed`
            // alone -- never gated on geometry_changed, which is precisely the
            // case where it is *not* set.
            ctx->cache.mark_element_dirty(el.get());
        }

        if(changed && geometry_changed)
        {
            // Fail closed: one node that moved is enough to make the whole
            // existing layout suspect, even if a hundred others did not.
            ctx->layout_clean = false;

            // But the movement is confined to this element, which is exactly
            // what the subtree relayout (E8) wants to know.
            ctx->note_sublayout_candidate(el);
        }

        // Note the arming happens even when nothing changed. A UI that pushes
        // the same string every frame has, by definition, moved nothing, and
        // the layout in front of it is still correct; making that case pay for
        // a full render would be the worst possible trade.
        ctx->pending_mutation = true;

        return changed ? 1 : 0;
    }

    // Structural change: the word/space run itself is different, so nodes have
    // to be created and destroyed. Still no HTML parsing and no selector
    // matching -- text nodes never match a stylesheet rule (element::apply_stylesheet
    // is a no-op for them) and el_text::compute_styles() derives everything it
    // needs from the parent's computed style.
    std::vector<litehtml::element::ptr> nodes;
    nodes.reserve(spec.size());
    for(const auto& s : spec)
    {
        if(s.is_space)
        {
            nodes.push_back(std::make_shared<litehtml::el_space>(s.text.c_str(), ctx->doc));
        } else
        {
            nodes.push_back(std::make_shared<litehtml::el_text>(s.text.c_str(), ctx->doc));
        }
    }

    el->clearRecursive();
    for(const auto& node : nodes)
    {
        el->appendChild(node); // sets the parent, which compute_styles() reads
    }
    for(const auto& node : nodes)
    {
        node->compute_styles(false);
    }

    // Rebuild only this element's render children. The invariant checked above
    // guarantees they are plain render_text leaves, so nothing above or beside
    // them is affected and the rest of the render tree survives untouched.
    ri->children().clear();
    for(const auto& node : nodes)
    {
        auto render_child = node->create_render_item(ri);
        if(render_child)
        {
            render_child = render_child->init();
            ri->add_child(render_child);
        }
    }

    // The render items themselves are new here: their m_pos has never been
    // through a line box. There is no version of this that can skip a render,
    // but the render can still be the subtree's alone (E8): the new items
    // hang under the same element, and its block ancestor re-lays them.
    ctx->layout_clean     = false;
    ctx->pending_mutation = true;
    ctx->note_sublayout_candidate(el);

    // The new render items carry dc_geom_valid == false, so the diff walk would
    // catch them anyway; mark explicitly rather than lean on that, because the
    // *old* items' entries are what would otherwise be replayed for the parent.
    ctx->cache.mark_element_dirty(el.get());

    return 1;
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_set_style(LhuContext* ctx, const char* selector, const char* css)
try
{
    if(!ctx || !ctx->doc || !selector || !css)
    {
        return 0;
    }

    if(!ctx->exp_setstyle)
    {
        // Off means off, not "quietly do it anyway". A host A/B-ing this on the
        // phone needs the 0 to fall back to lhu_load_html(), which is the thing
        // being measured against.
        ctx->last_error = "set_style: disabled (LHU_EXP_SETSTYLE=0)";
        return 0;
    }

    const litehtml::element::ptr root = ctx->doc->root();
    if(!root)
    {
        ctx->last_error = "set_style: no document";
        return 0;
    }

    const litehtml::element::ptr el = root->select_one(std::string(selector));
    if(!el)
    {
        ctx->last_error = std::string("set_style: no element matched '") + selector + "'";
        return 0;
    }

    // select_one() can only return something a selector matched, and a text node
    // never matches one -- but el_text is an element too, and it has no m_style
    // to rebuild, so prove the cast rather than assume it.
    auto* tag = dynamic_cast<litehtml::html_tag*>(el.get());
    if(!tag)
    {
        ctx->last_error = std::string("set_style: '") + selector + "' is not a styleable element";
        return 0;
    }

    // Same string in, nothing to do. Worth the strcmp: the whole point of this
    // call is to be used every frame, and a UI that pushes an unchanged style is
    // common enough (a bar that stopped moving) that making it pay for a restyle
    // and a forced render would be the worst possible trade. Reported as 0 the
    // same way lhu_set_text() reports unchanged text.
    const char* current = el->get_attr("style");
    if(current ? std::strcmp(current, css) == 0 : css[0] == '\0')
    {
        ctx->last_error.clear();
        return 0;
    }

    std::vector<StructuralKey> before;
    collect_structure(el, before);

    el->set_attr("style", css);

    // The two halves of "replace the inline style".
    //
    // m_style is NOT the inline block -- it is the element's whole declaration
    // map, the matched selectors' declarations with the style="" block folded on
    // top by compute_styles(). style::add() and style::combine() MERGE, so
    // calling compute_styles() again on its own would layer the new declarations
    // over the old ones and every property the previous frame set but this one
    // does not would survive. Fading a bar out by dropping `background` would
    // leave the old background in place forever.
    //
    // So the map has to be emptied and rebuilt from the selectors first, which is
    // exactly what refresh_styles() does -- and is why this is the same pair
    // (refresh, then compute) that element::find_styles_changes() uses when a
    // :hover changes which rules apply. The only difference is the recursion:
    // refresh_styles() re-runs selector matching over the whole subtree, and an
    // inline style edit cannot change which selectors match anything, so the
    // self-only variant is used and the descendants pay only for compute_styles.
    //
    // compute_styles() must still be recursive. Half of CSS inherits -- color,
    // font, line-height, white-space -- and a descendant's m_css is computed from
    // its parent's, so changing font-size on a container has to reach every text
    // node under it or they keep measuring at the old size. el_text's
    // compute_styles() re-measures from the parent's font, which is what makes
    // the inherited case come out byte-identical to a re-parse.
    {
        // Scoped so that a throw out of compute_styles() cannot leave the memo
        // switched off for every document this context loads afterwards.
        struct MemoGuard
        {
            litehtml::document_container* c;
            bool                          prev;
            ~MemoGuard()
            {
                c->set_inline_style_memoize(prev);
            }
        } guard {&ctx->container, ctx->container.inline_style_memoize()};

        ctx->container.set_inline_style_memoize(ctx->style_memoize_inline);

        if(ctx->style_deep_refresh)
        {
            tag->refresh_styles();
        } else
        {
            tag->refresh_styles_self();
        }
        tag->compute_styles(true);
    }

    ctx->last_error.clear();
    ++ctx->stat_style_applied;

    std::vector<StructuralKey> after;
    collect_structure(el, after);

    const bool rebuilt = structure_changed(before, after);
    if(rebuilt)
    {
        // See structural_key(): the render tree's shape came from these values
        // and document::render() will not revisit it. Rebuilding is still far
        // cheaper than re-parsing the HTML, but every render_item address in the
        // document has just changed, and the quad cache keys its entries on those
        // addresses -- so it has to be told, with the same call a new document
        // would trigger.
        ctx->doc->rebuild_render_tree();
        ctx->cache.on_document_replaced();
        ++ctx->stat_style_trees;
    } else
    {
        // E2, and the same trap lhu_set_text() documents. The cache decides what
        // to replay from a post-layout diff of fourteen floats per render item;
        // a style change that only altered a COLOUR moves none of them, so the
        // walk finds nothing and the stale colour is replayed forever. Marking
        // here is unconditional on "did anything move", because the case that
        // needs it is precisely the case where nothing did.
        //
        // Marking the element sets dc_force_subtree, which covers the inherited
        // change reaching descendants; the walk propagates a dirty child up to
        // its ancestors, which covers the one non-geometric way a change here can
        // repaint something above it (html_tag::get_background() lets the root
        // draw the body's background).
        ctx->cache.mark_element_dirty(el.get());
    }

    // E1, and the reason this is a plain invalidate rather than lhu_set_text()'s
    // careful "did the measurement move" test. A text node contributes exactly
    // one thing to layout, its measured size, so set_text can compare it and
    // prove nothing moved. A style declaration can change width, padding, margin,
    // border, font, flex-basis, writing direction -- there is no small set of
    // numbers to compare, and getting the set wrong renders a frame at the
    // previous geometry. Layout on the page this exists for measures 0.07 ms
    // against 3.45 ms of parsing, so buying a provable answer with a full render
    // is not even a close call.
    ctx->invalidate_layout();

    // ...but the render does not have to be the whole document's (E8): an
    // inline-style edit restyles exactly this element's subtree, the same
    // confinement lhu_set_text() proves for text. The candidate is handed
    // back after the reset above; whatever the style did to the element's own
    // box, the subtree render verifies the footprint and falls back if it
    // moved. A rebuilt render tree stays a full render: every captured render
    // input died with the old items.
    if(!rebuilt)
    {
        ctx->note_sublayout_candidate(el);
    }

    return 1;
}
LHU_API_CATCH(ctx, 0)

namespace
{
// EXPERIMENT E8: re-lay one mutated subtree in place instead of rendering the
// document. litehtml has no incremental layout, so a four-character text
// change pays for every render item on the page; this buys the incremental
// answer for the narrow case a HUD's frames actually consist of, and proves
// it instead of assuming it. The candidate's nearest block ancestor is
// re-rendered with the exact inputs the last full pass gave it (captured on
// the render item), and the result only stands if the block's outer
// footprint lands bit-identically where it was -- if so, nothing else on the
// page could have moved, because plain in-flow blocks read nothing from a
// subtree except its footprint. Every precondition that could break that
// reasoning falls back to a full render, which is always correct.
bool try_sublayout(LhuContext* ctx, const litehtml::element::ptr& mutated)
{
    // Facts that make subtree isolation unsound, checked once per document:
    // floats leak across formatting-context edges, and absolutely positioned
    // boxes are laid out by a separate document pass this path does not run.
    if(ctx->doc_has_floats || ctx->doc_has_positioned)
    {
        return false;
    }

    // The box to re-lay: the nearest block-level ancestor with a rendered
    // item. Inline elements flow through their parents' line boxes, so the
    // block above them is the smallest thing that can be re-laid alone.
    litehtml::element::ptr block = mutated;
    while(block)
    {
        const litehtml::style_display d = block->css().get_display();
        if((d == litehtml::display_block || d == litehtml::display_list_item) && block->get_render_item())
        {
            break;
        }
        block = block->parent();
    }
    if(!block)
    {
        return false;
    }

    const auto ri = block->get_render_item();
    if(!ri || !ri->lhu_rendered)
    {
        return false;
    }

    // Every ancestor must be a plain in-flow block. Anything that sizes
    // itself from its contents -- flex, tables, inline-blocks -- reads the
    // subtree's intrinsic widths, and those can change even when the final
    // footprint does not: the verification below would pass and the page
    // would still be stale. Plain blocks read nothing from below except
    // heights, which the footprint check covers.
    for(litehtml::element::ptr a = block->parent(); a; a = a->parent())
    {
        const litehtml::style_display d = a->css().get_display();
        if(d != litehtml::display_block && d != litehtml::display_list_item)
        {
            return false;
        }
    }

    // Re-render in place, then require the outer footprint (margin box
    // included) to land exactly where it was. If it moved, siblings move,
    // and only a document render answers for them. The half-updated subtree
    // this leaves behind is fine: the caller's full render overwrites it.
    const litehtml::position before     = ri->pos();
    const litehtml::pixel_t  before_l   = ri->left();
    const litehtml::pixel_t  before_r   = ri->right();
    const litehtml::pixel_t  before_t   = ri->top();
    const litehtml::pixel_t  before_b   = ri->bottom();

    ri->render(ri->lhu_last_x, ri->lhu_last_y, ri->lhu_last_cb, nullptr, false);

    const litehtml::position& after = ri->pos();
    if(after.x != before.x || after.y != before.y || after.width != before.width ||
       after.height != before.height || ri->left() != before_l || ri->right() != before_r ||
       ri->top() != before_t || ri->bottom() != before_b)
    {
        return false;
    }

    // A scroll view inside the subtree may hold more or less content now;
    // this re-measures every one of them, and the document size with them.
    ctx->doc->lhu_update_document_size();
    return true;
}
} // namespace

float lhu_layout(LhuContext* ctx, float max_width)
try
{
    if(!ctx || !ctx->doc)
    {
        return 0.f;
    }

    if(ctx->styles_stale && ctx->doc->root())
    {
        ctx->styles_stale = false;

        // to_pixels() resolves vw against document::m_media, a snapshot taken at
        // parse time -- so the recompute below is worthless until that snapshot
        // is refreshed. media_changed() is the only thing that refreshes it. Its
        // own restyle is not enough on its own: it runs only when a media query
        // actually flips, and a page with no @media rules at all still has vw
        // lengths to redo.
        ctx->doc->media_changed();

        ctx->doc->root()->refresh_styles();
        ctx->doc->root()->compute_styles();

        // Deliberately no rebuild_render_tree() here. It is tempting -- a media
        // query keyed to the viewport can change `display`, and the render tree
        // is built from computed display values -- but media_changed() above
        // already rebuilds for exactly that case, and only for that case. Doing
        // it unconditionally destroys and recreates every render item, and a
        // freshly created scroll_view reports nothing to scroll until a second
        // render has measured its content: the page scrolls, moves zero pixels,
        // and looks like the touch was ignored.
        ctx->layout_clean = false;

        // The restyle above touched every element, so a candidate noted since
        // the viewport change no longer bounds the stale geometry (E8).
        ctx->sublayout_candidate.reset();
        ctx->sublayout_multi = false;
    }

    if(ctx->exp_subtree && ctx->layout_clean && ctx->pending_mutation && max_width == ctx->layout_width)
    {
        // Every mutation since the last render re-measured to exactly the size
        // it already had, so the tree in front of us *is* the answer render()
        // would produce. doc_width/doc_height are still the ones that render
        // computed, and every render item's m_pos is untouched, which is what
        // lhu_record() reads.
        ctx->pending_mutation = false;
        ++ctx->stat_skipped;
        return ctx->doc_height;
    }

    // E8: when everything stale sits inside one candidate subtree (see
    // LhuContext -- any wider invalidation dropped the candidate on its way
    // through invalidate_layout()), try to re-lay just that. Same max_width
    // only: a different width is a different layout, always.
    if(ctx->exp_sublayout && !ctx->sublayout_multi && max_width == ctx->layout_width)
    {
        if(const auto el = ctx->sublayout_candidate.lock())
        {
            if(try_sublayout(ctx, el))
            {
                ctx->sublayout_candidate.reset();
                ctx->doc_width        = static_cast<float>(ctx->doc->width());
                ctx->doc_height       = static_cast<float>(ctx->doc->height());
                ctx->layout_clean     = true;
                ctx->pending_mutation = false;
                ++ctx->stat_sub_layouts;
                return ctx->doc_height;
            }
            ++ctx->stat_sub_fallbacks;
        }
    }

    ctx->doc->render(litehtml::pixel_t(max_width));
    ctx->doc_width  = static_cast<float>(ctx->doc->width());
    ctx->doc_height = static_cast<float>(ctx->doc->height());

    ctx->layout_clean     = true;
    ctx->pending_mutation = false;
    ctx->layout_width     = max_width;
    ctx->sublayout_candidate.reset();
    ctx->sublayout_multi = false;
    ++ctx->stat_rendered;

    return ctx->doc_height;
}
LHU_API_CATCH(ctx, 0.f)

namespace
{
// Union of the pixels a quad can touch: its rect intersected with its clip
// (both axis-aligned; the rounded corners only shrink the area, so this is a
// superset of what is painted -- which is the safe direction for a dirty rect).
struct DirtyBounds
{
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    bool  any = false;

    void add(const LhuQuad& q)
    {
        float qx0 = q.x, qy0 = q.y, qx1 = q.x + q.w, qy1 = q.y + q.h;

        if(q.clip_w > 0.f && q.clip_h > 0.f)
        {
            qx0 = std::max(qx0, q.clip_x);
            qy0 = std::max(qy0, q.clip_y);
            qx1 = std::min(qx1, q.clip_x + q.clip_w);
            qy1 = std::min(qy1, q.clip_y + q.clip_h);
        }

        if(qx1 <= qx0 || qy1 <= qy0)
        {
            return; // clipped away entirely; it paints nothing
        }

        if(!any)
        {
            x0 = qx0; y0 = qy0; x1 = qx1; y1 = qy1;
            any = true;
        } else
        {
            x0 = std::min(x0, qx0);
            y0 = std::min(y0, qy0);
            x1 = std::max(x1, qx1);
            y1 = std::max(y1, qy1);
        }
    }
};

// Axis-aligned pixels a quad can touch: rect intersected with clip. Returns
// false when the quad paints nothing at all. Note the clip convention: a
// negative width means "no clip", but a PRESENT clip with zero width or
// height means the quad is clipped away entirely -- which is exactly what a
// row scrolled out of its viewport carries, so getting this wrong reads
// offscreen content as painted.
bool painted_box(const LhuQuad& q, float& x0, float& y0, float& x1, float& y1)
{
    x0 = q.x;
    y0 = q.y;
    x1 = q.x + q.w;
    y1 = q.y + q.h;

    if(q.clip_w >= 0.f)
    {
        x0 = std::max(x0, q.clip_x);
        y0 = std::max(y0, q.clip_y);
        x1 = std::min(x1, q.clip_x + q.clip_w);
        y1 = std::min(y1, q.clip_y + q.clip_h);
    }

    return x1 > x0 && y1 > y0;
}

// Is `b` exactly `a` translated by (tx, ty), ignoring the clip? Positions
// move; a gradient's geometry is absolute document coordinates, so it moves
// too. The clip is judged separately (see clip_compatible): litehtml clips a
// background layer to its own element's box, so a scrolled row's clip moves
// with it, while text under the container's overflow clip keeps a stationary
// one -- byte equality cannot express both. Bit-exact by construction: layout
// computes positions as (content - scroll_offset), and with integral scroll
// offsets the float arithmetic reassociates without rounding, which is what
// lets memcmp be the comparison.
bool geometry_translated(const LhuQuad& a, const LhuQuad& b, float tx, float ty)
{
    LhuQuad ta = a;
    ta.x += tx;
    ta.y += ty;

    switch(ta.type)
    {
    case LHU_QUAD_LINEAR_GRAD:
        ta.params[0] += tx;
        ta.params[1] += ty;
        ta.params[2] += tx;
        ta.params[3] += ty;
        break;
    case LHU_QUAD_RADIAL_GRAD:
    case LHU_QUAD_CONIC_GRAD:
        ta.params[0] += tx;
        ta.params[1] += ty;
        break;
    default:
        break;
    }

    LhuQuad tb = b;
    for(LhuQuad* q : {&ta, &tb})
    {
        q->clip_x = q->clip_y = q->clip_w = q->clip_h = 0.f;
        q->clip_r[0] = q->clip_r[1] = q->clip_r[2] = q->clip_r[3] = 0.f;
    }

    return std::memcmp(&ta, &tb, sizeof(LhuQuad)) == 0;
}

// For a matched pair, the copy is only exact if the clips agree wherever the
// copied pixels land: over the destination D, b's clip must expose exactly
// what a's clip (translated) exposed at the source. Interval arithmetic
// covers every real case at once -- a stationary overflow clip (both sides
// reduce to D), a clip that rides along with its row, and a row clip clamped
// at the viewport edge. Rounded clips are refused outright: their corners are
// not translation-invariant.
bool clip_compatible(const LhuQuad& a, const LhuQuad& b, float tx, float ty, float dx0, float dy0, float dx1,
                     float dy1)
{
    for(int i = 0; i < 4; ++i)
    {
        if(a.clip_r[i] > 0.001f || b.clip_r[i] > 0.001f)
        {
            return false;
        }
    }

    // Clip of `a`, translated, clamped to D. Present-but-empty clips collapse
    // the interval on their own; only a truly absent clip means unbounded.
    float ax0 = dx0, ay0 = dy0, ax1 = dx1, ay1 = dy1;
    if(a.clip_w >= 0.f)
    {
        ax0 = std::max(dx0, a.clip_x + tx);
        ay0 = std::max(dy0, a.clip_y + ty);
        ax1 = std::min(dx1, a.clip_x + a.clip_w + tx);
        ay1 = std::min(dy1, a.clip_y + a.clip_h + ty);
    }

    // Clip of `b`, clamped to D.
    float bx0 = dx0, by0 = dy0, bx1 = dx1, by1 = dy1;
    if(b.clip_w >= 0.f)
    {
        bx0 = std::max(dx0, b.clip_x);
        by0 = std::max(dy0, b.clip_y);
        bx1 = std::min(dx1, b.clip_x + b.clip_w);
        by1 = std::min(dy1, b.clip_y + b.clip_h);
    }

    const bool a_empty = ax1 <= ax0 || ay1 <= ay0;
    const bool b_empty = bx1 <= bx0 || by1 <= by0;
    if(a_empty || b_empty)
    {
        return a_empty == b_empty;
    }

    const float eps = 0.01f;
    return std::fabs(ax0 - bx0) < eps && std::fabs(ay0 - by0) < eps && std::fabs(ax1 - bx1) < eps &&
           std::fabs(ay1 - by1) < eps;
}

// The scroll refinement: can this frame be explained as the previous one with
// one region's pixels translated, plus a repaint of the strips along the
// scroll axis?
//
// Nothing is taken on faith; the frame has to prove the translation quad by
// quad. Pass 1 aligns the changed spans of both frames into four classes:
// byte-identical quads (content that sat still in the middle of the span --
// a heading above a scrolled list keeps painting between the moving quads),
// translated pairs, and quads only one frame has (content that scrolled in or
// out). Pass 2 derives the scrolled window from the clips the moving quads
// themselves carry, shrinks it to the whole pixels a texel copy can move --
// a window edge sitting on a fraction (text heights above it are fractional)
// blends content with background in the pixel row it crosses, and that row
// must be repainted, never copied -- and then checks every class against the
// copy: pairs must expose the same pixels through their clips, one-sided
// quads must stay out of the copied region, and everything that did not move
// (in the span or outside it) must be translation-invariant over the copied
// pixels. The first quad that fails makes the whole attempt return false and
// the caller keeps the plain rect answer, so this path can be wrong about
// nothing.
bool try_scroll_refine(const std::vector<LhuQuad>& now, const std::vector<LhuQuad>& then, size_t prefix,
                       size_t suffix, float tx, float ty, LhuFrame* out)
{
    // The copy is a texel move; a fractional translation cannot use it.
    if(std::fabs(tx - std::round(tx)) > 0.001f || std::fabs(ty - std::round(ty)) > 0.001f)
    {
        return false;
    }

    const size_t ie = then.size() - suffix;
    const size_t je = now.size() - suffix;

    const auto matches = [&](const LhuQuad& a, const LhuQuad& b) {
        return std::memcmp(&a, &b, sizeof(LhuQuad)) == 0 || geometry_translated(a, b, tx, ty);
    };

    // ---- pass 1: align the spans -------------------------------------------
    struct Pair
    {
        const LhuQuad* a;
        const LhuQuad* b;
    };
    std::vector<Pair>           moved;
    std::vector<const LhuQuad*> stationary;
    std::vector<const LhuQuad*> extra_old;
    std::vector<const LhuQuad*> extra_new;

    // How far ahead resynchronization may scan. A quad that scrolled in or out
    // is a whole element's run (a background, borders, a few glyphs); 64 quads
    // of slack covers any sane element and bounds the cost.
    const size_t kResync = 64;

    size_t i = prefix, j = prefix;
    float  bx0, by0, bx1, by1;
    while(i < ie || j < je)
    {
        if(i < ie && !painted_box(then[i], bx0, by0, bx1, by1))
        {
            ++i;
            continue;
        }
        if(j < je && !painted_box(now[j], bx0, by0, bx1, by1))
        {
            ++j;
            continue;
        }
        if(i >= ie)
        {
            extra_new.push_back(&now[j]);
            ++j;
            continue;
        }
        if(j >= je)
        {
            extra_old.push_back(&then[i]);
            ++i;
            continue;
        }

        if(std::memcmp(&then[i], &now[j], sizeof(LhuQuad)) == 0)
        {
            stationary.push_back(&then[i]);
            ++i;
            ++j;
            continue;
        }
        if(geometry_translated(then[i], now[j], tx, ty))
        {
            moved.push_back({&then[i], &now[j]});
            ++i;
            ++j;
            continue;
        }

        // Out of step: one side has quads the other lacks. Resync on whichever
        // side reaches a match sooner, and classify what was skipped.
        size_t k = 0, l = 0;
        for(size_t t = j + 1; t < je && t - j <= kResync; ++t)
        {
            if(matches(then[i], now[t]))
            {
                k = t - j;
                break;
            }
        }
        for(size_t t = i + 1; t < ie && t - i <= kResync; ++t)
        {
            if(matches(then[t], now[j]))
            {
                l = t - i;
                break;
            }
        }

        if(k > 0 && (l == 0 || k <= l))
        {
            while(k-- > 0)
            {
                extra_new.push_back(&now[j]);
                ++j;
            }
            continue;
        }
        if(l > 0)
        {
            while(l-- > 0)
            {
                extra_old.push_back(&then[i]);
                ++i;
            }
            continue;
        }
        return false;
    }

    if(moved.empty())
    {
        return false;
    }

    // ---- the scrolled window, from the moving quads's own clips -------------
    float wx0 = 0.f, wy0 = 0.f, wx1 = 0.f, wy1 = 0.f;
    {
        bool       any        = false;
        const auto grow_clip = [&](const LhuQuad* q) {
            if(q->clip_w < 0.f)
            {
                return false; // scrolled content is always clipped; no clip, no window
            }
            if(!any)
            {
                wx0 = q->clip_x;
                wy0 = q->clip_y;
                wx1 = q->clip_x + q->clip_w;
                wy1 = q->clip_y + q->clip_h;
                any = true;
            } else
            {
                wx0 = std::min(wx0, q->clip_x);
                wy0 = std::min(wy0, q->clip_y);
                wx1 = std::max(wx1, q->clip_x + q->clip_w);
                wy1 = std::max(wy1, q->clip_y + q->clip_h);
            }
            return true;
        };

        for(const Pair& p : moved)
        {
            if(!grow_clip(p.a) || !grow_clip(p.b))
            {
                return false;
            }
        }
        for(const LhuQuad* q : extra_old)
        {
            if(!grow_clip(q))
            {
                return false;
            }
        }
        for(const LhuQuad* q : extra_new)
        {
            if(!grow_clip(q))
            {
                return false;
            }
        }
    }

    // Across the scroll axis the window's edges must sit on whole pixels:
    // content slides along them, so a fractional edge would blend differently
    // every frame in a pixel column no rect below repaints.
    const auto is_whole = [](float v) { return std::fabs(v - std::round(v)) < 0.01f; };
    if(ty != 0.f && (!is_whole(wx0) || !is_whole(wx1)))
    {
        return false;
    }
    if(tx != 0.f && (!is_whole(wy0) || !is_whole(wy1)))
    {
        return false;
    }

    // ---- D: the copied pixels ----------------------------------------------
    // Window intersected with window-translated, shrunk to whole pixels plus
    // one unit of margin along the scroll axis: the pixel rows straddling the
    // window's (possibly fractional) edges blend content with background
    // through the clip's antialiasing, and blended rows cannot be copied.
    float dx0 = std::max(wx0, wx0 + tx);
    float dy0 = std::max(wy0, wy0 + ty);
    float dx1 = std::min(wx1, wx1 + tx);
    float dy1 = std::min(wy1, wy1 + ty);

    if(ty != 0.f)
    {
        dy0 = std::ceil(dy0) + 1.f;
        dy1 = std::floor(dy1) - 1.f;
    } else
    {
        dx0 = std::ceil(dx0) + 1.f;
        dx1 = std::floor(dx1) - 1.f;
    }

    if(dx1 <= dx0 || dy1 <= dy0)
    {
        return false;
    }

    const float eps = 0.01f;

    // ---- pass 2: every class proves itself against D ------------------------
    for(const Pair& p : moved)
    {
        if(!clip_compatible(*p.a, *p.b, tx, ty, dx0, dy0, dx1, dy1))
        {
            return false;
        }
    }
    for(const LhuQuad* q : extra_new)
    {
        painted_box(*q, bx0, by0, bx1, by1);
        if(bx1 > dx0 + eps && bx0 < dx1 - eps && by1 > dy0 + eps && by0 < dy1 - eps)
        {
            return false; // scrolled in, yet paints into the copied pixels
        }
    }
    for(const LhuQuad* q : extra_old)
    {
        painted_box(*q, bx0, by0, bx1, by1);
        if(bx1 + tx > dx0 + eps && bx0 + tx < dx1 - eps && by1 + ty > dy0 + eps && by0 + ty < dy1 - eps)
        {
            return false; // scrolled out, yet the copy would still carry it
        }
    }

    // The copy shifts the pixels *under* the moved content too, so anything
    // that did not move -- in the span or outside it -- must look the same
    // shifted wherever the copied pixels come from or land: a solid, unrounded
    // rect covering that union, in practice the background the content
    // scrolls across.
    const float rx0 = std::min(dx0, dx0 - tx);
    const float ry0 = std::min(dy0, dy0 - ty);
    const float rx1 = std::max(dx1, dx1 - tx);
    const float ry1 = std::max(dy1, dy1 - ty);

    const auto invariant_over_copy = [&](const LhuQuad& q) {
        float qx0, qy0, qx1, qy1;
        if(!painted_box(q, qx0, qy0, qx1, qy1))
        {
            return true;
        }
        if(qx1 <= rx0 || qx0 >= rx1 || qy1 <= ry0 || qy0 >= ry1)
        {
            return true; // does not touch the copied pixels
        }
        if(q.type == LHU_QUAD_BORDER)
        {
            // A border paints only its ring. If the copied pixels sit inside
            // the ring's inner rect -- clear of the rounded inner corners,
            // conservatively sized by the outer radii -- it cannot touch them.
            const float ix0 = q.x + q.border[0]; // border order: left, top, right, bottom
            const float iy0 = q.y + q.border[1];
            const float ix1 = q.x + q.w - q.border[2];
            const float iy1 = q.y + q.h - q.border[3];
            if(rx0 < ix0 || ry0 < iy0 || rx1 > ix1 || ry1 > iy1)
            {
                return false;
            }
            const float br[4][2] = {{q.rx[0], q.ry[0]}, {q.rx[1], q.ry[1]}, {q.rx[2], q.ry[2]}, {q.rx[3], q.ry[3]}};
            const float bcx[4]   = {ix0, ix1, ix1, ix0};
            const float bcy[4]   = {iy0, iy0, iy1, iy1};
            for(int c = 0; c < 4; ++c)
            {
                if(br[c][0] <= 0.f || br[c][1] <= 0.f)
                {
                    continue;
                }
                const float kx0 = std::min(bcx[c], bcx[c] + (c == 0 || c == 3 ? br[c][0] : -br[c][0]));
                const float kx1 = std::max(bcx[c], bcx[c] + (c == 0 || c == 3 ? br[c][0] : -br[c][0]));
                const float ky0 = std::min(bcy[c], bcy[c] + (c <= 1 ? br[c][1] : -br[c][1]));
                const float ky1 = std::max(bcy[c], bcy[c] + (c <= 1 ? br[c][1] : -br[c][1]));
                if(kx1 > rx0 && kx0 < rx1 && ky1 > ry0 && ky0 < ry1)
                {
                    return false;
                }
            }
            return true;
        }
        if(q.type != LHU_QUAD_RECT)
        {
            return false; // glyphs, images, gradients: never uniform
        }
        if(qx0 > rx0 || qy0 > ry0 || qx1 < rx1 || qy1 < ry1)
        {
            return false; // covers only part of the region: its edge would smear
        }

        // Rounded corners are the one non-uniform part of a solid rect; they
        // must stay clear of the region.
        const float cr[4][2] = {{q.rx[0], q.ry[0]}, {q.rx[1], q.ry[1]}, {q.rx[2], q.ry[2]}, {q.rx[3], q.ry[3]}};
        const float cx[4]    = {q.x, q.x + q.w, q.x + q.w, q.x};
        const float cy[4]    = {q.y, q.y, q.y + q.h, q.y + q.h};
        for(int c = 0; c < 4; ++c)
        {
            if(cr[c][0] <= 0.f || cr[c][1] <= 0.f)
            {
                continue;
            }
            const float kx0 = std::min(cx[c], cx[c] + (c == 0 || c == 3 ? cr[c][0] : -cr[c][0]));
            const float kx1 = std::max(cx[c], cx[c] + (c == 0 || c == 3 ? cr[c][0] : -cr[c][0]));
            const float ky0 = std::min(cy[c], cy[c] + (c <= 1 ? cr[c][1] : -cr[c][1]));
            const float ky1 = std::max(cy[c], cy[c] + (c <= 1 ? cr[c][1] : -cr[c][1]));
            if(kx1 > rx0 && kx0 < rx1 && ky1 > ry0 && ky0 < ry1)
            {
                return false;
            }
        }
        return true;
    };

    for(const LhuQuad* q : stationary)
    {
        if(!invariant_over_copy(*q))
        {
            return false;
        }
    }
    for(size_t k = 0; k < prefix; ++k)
    {
        if(!invariant_over_copy(then[k]))
        {
            return false;
        }
    }
    for(size_t k = then.size() - suffix; k < then.size(); ++k)
    {
        if(!invariant_over_copy(then[k]))
        {
            return false;
        }
    }

    // ---- verified; emit the recipe ------------------------------------------
    // D is the copy destination. What remains of the window on either side of
    // it along the scroll axis -- the strip that scrolled in on one end, the
    // fractional-edge sliver on the other -- is repainted. Same padding as the
    // plain rect, for the same AA-skirt reason; the pad may spill into the
    // copied region, where the repaint redraws identical pixels.
    const float pad = 4.f;

    float r1x0, r1y0, r1x1, r1y1; // the rect after D along the axis
    float r2x0, r2y0, r2x1, r2y1; // the rect before D
    if(ty != 0.f)
    {
        r1x0 = r2x0 = wx0;
        r1x1 = r2x1 = wx1;
        r2y0         = std::floor(wy0);
        r2y1         = dy0;
        r1y0         = dy1;
        r1y1         = std::ceil(wy1);
    } else
    {
        r1y0 = r2y0 = wy0;
        r1y1 = r2y1 = wy1;
        r2x0         = std::floor(wx0);
        r2x1         = dx0;
        r1x0         = dx1;
        r1x1         = std::ceil(wx1);
    }

    // The strip that scrolled in is the interesting rect; report it first so
    // hosts and tests that only look at the primary rect see the strip.
    if((r2x1 - r2x0) * (r2y1 - r2y0) > (r1x1 - r1x0) * (r1y1 - r1y0))
    {
        std::swap(r1x0, r2x0);
        std::swap(r1y0, r2y0);
        std::swap(r1x1, r2x1);
        std::swap(r1y1, r2y1);
    }

    const auto emit_rect = [&](float x0, float y0, float x1, float y1, float& ox, float& oy, float& ow, float& oh) {
        ox = std::max(0.f, x0 - pad);
        oy = std::max(0.f, y0 - pad);
        ow = std::max(0.f, std::min(out->doc_width, x1 + pad) - ox);
        oh = std::max(0.f, std::min(out->doc_height, y1 + pad) - oy);
    };

    out->dirty_mode = LHU_DIRTY_MODE_SCROLL;
    out->scroll_x   = dx0;
    out->scroll_y   = dy0;
    out->scroll_w   = dx1 - dx0;
    out->scroll_h   = dy1 - dy0;
    out->scroll_dx  = tx;
    out->scroll_dy  = ty;
    emit_rect(r1x0, r1y0, r1x1, r1y1, out->dirty_x, out->dirty_y, out->dirty_w, out->dirty_h);
    emit_rect(r2x0, r2y0, r2x1, r2y1, out->dirty2_x, out->dirty2_y, out->dirty2_w, out->dirty2_h);
    return true;
}

// Fills out->dirty_* by diffing this frame against the previous recorded one.
//
// The diff is prefix/suffix rather than pairwise: a text that gains a digit
// inserts a quad, and pairwise comparison would then see every later quad as
// "different" and dirty the rest of the page. Common prefix + common suffix
// isolates the changed span in both frames instead, exactly and in O(n).
//
// `trusted` is false when the record was a full rebuild or the cache is off:
// then the stream can differ for reasons the diff cannot see (an atlas repack
// moving texels under identical UVs, an image edited in place), so the only
// honest answer is "repaint everything".
void compute_dirty_region(LhuContext* ctx, LhuFrame* out, bool trusted)
{
    out->dirty_mode = LHU_DIRTY_MODE_FULL;

    const bool comparable = trusted && ctx->have_prev_frame &&
                            ctx->prev_doc_w == out->doc_width && ctx->prev_doc_h == out->doc_height;

    const std::vector<LhuQuad>& now  = ctx->container.quads();
    const std::vector<LhuQuad>& then = ctx->prev_quads;

    if(comparable)
    {
        const size_t n_now = now.size(), n_then = then.size();
        const size_t n_min = std::min(n_now, n_then);

        size_t prefix = 0;
        while(prefix < n_min && std::memcmp(&now[prefix], &then[prefix], sizeof(LhuQuad)) == 0)
        {
            ++prefix;
        }

        size_t suffix = 0;
        while(suffix < n_min - prefix &&
              std::memcmp(&now[n_now - 1 - suffix], &then[n_then - 1 - suffix], sizeof(LhuQuad)) == 0)
        {
            ++suffix;
        }

        const bool lut_same     = ctx->container.grad_lut() == ctx->prev_grad_lut;
        const bool span_changed = prefix < n_then - suffix || prefix < n_now - suffix;

        // A pending scroll offers a better explanation than a bounding rect:
        // the frame may be the previous one translated. Single view, single
        // axis, LUT untouched -- and then every quad has to prove it. The
        // content moves opposite to the scroll delta, hence the negation.
        if(span_changed && lut_same && ctx->scroll_hint_any && !ctx->scroll_hint_multi &&
           (ctx->scroll_hint_dx == 0.f) != (ctx->scroll_hint_dy == 0.f) &&
           try_scroll_refine(now, then, prefix, suffix, -ctx->scroll_hint_dx, -ctx->scroll_hint_dy, out))
        {
            ctx->prev_quads.assign(now.begin(), now.end());
            ctx->prev_grad_lut   = ctx->container.grad_lut();
            ctx->prev_doc_w      = out->doc_width;
            ctx->prev_doc_h      = out->doc_height;
            ctx->have_prev_frame = true;
            return;
        }

        DirtyBounds bounds;
        for(size_t i = prefix; i < n_then - suffix; ++i)
        {
            bounds.add(then[i]); // where the old content was
        }
        for(size_t i = prefix; i < n_now - suffix; ++i)
        {
            bounds.add(now[i]); // where the new content is
        }

        // A gradient's stops live in the LUT, not in the quad, so a stop can
        // change under a byte-identical quad. If the LUT moved, every gradient
        // in either frame is suspect.
        if(!lut_same)
        {
            for(const LhuQuad& q : then)
            {
                if(q.type >= LHU_QUAD_LINEAR_GRAD) { bounds.add(q); }
            }
            for(const LhuQuad& q : now)
            {
                if(q.type >= LHU_QUAD_LINEAR_GRAD) { bounds.add(q); }
            }
        }

        if(!bounds.any)
        {
            out->dirty_mode = LHU_DIRTY_MODE_NONE;
        } else
        {
            // The mesh grows every quad by an antialiasing skirt and the shader
            // feathers up to a pixel beyond that; four document units cover both
            // with room to spare.
            const float pad = 4.f;

            const float x0 = std::max(0.f, bounds.x0 - pad);
            const float y0 = std::max(0.f, bounds.y0 - pad);
            const float x1 = std::min(out->doc_width, bounds.x1 + pad);
            const float y1 = std::min(out->doc_height, bounds.y1 + pad);

            out->dirty_mode = LHU_DIRTY_MODE_RECT;
            out->dirty_x    = x0;
            out->dirty_y    = y0;
            out->dirty_w    = std::max(0.f, x1 - x0);
            out->dirty_h    = std::max(0.f, y1 - y0);
        }
    }

    // NONE means the buffer still equals prev_quads; skip the copy.
    if(out->dirty_mode != LHU_DIRTY_MODE_NONE)
    {
        ctx->prev_quads.assign(now.begin(), now.end());
        ctx->prev_grad_lut = ctx->container.grad_lut();
        ctx->prev_doc_w    = out->doc_width;
        ctx->prev_doc_h    = out->doc_height;
        ctx->have_prev_frame = true;
    }
}
} // namespace

void lhu_record(LhuContext* ctx, LhuFrame* out)
try
{
    if(!out)
    {
        return;
    }

    *out = LhuFrame {};

    if(!ctx)
    {
        return;
    }

    // The diff compares the emitted bytes, so it does not care WHY this record
    // ran -- a health-driven rebuild re-emits the same bytes for unchanged
    // content and diffs to nothing. Only a host invalidate poisons it (see
    // dirty_suspect), and only until the next record has rebuilt the baseline.
    const bool dirty_diff_trusted = !ctx->dirty_suspect;
    ctx->dirty_suspect = false;

    if(ctx->doc)
    {
        // Rasterizing a glyph can force the atlas to grow, which invalidates
        // every UV emitted so far. Detect that and record once more.
        for(int attempt = 0; attempt < 2; ++attempt)
        {
            const int epoch_before = ctx->fonts.atlas().epoch();

            if(ctx->cache.enabled())
            {
                // The atlas can also grow *outside* a record -- text_width()
                // rasterizes, so lhu_load_html and lhu_set_text both can move
                // every UV. Reconcile before deciding anything, or a cached run
                // recorded against the old packing gets replayed verbatim.
                if(ctx->atlas_epoch != epoch_before)
                {
                    ctx->atlas_epoch = epoch_before;
                    ctx->cache.invalidate_all();
                }

                // Note this walk runs whether or not lhu_layout() actually
                // rendered. When E1 took its fast path nothing moved, so the
                // walk finds only what lhu_set_text marked by hand -- which is
                // the whole reason that mark_element_dirty() call exists.
                const bool anything_dirty = ctx->cache.refresh(ctx->doc->root_render());
                const lhu::QuadCache::Plan p =
                    ctx->cache.plan(anything_dirty, ctx->doc_width, ctx->doc_height);

                ctx->cache.begin_frame(p);

                if(p == lhu::QuadCache::Plan::Fast)
                {
                    // Nothing changed and the buffer still holds the retained
                    // frame: it is already the answer. begin_record() is what
                    // normally bumps the gradient LUT version, so do it by hand
                    // to keep the numbers the host sees identical in both modes.
                    ctx->container.bump_grad_version();
                    ctx->cache.end_frame(p, ctx->doc_width, ctx->doc_height);
                    break;
                }

                ctx->container.begin_record();

                litehtml::position clip(litehtml::pixel_t(0.f), litehtml::pixel_t(0.f),
                                        litehtml::pixel_t(ctx->doc_width), litehtml::pixel_t(ctx->doc_height));
                ctx->doc->draw(0, litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), &clip);

                ctx->container.end_record();
                ctx->cache.end_frame(p, ctx->doc_width, ctx->doc_height);

                if(ctx->fonts.atlas().epoch() == epoch_before)
                {
                    break;
                }

                // The atlas grew while we were recording. Everything captured
                // during this attempt carries UVs into the old packing, so the
                // retry has to start from an empty cache -- otherwise attempt 2
                // replays attempt 1's stale UVs and the frame is silently wrong.
                ctx->atlas_epoch = ctx->fonts.atlas().epoch();
                ctx->cache.invalidate_all();
                continue;
            }

            ctx->container.begin_record();

            litehtml::position clip(litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), litehtml::pixel_t(ctx->doc_width),
                                    litehtml::pixel_t(ctx->doc_height));
            ctx->doc->draw(0, litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), &clip);

            ctx->container.end_record();

            if(ctx->fonts.atlas().epoch() == epoch_before)
            {
                break;
            }
        }
    }

    const auto& quads = ctx->container.quads();
    out->quads        = quads.empty() ? nullptr : quads.data();
    out->quad_count   = static_cast<int32_t>(quads.size());
    out->doc_width    = ctx->doc_width;
    out->doc_height   = ctx->doc_height;

    const lhu::Atlas& atlas   = ctx->fonts.atlas();
    out->font_atlas_pixels    = atlas.pixels();
    out->font_atlas_w         = atlas.width();
    out->font_atlas_h         = atlas.height();
    out->font_atlas_version   = atlas.version();

    const auto& lut        = ctx->container.grad_lut();
    out->grad_lut_pixels   = lut.empty() ? nullptr : lut.data();
    out->grad_lut_w        = lhu::Container::kGradLutWidth;
    out->grad_lut_rows     = ctx->container.grad_lut_rows();
    out->grad_lut_version  = ctx->container.grad_lut_version();

    compute_dirty_region(ctx, out, dirty_diff_trusted);

    // The hypothesis was for this record only; whether it was used, refuted or
    // ignored, the next record starts from what the next scrolls report.
    ctx->clear_scroll_hint();
}
catch(const std::exception& e)
{
    set_last_error_noexcept(ctx, e.what());
    // A throw mid-record leaves *out half-filled -- possibly with pointers
    // into buffers whose construction was abandoned. An empty frame is the
    // only result the host can act on safely.
    if(out)
    {
        *out = LhuFrame {};
    }
}
catch(...)
{
    set_last_error_noexcept(ctx, "unknown native exception");
    if(out)
    {
        *out = LhuFrame {};
    }
}

namespace
{
// Union of the painted boxes of every render item an element produced.
//
// element::get_placement() reports the CONTENT box, which is the wrong rectangle
// to size a surface by: a padded, bordered badge paints well outside it and
// would be clipped. Padding and borders are only reachable from the render item,
// so the tree is walked to find the ones this element owns.
bool element_border_box(const std::shared_ptr<litehtml::render_item>& ri,
                        const litehtml::element::ptr& want, litehtml::position& out, bool& found)
{
    if(!ri)
    {
        return found;
    }

    if(ri->src_el() == want)
    {
        const litehtml::position content = ri->get_placement();

        litehtml::position box;
        box.x      = content.x - ri->padding_left() - ri->border_left();
        box.y      = content.y - ri->padding_top() - ri->border_top();
        box.width  = content.width + ri->padding_left() + ri->padding_right() + ri->border_left() +
                     ri->border_right();
        box.height = content.height + ri->padding_top() + ri->padding_bottom() + ri->border_top() +
                     ri->border_bottom();

        if(!found)
        {
            found = true;
            out   = box;
        } else
        {
            const litehtml::pixel_t right  = std::max(out.right(), box.right());
            const litehtml::pixel_t bottom = std::max(out.bottom(), box.bottom());
            out.x      = std::min(out.x, box.x);
            out.y      = std::min(out.y, box.y);
            out.width  = right - out.x;
            out.height = bottom - out.y;
        }
    }

    for(const auto& child : ri->children())
    {
        element_border_box(child, want, out, found);
    }

    return found;
}
} // namespace

namespace
{
// Paint order: a parent is visited before its children and an earlier sibling
// before a later one, so the last element that both covers the point and has an
// id is the topmost one under it.
void topmost_id_at(const std::shared_ptr<litehtml::render_item>& ri, litehtml::pixel_t px, litehtml::pixel_t py,
                   std::string& out)
{
    if(!ri || !ri->is_visible())
    {
        return;
    }

    const litehtml::position content = ri->get_placement();

    litehtml::position box;
    box.x      = content.x - ri->padding_left() - ri->border_left();
    box.y      = content.y - ri->padding_top() - ri->border_top();
    box.width  = content.width + ri->padding_left() + ri->padding_right() + ri->border_left() + ri->border_right();
    box.height = content.height + ri->padding_top() + ri->padding_bottom() + ri->border_top() + ri->border_bottom();

    if(px >= box.x && px < box.right() && py >= box.y && py < box.bottom())
    {
        const litehtml::element::ptr& el = ri->src_el();
        if(el)
        {
            const char* id = el->get_attr("id");
            if(id && *id)
            {
                out = id;
            }
        }
    }

    // Descend regardless: a child can stick out of its parent's box.
    for(const auto& child : ri->children())
    {
        topmost_id_at(child, px, py, out);
    }
}
} // namespace

int32_t lhu_element_at(LhuContext* ctx, float x, float y, char* out_id, int32_t len)
try
{
    if(out_id && len > 0)
    {
        out_id[0] = '\0';
    }

    if(!ctx || !ctx->doc)
    {
        return 0;
    }

    std::string found;
    topmost_id_at(ctx->doc->root_render(), litehtml::pixel_t(x), litehtml::pixel_t(y), found);

    if(found.empty())
    {
        return 0;
    }

    if(out_id && len > 0)
    {
        const size_t n = std::min(found.size(), static_cast<size_t>(len - 1));
        std::memcpy(out_id, found.data(), n);
        out_id[n] = '\0';
    }

    return 1;
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_element_rect(LhuContext* ctx, const char* selector, float* x, float* y, float* w, float* h)
try
{
    if(!ctx || !ctx->doc || !selector)
    {
        return 0;
    }

    const litehtml::element::ptr root = ctx->doc->root();
    if(!root)
    {
        return 0;
    }

    litehtml::element::ptr el;
    try
    {
        el = root->select_one(std::string(selector));
    }
    catch(const std::exception& e)
    {
        ctx->last_error = e.what();
        return 0;
    }

    if(!el)
    {
        ctx->last_error = std::string("no element matched: ") + selector;
        return 0;
    }

    litehtml::position box;
    bool               found = false;
    if(!element_border_box(ctx->doc->root_render(), el, box, found) || !found)
    {
        ctx->last_error = std::string("element matched but was not laid out: ") + selector;
        return 0;
    }

    if(x) *x = static_cast<float>(box.x);
    if(y) *y = static_cast<float>(box.y);
    if(w) *w = static_cast<float>(box.width);
    if(h) *h = static_cast<float>(box.height);

    ctx->last_error.clear();
    return 1;
}
LHU_API_CATCH(ctx, 0)

float lhu_doc_width(LhuContext* ctx)
{
    return ctx ? ctx->doc_width : 0.f;
}

float lhu_doc_height(LhuContext* ctx)
{
    return ctx ? ctx->doc_height : 0.f;
}

namespace
{
// Turns "on_mouse_* reported a change" into LhuDirtyFlags. The geometry hook
// fires during the call when the restyle moved anything layout reads; when the
// hooks are not attached at all (quad cache disabled), nobody was watching, so
// every change has to be treated as geometry -- stale layout is the failure
// mode this contract exists to prevent, and a pessimistic extra layout is not.
int32_t classify_input_change(LhuContext* ctx, bool changed)
{
    if(!changed)
    {
        return 0;
    }

    int32_t flags = LHU_DIRTY_PAINT;

    const bool watched = ctx->doc->draw_cache() != nullptr;
    if(!watched || ctx->cache.take_geometry_changed())
    {
        flags |= LHU_DIRTY_LAYOUT;
        ctx->invalidate_layout();
    }
    return flags;
}
} // namespace

int32_t lhu_mouse_move(LhuContext* ctx, float x, float y)
try
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    // Clear a stale flag so the classification below reads only what THIS
    // event did, then let the hook observe the restyle as it happens.
    ctx->cache.take_geometry_changed();

    const bool changed = ctx->doc->on_mouse_over(litehtml::pixel_t(x), litehtml::pixel_t(y), litehtml::pixel_t(x),
                                                 litehtml::pixel_t(y), kIgnoreRedraw);
    return classify_input_change(ctx, changed);
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_mouse_down(LhuContext* ctx, float x, float y)
try
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    ctx->cache.take_geometry_changed();

    const bool changed = ctx->doc->on_lbutton_down(litehtml::pixel_t(x), litehtml::pixel_t(y), litehtml::pixel_t(x),
                                                   litehtml::pixel_t(y), kIgnoreRedraw);
    return classify_input_change(ctx, changed);
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_mouse_up(LhuContext* ctx, float x, float y)
try
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    ctx->cache.take_geometry_changed();

    const bool changed = ctx->doc->on_lbutton_up(litehtml::pixel_t(x), litehtml::pixel_t(y), litehtml::pixel_t(x),
                                                 litehtml::pixel_t(y), kIgnoreRedraw);
    return classify_input_change(ctx, changed);
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_mouse_leave(LhuContext* ctx)
try
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    ctx->cache.take_geometry_changed();

    const bool changed = ctx->doc->on_mouse_leave(kIgnoreRedraw);
    return classify_input_change(ctx, changed);
}
LHU_API_CATCH(ctx, 0)

int32_t lhu_scroll(LhuContext* ctx, float dx, float dy, float x, float y)
try
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }

    const auto scrolled = ctx->doc->on_scroll(litehtml::pixel_t(dx), litehtml::pixel_t(dy), litehtml::pixel_t(x),
                                              litehtml::pixel_t(y), litehtml::pixel_t(x), litehtml::pixel_t(y));
    if(!scrolled.empty())
    {
        // render() rebuilds every scroll_view from the freshly measured content
        // size; skipping it would leave the scroll offsets to be clamped by a
        // different code path than the one the slow path uses.
        ctx->invalidate_layout();

        // Feed the dirty diff its translation hypothesis. The applied (post-
        // clamp) deltas accumulate; scrolls of two different views between one
        // pair of records disarm the fast path, because that is two
        // translations and the frame encodes one.
        for(const auto& sv : scrolled)
        {
            const float bx = static_cast<float>(sv.scroll_box.x);
            const float by = static_cast<float>(sv.scroll_box.y);

            if(!ctx->scroll_hint_any)
            {
                ctx->scroll_hint_any   = true;
                ctx->scroll_hint_box_x = bx;
                ctx->scroll_hint_box_y = by;
            } else if(ctx->scroll_hint_box_x != bx || ctx->scroll_hint_box_y != by)
            {
                ctx->scroll_hint_multi = true;
            }

            ctx->scroll_hint_dx += static_cast<float>(sv.dx);
            ctx->scroll_hint_dy += static_cast<float>(sv.dy);
        }
    }
    return static_cast<int32_t>(scrolled.size());
}
LHU_API_CATCH(ctx, 0)

void lhu_exp_set_enabled(LhuContext* ctx, int32_t enabled)
{
    if(!ctx)
    {
        return;
    }

    // Turning the fast path off must not leave a skip armed from before, or the
    // very next lhu_layout() would still take it.
    ctx->exp_subtree = enabled != 0;
    if(!ctx->exp_subtree)
    {
        ctx->pending_mutation = false;
    }
}

void lhu_exp_setstyle_set_enabled(LhuContext* ctx, int32_t enabled)
{
    if(ctx)
    {
        ctx->exp_setstyle = enabled != 0;
    }
}

void lhu_exp_sublayout_set_enabled(LhuContext* ctx, int32_t enabled)
{
    if(!ctx)
    {
        return;
    }

    ctx->exp_sublayout = enabled != 0;

    // Turning it off must not leave a candidate armed, or the very next
    // lhu_layout() after re-enabling would act on stale confinement.
    if(!ctx->exp_sublayout)
    {
        ctx->sublayout_candidate.reset();
        ctx->sublayout_multi = false;
    }
}

void lhu_exp_sublayout_stats(LhuContext* ctx, int32_t* out_enabled, int32_t* out_sub_layouts, int32_t* out_fallbacks)
{
    if(out_enabled)
    {
        *out_enabled = ctx && ctx->exp_sublayout ? 1 : 0;
    }
    if(out_sub_layouts)
    {
        *out_sub_layouts = ctx ? ctx->stat_sub_layouts : 0;
    }
    if(out_fallbacks)
    {
        *out_fallbacks = ctx ? ctx->stat_sub_fallbacks : 0;
    }
}

void lhu_exp_setstyle_stats(LhuContext* ctx, int32_t* out_enabled, int32_t* out_applied, int32_t* out_tree_rebuilds,
                            int32_t* out_style_cache_entries)
{
    if(out_enabled)
    {
        *out_enabled = ctx && ctx->exp_setstyle ? 1 : 0;
    }
    if(out_applied)
    {
        *out_applied = ctx ? ctx->stat_style_applied : 0;
    }
    if(out_tree_rebuilds)
    {
        *out_tree_rebuilds = ctx ? ctx->stat_style_trees : 0;
    }
    if(out_style_cache_entries)
    {
        *out_style_cache_entries = ctx ? static_cast<int32_t>(ctx->container.inline_style_cache_size()) : 0;
    }
}

void lhu_exp_stats(LhuContext* ctx, int32_t* out_enabled, int32_t* out_skipped, int32_t* out_rendered)
{
    if(out_enabled)
    {
        *out_enabled = ctx && ctx->exp_subtree ? 1 : 0;
    }
    if(out_skipped)
    {
        *out_skipped = ctx ? ctx->stat_skipped : 0;
    }
    if(out_rendered)
    {
        *out_rendered = ctx ? ctx->stat_rendered : 0;
    }
}

int64_t lhu_quadcache_stat(LhuContext* ctx, int32_t which)
try
{
    if(!ctx)
    {
        return -1;
    }

    switch(which)
    {
    case LHU_QC_ENABLED:        return ctx->cache.enabled() ? 1 : 0;
    case LHU_QC_QUADS_REPLAYED: return ctx->cache.quads_replayed();
    case LHU_QC_QUADS_EMITTED:  return ctx->cache.quads_emitted();
    case LHU_QC_RUNS_REPLAYED:  return ctx->cache.runs_replayed();
    case LHU_QC_FRAMES_FAST:    return ctx->cache.frames_fast();
    case LHU_QC_FRAMES_REBUILD: return ctx->cache.frames_rebuild();
    case LHU_QC_FRAMES_PARTIAL: return ctx->cache.frames_partial();
    case LHU_QC_RESET:          ctx->cache.reset_stats(); return 0;
    case LHU_QC_BYTES:          return ctx->cache.bytes();
    case LHU_QC_INVALIDATE:
        ctx->cache.invalidate_all();
        ctx->dirty_suspect = true;
        return 0;
    default:                    return -1;
    }
}
LHU_API_CATCH(ctx, -1)

const char* lhu_last_error(LhuContext* ctx)
{
    return ctx ? ctx->last_error.c_str() : "null context";
}

const char* lhu_cursor(LhuContext* ctx)
{
    return ctx ? ctx->container.cursor().c_str() : "auto";
}

const char* lhu_caption(LhuContext* ctx)
{
    return ctx ? ctx->container.caption().c_str() : "";
}

} // extern "C"
