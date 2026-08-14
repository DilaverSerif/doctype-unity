// LiteHtmlUnity — flat C ABI consumed by Unity through P/Invoke.
//
// Threading: a context is NOT thread-safe. Create it, feed it and read from it
// on one thread (Unity's main thread).

#ifndef LHU_API_H
#define LHU_API_H

#include "lhu_types.h"

#if defined(_WIN32)
#define LHU_API __declspec(dllexport)
#else
#define LHU_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LhuContext LhuContext;

// Library version, e.g. "0.1.0".
LHU_API const char* lhu_version(void);

// Size of LhuQuad as the native side sees it. C# asserts this against its own
// Marshal.SizeOf so a layout drift can never go unnoticed.
LHU_API int32_t lhu_quad_size(void);

LHU_API LhuContext* lhu_create(const LhuHostCallbacks* host);
LHU_API void        lhu_destroy(LhuContext* ctx);

// --- fonts -----------------------------------------------------------------

// `data` is copied. `weight` follows CSS (400 normal, 700 bold).
LHU_API int32_t lhu_register_font(LhuContext* ctx, const char* family, int32_t weight, int32_t italic,
                                  const uint8_t* data, int32_t len);

LHU_API void lhu_set_default_font(LhuContext* ctx, const char* family, float size);

// --- document --------------------------------------------------------------

// Which default stylesheet litehtml applies before the page's own CSS.
//
// litehtml re-parses this for every document, and it dominates the cost of
// building one. LHU_MASTER_CSS_GAME_UI is a trimmed version that drops form
// controls and legacy tags a game UI never uses; see lhu_master_css.h for the
// exact list and the measurements.
enum LhuMasterCss
{
    LHU_MASTER_CSS_GAME_UI = 0, // trimmed, ~2.4x faster document creation
    LHU_MASTER_CSS_FULL    = 1, // litehtml's own, full fidelity
};

LHU_API void lhu_set_master_css(LhuContext* ctx, int32_t mode);

LHU_API void lhu_set_viewport(LhuContext* ctx, float width, float height);
LHU_API void lhu_set_device_scale(LhuContext* ctx, float scale);

// Parses HTML. `user_css` may be NULL. Returns 1 on success.
LHU_API int32_t lhu_load_html(LhuContext* ctx, const char* html, const char* user_css);

// --- DOM mutation -----------------------------------------------------------
//
// Replaces the text content of the first element matching `selector` (the same
// CSS selector syntax litehtml uses for stylesheets). Returns 1 if an element
// matched and its text actually changed, 0 otherwise -- including when the new
// text is identical to the old one, so a caller can use the return value to
// decide whether a re-layout is needed at all.
//
// This exists so that the common case -- a score, an HP number, an item count --
// does not have to go back through lhu_load_html(), which re-parses the whole
// document and re-runs selector matching. Nothing is re-parsed here: the text is
// split into word/space nodes the same way the HTML parser splits it, the
// affected text nodes are re-measured, and the render items for them are
// re-created only when the number of nodes changes.
//
// Constraints, all of which fail closed (return 0 and set lhu_last_error):
//   * the matched element's children must all be text nodes. Setting the text of
//     an element that contains other elements would mean destroying them, which
//     this call deliberately will not do.
//   * the element must have exactly one render item whose children map 1:1 onto
//     those text nodes. That rules out flex and table containers, where litehtml
//     wraps inline content in anonymous boxes that this call must not disturb.
//
// The caller must run lhu_layout() before the next lhu_record(): recording
// without a layout in between draws the previous frame's line boxes.
LHU_API int32_t lhu_set_text(LhuContext* ctx, const char* selector, const char* utf8);

