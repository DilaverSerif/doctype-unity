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

#include <cstdlib>
#include <cstring>
#include <algorithm>
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
{
    LhuHostCallbacks cb {};
    if(host)
    {
        cb = *host;
    }
    return new(std::nothrow) LhuContext(cb);
}

void lhu_destroy(LhuContext* ctx)
{
    delete ctx;
}

int32_t lhu_register_font(LhuContext* ctx, const char* family, int32_t weight, int32_t italic, const uint8_t* data,
                          int32_t len)
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

void lhu_set_default_font(LhuContext* ctx, const char* family, float size)
{
    if(!ctx)
    {
        return;
    }

    ctx->invalidate_layout_and_cache();
    ctx->fonts.set_default_family(family);
    ctx->container.set_default_font(family ? family : "sans-serif", size > 0.f ? size : 16.f);
}

void lhu_set_master_css(LhuContext* ctx, int32_t mode)
{
    if(ctx)
    {
        ctx->invalidate_layout_and_cache();
        ctx->master_css_mode = mode;
    }
}

void lhu_set_viewport(LhuContext* ctx, float width, float height)
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

void lhu_set_device_scale(LhuContext* ctx, float scale)
{
    if(ctx)
    {
        ctx->invalidate_layout_and_cache();
        ctx->container.set_device_scale(scale);
    }
}

int32_t lhu_load_html(LhuContext* ctx, const char* html, const char* user_css)
{
    if(!ctx || !html)
    {
        return 0;
    }

    ctx->invalidate_layout();

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

    ctx->last_error.clear();
    return 1;
}

int32_t lhu_set_text(LhuContext* ctx, const char* selector, const char* utf8)
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
    // through a line box. There is no version of this that can skip a render.
    ctx->layout_clean     = false;
    ctx->pending_mutation = true;

    // The new render items carry dc_geom_valid == false, so the diff walk would
    // catch them anyway; mark explicitly rather than lean on that, because the
    // *old* items' entries are what would otherwise be replayed for the parent.
    ctx->cache.mark_element_dirty(el.get());

    return 1;
}

int32_t lhu_set_style(LhuContext* ctx, const char* selector, const char* css)
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

    if(structure_changed(before, after))
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

    return 1;
}

float lhu_layout(LhuContext* ctx, float max_width)
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

    ctx->doc->render(litehtml::pixel_t(max_width));
    ctx->doc_width  = static_cast<float>(ctx->doc->width());
    ctx->doc_height = static_cast<float>(ctx->doc->height());

    ctx->layout_clean     = true;
    ctx->pending_mutation = false;
    ctx->layout_width     = max_width;
    ++ctx->stat_rendered;

    return ctx->doc_height;
}

void lhu_record(LhuContext* ctx, LhuFrame* out)
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

int32_t lhu_element_rect(LhuContext* ctx, const char* selector, float* x, float* y, float* w, float* h)
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

float lhu_doc_width(LhuContext* ctx)
{
    return ctx ? ctx->doc_width : 0.f;
}

float lhu_doc_height(LhuContext* ctx)
{
    return ctx ? ctx->doc_height : 0.f;
}

int32_t lhu_mouse_move(LhuContext* ctx, float x, float y)
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    // A :hover rule can change any property, including ones that resize a box,
    // so a hit that reports a change puts the layout back on the slow path.
    const bool changed = ctx->doc->on_mouse_over(litehtml::pixel_t(x), litehtml::pixel_t(y), litehtml::pixel_t(x),
                                                 litehtml::pixel_t(y), kIgnoreRedraw);
    if(changed)
    {
        ctx->invalidate_layout();
    }
    return changed ? 1 : 0;
}

int32_t lhu_mouse_down(LhuContext* ctx, float x, float y)
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    const bool changed = ctx->doc->on_lbutton_down(litehtml::pixel_t(x), litehtml::pixel_t(y), litehtml::pixel_t(x),
                                                   litehtml::pixel_t(y), kIgnoreRedraw);
    if(changed)
    {
        ctx->invalidate_layout();
    }
    return changed ? 1 : 0;
}

int32_t lhu_mouse_up(LhuContext* ctx, float x, float y)
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    const bool changed = ctx->doc->on_lbutton_up(litehtml::pixel_t(x), litehtml::pixel_t(y), litehtml::pixel_t(x),
                                                 litehtml::pixel_t(y), kIgnoreRedraw);
    if(changed)
    {
        ctx->invalidate_layout();
    }
    return changed ? 1 : 0;
}

int32_t lhu_mouse_leave(LhuContext* ctx)
{
    if(!ctx || !ctx->doc)
    {
        return 0;
    }
    const bool changed = ctx->doc->on_mouse_leave(kIgnoreRedraw);
    if(changed)
    {
        ctx->invalidate_layout();
    }
    return changed ? 1 : 0;
}

int32_t lhu_scroll(LhuContext* ctx, float dx, float dy, float x, float y)
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
    }
    return static_cast<int32_t>(scrolled.size());
}

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
    case LHU_QC_INVALIDATE:     ctx->cache.invalidate_all(); return 0;
    default:                    return -1;
    }
}

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