// Replaces the inline style of the first element matching `selector` with `css`
// -- the contents of a style="..." attribute, e.g. "left:40px;background:#e11".
// Returns 1 when an element matched and its style actually changed, 0 otherwise,
// including when `css` is identical to what the element already carries.
//
// This exists for the frame-by-frame case lhu_set_text() cannot serve: an
// animation that moves a bar, resizes a box or recolours a gradient by writing a
// new style attribute. On the demo's animation page that was measured on a
// Redmi Note 12 Pro+ at 3.45 ms of parsing per frame against 0.07 ms of layout
// and 0.31 ms of drawing, i.e. essentially all of the cost was re-parsing HTML
// that had not structurally changed.
//
// WHAT IT REDOES. An element's style="" block is not stored separately: litehtml
// folds it into the element's whole declaration map in compute_styles(), on top
// of the declarations the matched selectors contributed, and both style::add()
// and style::combine() merge rather than replace. Replacing an inline style
// therefore means emptying that map and rebuilding it -- selectors first, new
// inline block second -- and then recomputing the element's computed values and
// those of every descendant, because inherited properties (color, font,
// line-height, white-space) flow downwards. Nothing is re-parsed except the one
// new declaration block, no selector is re-matched, and the DOM is untouched.
//
// `css` may be the empty string, which removes the inline style entirely.
// Declarations litehtml cannot parse are dropped, exactly as they are dropped
// when the same text arrives on a style attribute in parsed HTML.
//
// WHAT IT DOES NOT COVER, stated as plainly as lhu_set_text()'s limits:
//   * It changes one element's inline style. It does not add or remove elements,
//     does not touch class or id, and does not edit a stylesheet rule. A change
//     that has to restructure the DOM still needs lhu_load_html().
//   * It always forces a full document::render(). Unlike lhu_set_text(), which
//     can prove a text change moved nothing, a style declaration has no small set
//     of numbers to compare, so the layout short-circuit is disarmed on every
//     successful call. The caller must run lhu_layout() before lhu_record().
//   * A change to display, float or position rebuilds the whole render tree and
//     throws the retained display list away -- correct, but the frame it lands on
//     costs about what a fresh layout+record costs. It is not a per-frame move.
//   * Selector matching is re-run for the mutated element and for nothing else.
//     That is exact for every selector CSS actually has, because none of them
//     read an element's inline style -- with one exception, an attribute selector
//     on [style], where a descendant's or a ::before's match could change and
//     will not be noticed. No game UI writes one; if a page does, it needs
//     lhu_load_html().
//   * ::before and ::after content is left alone. Re-applying a pseudo selector
//     is not a merge in litehtml -- el_before_after_base::add_style() re-reads
//     `content` and replaces the pseudo element's children outright -- so the
//     generated content keeps the declarations it already had and only
//     re-inherits. That is correct for an inline style edit and is what keeps the
//     render tree valid; a page whose ::before content itself must change needs
//     lhu_load_html().
//   * var() is honoured for the mutated element and for any descendant that
//     carries the var() in its own style="" attribute, but a descendant whose
//     *stylesheet* rule reads a custom property that this call changed keeps the
//     value that was substituted into it at parse time. litehtml substitutes
//     var() destructively into the declaration map, and this call deliberately
//     does not re-derive the descendants' declaration maps.
//   * CSS counters drift: refreshing an element's styles re-applies any
//     counter-increment its selectors declare, and litehtml's own :hover restyle
//     has the same behaviour. A page that animates inline styles on an element
//     under a counter-increment rule should not use this call.
//   * Cost scales with the size of the mutated element's SUBTREE, not with the
//     document. Measured on the 150-row inventory page: 0.012 ms on a leaf span,
//     0.98 ms on the container all 150 rows hang off, against 3.1 ms to re-parse
//     the page. Mutating a leaf is what this is for.
//   * Not thread-safe, like everything else on a context.
LHU_API int32_t lhu_set_style(LhuContext* ctx, const char* selector, const char* css);

// Runs layout at the given available width. Returns the document height.
//
// litehtml has no incremental layout: document::render() re-lays-out every
// element, so the cost scales with the size of the document and not with the
// size of the change. Profiling the four bench pages showed that after a
// four-character lhu_set_text() the renderer still visits every render item --
// 453 of them on the 150-row inventory page, of which 450 (99.3%) sit outside
// the mutated element and could not possibly have moved.
//
// This call therefore short-circuits when the preceding mutations provably
// changed no geometry. A text node contributes exactly one thing to layout: the
// size compute_styles() measured for it. If every node lhu_set_text() rewrote
// came back with a bit-identical size, then every input to line breaking, float
// placement and block sizing is bit-identical too, the existing render tree is
// already the correct answer, and render() would recompute it unchanged. That
// covers the case a game UI actually spends its frames on -- a counter whose
// digit count does not change, which in any font with tabular figures is the
// same width as the digits it replaced.
//
// Anything that could invalidate the assumption -- a new document, a viewport
// or device-scale change, a font change, a mouse or scroll event that may have
// restyled something, a different max_width, or a mutation whose measurement
// *did* move -- forces the full render, as does the very first layout of a
// document. Set LHU_EXP_SUBTREE=0 in the environment (or call
// lhu_exp_set_enabled(ctx, 0)) to disable the short-circuit and always render.
LHU_API float lhu_layout(LhuContext* ctx, float max_width);

// Walks the laid-out tree and fills `out` with this frame's quads plus the
// current atlas state. Pointers in `out` stay valid until the next call.
LHU_API void lhu_record(LhuContext* ctx, LhuFrame* out);

// Laid-out rectangle of the first element matching `selector`, in document
// coordinates, after lhu_layout(). Returns 1 when an element matched.
//
// The document itself is never narrower than the viewport it was laid out
// against -- litehtml has no shrink-to-fit at the document level -- so a host
// that wants to size a surface to its content has to ask for the content's own
// box. Lay out against the widest the element is allowed to be, then read it
// back with this and resize.
//
// Any of the out pointers may be NULL.
LHU_API int32_t lhu_element_rect(LhuContext* ctx, const char* selector, float* x, float* y, float* w, float* h);

// Id of the topmost element covering a point in document coordinates, after
// lhu_layout(). Writes at most `len` bytes including the terminator and returns
// 1 when an element carrying an id was found.
//
// This is a pure query: unlike driving lhu_mouse_move() to find out what is
// under the pointer, it changes no hover or active state. That is what makes it
// usable as a drop target test while a drag is in flight.
//
// Elements without an id are transparent to it, so the icon inside a slot
// reports the slot -- which is normally what a caller wants to act on.
LHU_API int32_t lhu_element_at(LhuContext* ctx, float x, float y, char* out_id, int32_t len);

LHU_API float lhu_doc_width(LhuContext* ctx);
LHU_API float lhu_doc_height(LhuContext* ctx);

// --- input (all return 1 when something changed and a redraw is needed) -----

LHU_API int32_t lhu_mouse_move(LhuContext* ctx, float x, float y);
LHU_API int32_t lhu_mouse_down(LhuContext* ctx, float x, float y);
LHU_API int32_t lhu_mouse_up(LhuContext* ctx, float x, float y);
LHU_API int32_t lhu_mouse_leave(LhuContext* ctx);

// Returns the number of elements that consumed the scroll. Zero means the host
// should scroll the view itself.
LHU_API int32_t lhu_scroll(LhuContext* ctx, float dx, float dy, float x, float y);

// --- diagnostics -----------------------------------------------------------

// --- experiment: incremental layout ----------------------------------------
//
// lhu_layout()'s short-circuit is switchable at runtime so that one binary can
// be measured both ways. Comparing two separately built binaries is not
// reliable here: adding a measurement pass to the benchmark has been observed
// to move the timings of untouched code by 10-28%.
//
// The default comes from the LHU_EXP_SUBTREE environment variable, read once
// per context at lhu_create(): "0" disables the fast path, anything else (or an
// unset variable) enables it.
LHU_API void lhu_exp_set_enabled(LhuContext* ctx, int32_t enabled);

// Reports the current setting and how many lhu_layout() calls each path has
// served since the context was created. Any pointer may be NULL.
LHU_API void lhu_exp_stats(LhuContext* ctx, int32_t* out_enabled, int32_t* out_skipped, int32_t* out_rendered);

// --- experiment: inline style mutation (E7) ---------------------------------
//
// lhu_set_style() is switchable at runtime for the same reason every other
// experiment here is: one binary has to be measurable both ways on the device.
// The default comes from LHU_EXP_SETSTYLE, read once per context at
// lhu_create(); "0" disables, anything else (or unset) enables.
//
// Disabled means lhu_set_style() refuses the call and returns 0, so a host falls
// back to rebuilding the page with lhu_load_html() -- which is the baseline the
// feature is measured against. It never silently half-applies.
//
// Two further environment variables are measurement knobs, not features, and
// both are off by default:
//
//   LHU_SETSTYLE_DEEP=1  re-runs litehtml's recursive refresh_styles() over the
//     whole mutated subtree instead of only the element. Measured at +29% on a
//     450-element subtree (0.98 -> 1.27 ms) for byte-identical output, which is
//     the evidence that the descendants' selector matching is redundant. It also
//     takes the ::before path described above, so it rebuilds the render tree
//     whenever the mutated subtree contains generated content -- correct, but an
//     order of magnitude more expensive.
//
//   LHU_SETSTYLE_MEMO=1  stops bypassing the E6 inline-style parse memo, i.e.
//     feeds every one-shot animation string into it. Measured: the memo fills to
//     its 1024-entry cap within three bench pages and stays there, after which no
//     genuinely repeated string can ever be memoized again. Per-call time is
//     inside the noise; the cap is the damage.
LHU_API void lhu_exp_setstyle_set_enabled(LhuContext* ctx, int32_t enabled);

// Reports the setting, how many lhu_set_style() calls actually changed
// something, how many of those had to rebuild the render tree, and how many
// entries the inline-style parse memo (E6) is currently holding -- the last one
// because a page that writes a unique style string every frame is exactly the
// workload that memo is worst at, and it should be watched. Any pointer may be
// NULL.
LHU_API void lhu_exp_setstyle_stats(LhuContext* ctx, int32_t* out_enabled, int32_t* out_applied,
                                    int32_t* out_tree_rebuilds, int32_t* out_style_cache_entries);

// --- experiment: retained display list --------------------------------------
//
// lhu_record()'s retained display list (EXPERIMENT E2) counters. Switched on
// and off at runtime with the LHU_EXP_QUADCACHE environment variable ("0"
// disables), so one build can be A/B'd against itself -- the same reason
// LHU_EXP_SUBTREE is a runtime toggle rather than a build flag.
enum LhuQuadCacheStat
{
    LHU_QC_ENABLED        = 0,
    LHU_QC_QUADS_REPLAYED = 1, // quads spliced in from a cached run
    LHU_QC_QUADS_EMITTED  = 2, // quads litehtml was actually walked for
    LHU_QC_RUNS_REPLAYED  = 3, // cached subtree runs served
    LHU_QC_FRAMES_FAST    = 4, // frames answered without drawing at all
    LHU_QC_FRAMES_REBUILD = 5, // frames that rebuilt the retained snapshot
    LHU_QC_FRAMES_PARTIAL = 6, // frames that spliced
    LHU_QC_RESET          = 7, // resets the counters, returns 0
    LHU_QC_BYTES          = 8, // bytes the cache is holding on to

    // Throws the retained frame away, returns 0.
    //
    // The engine invalidates itself for everything it can see -- text, styles,
    // layout, the glyph atlas, the device scale. It cannot see the *host's*
    // image atlas: draw_image asks for a UV rect through the get_image_uv
    // callback, and a host that repacks that atlas has silently moved the UVs
    // of every cached image quad. A host that repacks must call this. Nothing
    // else needs it.
    LHU_QC_INVALIDATE     = 9,
};

LHU_API int64_t lhu_quadcache_stat(LhuContext* ctx, int32_t which);

LHU_API const char* lhu_last_error(LhuContext* ctx);
LHU_API const char* lhu_cursor(LhuContext* ctx);
LHU_API const char* lhu_caption(LhuContext* ctx);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LHU_API_H
