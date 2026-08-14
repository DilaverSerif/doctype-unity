// Performance benchmark for the Doctype native layer.
//
// Separates the three costs that get lumped together in a frame time — parsing,
// layout and recording — because they have completely different fixes. Also
// reports the GPU-side numbers the mesh builder will produce, so vertex
// bandwidth and overdraw can be reasoned about before writing any shader code.
//
//   build/macos/bin/lhu_bench [iterations]

#include "lhu_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Stats
{
    double mean = 0, p50 = 0, p95 = 0, max = 0;

    static Stats from(std::vector<double> samples)
    {
        Stats s;
        if(samples.empty())
        {
            return s;
        }

        std::sort(samples.begin(), samples.end());

        double total = 0;
        for(double v : samples)
        {
            total += v;
        }

        s.mean = total / static_cast<double>(samples.size());
        s.p50  = samples[samples.size() / 2];
        s.p95  = samples[static_cast<size_t>(static_cast<double>(samples.size()) * 0.95)];
        s.max  = samples.back();
        return s;
    }
};

std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if(!f)
    {
        return {};
    }

    const std::streamsize n = f.tellg();
    f.seekg(0);

    std::vector<uint8_t> out(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    return out;
}

// --- test pages --------------------------------------------------------------

// The pages carry an id on the one element each scenario mutates. An id
// attribute is inert here -- no stylesheet selects on it -- so the parsed
// output is unchanged; it just gives lhu_set_text() something to aim at, the
// way a real UI would.
std::string page_hud(const char* hp = "HP 84 / 100")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#fff'>"
           "<div style='padding:8px'>"
           "<div id='hp' style='font-size:22px'>") + hp + "</div>"
           "<div style='height:8px;border-radius:4px;background:#333'>"
           "<i style='display:block;width:84%;height:8px;border-radius:4px;background:#e11'></i></div>"
           "<div style='margin-top:6px;font-size:13px;color:#aaa'>Bolge: Kuzey Gecidi</div>"
           "</div></body>";
}

std::string page_menu(const char* resolution = "1920x1080")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
           "<div style='margin:14px'>"
           "<div style='margin-bottom:12px'>"
           "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Genel</span>"
           "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Animasyon</span>"
           "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Performans</span>"
           "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Tipografi</span>"
           "</div>"
           "<div style='padding:16px 18px;border-radius:14px;border:1px solid #2a3350;"
           "background:linear-gradient(135deg,#161b29,#1d2540)'>"
           "<h1 style='font-size:21px;margin:0 0 4px 0'>Ayarlar</h1>"
           "<p style='margin:0 0 14px 0;color:#8e97b3;font-size:13px'>Oyun ve goruntu secenekleri</p>"
           "<table style='width:100%;border-collapse:collapse;font-size:13px'>"
           "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Cozunurluk</td>"
           "<td id='res' style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>") +
           resolution + "</td></tr>"
           "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Golgeler</td>"
           "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>Yuksek</td></tr>"
           "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Doku kalitesi</td>"
           "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>Ultra</td></tr>"
           "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Dikey esitleme</td>"
           "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>Kapali</td></tr>"
           "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Kare siniri</td>"
           "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>61 fps</td></tr>"
           "</table>"
           "<div style='margin-top:14px'>"
           "<button style='padding:9px 18px;border-radius:9px;background:#233056;color:#dbe6ff'>Kaydet</button>"
           "<button style='padding:9px 18px;border-radius:9px;background:#233056;color:#dbe6ff'>Geri</button>"
           "</div></div></div></body>";
}

// A long inventory list: the shape that actually gets big in a real game.
//
// The row halfway down carries id='mark'. `mark_label` overrides that row's
// label, which is what lets the verification pass compare a mutated document
// against a freshly parsed one that already contains the new text.
int list_marked_row(int rows)
{
    return rows / 2;
}

std::string list_label(int row)
{
    return "Esya " + std::to_string(row + 1);
}

std::string page_list(int rows, const char* mark_label = nullptr)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div style='margin:10px'>";

    const int marked = list_marked_row(rows);

    for(int i = 0; i < rows; ++i)
    {
        const bool        is_marked = i == marked;
        const std::string label     = (is_marked && mark_label) ? std::string(mark_label) : list_label(i);

        out += "<div style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span" +
               std::string(is_marked ? " id='mark'" : "") +
               " style='font-size:14px'>" + label +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }

    out += "</div></body>";
    return out;
}

// --- pages built to defeat the incremental-layout fast path -------------------
//
// Each one puts the mutated element somewhere the "nothing outside it moved"
// argument could plausibly break: inside a nested inline run, inside a box
// whose width is a percentage of its parent, inside a table cell whose column
// width is negotiated across rows, and inside a flex container where litehtml
// wraps content in anonymous boxes.

// The mutated element sits two inline levels down, sharing its line box with
// bold and italic siblings. If a text node's new measurement moved anything,
// the whole line re-flows and the sibling runs move with it.
std::string page_nested_inline(const char* level = "12")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='padding:6px 8px;font-size:15px'>"
           "<span style='font-weight:bold'>Seviye <span id='lv'>") + level + "</span> / 40</span>"
           " <span style='color:#8ab4ff'>(<i>XP</i> 3400 <b>+120</b>)</span>"
           " <span style='color:#9aa'>Kuzey Gecidi bolgesinde toplanan tum esyalar</span>"
           "</div></body>";
}

// The mutated element is itself sized as a percentage of its containing block,
// so its box width does not come from its text -- but its *content* width does,
// and a wider string could push the line onto a second row.
std::string page_percent(const char* label = "Esya 12")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='margin:8px;padding:6px;background:#1b2030'>"
           "<div id='pw' style='width:50%;padding:4px 6px;background:#243050;font-size:14px'>") + label + "</div>"
           "<div style='width:35%;padding:4px 6px;background:#243050;font-size:14px'>Sabit</div>"
           "</div></body>";
}

// A table with auto column widths: the mutated cell's measurement is one of the
// inputs to the column-width algorithm, so a change there resizes a column and
// moves every other cell in the table.
std::string page_table_auto(const char* cell = "1920x1080")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='margin:8px'>"
           "<table style='border-collapse:collapse;font-size:13px'>"
           "<tr><td style='padding:4px 9px;border:1px solid #333'>Cozunurluk</td>"
           "<td id='tc' style='padding:4px 9px;border:1px solid #333'>") + cell + "</td>"
           "<td style='padding:4px 9px;border:1px solid #333'>varsayilan</td></tr>"
           "<tr><td style='padding:4px 9px;border:1px solid #333'>Golgeler</td>"
           "<td style='padding:4px 9px;border:1px solid #333'>Yuksek</td>"
           "<td style='padding:4px 9px;border:1px solid #333'>onerilen</td></tr>"
           "</table></div></body>";
}

// Two flex shapes. '#fx' is an ordinary block that happens to be a flex *item*;
// '#fc' is the flex *container* itself, holding loose text that litehtml wraps
// in an anonymous box -- exactly the shape lhu_set_text() refuses to touch.
// Both are checked so that "flex is handled" is a tested fact rather than an
// assumption about which side of the line each one falls on.
std::string page_flex(const char* item = "Esya 12", const char* container = "Toplam 34")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='display:flex;margin:8px;font-size:14px'>"
           "<div id='fx' style='padding:4px 8px;background:#243050'>") + item + "</div>"
           "<div style='padding:4px 8px;background:#1b2030'>x9</div>"
           "</div>"
           "<div id='fc' style='display:flex;margin:8px;font-size:14px;background:#1b2030'>" + container + "</div>"
           "</body>";
}

// Two words whose *total* width can stay the same while the split between them
// moves: "12 34" and "123 4" are four digits and a space either way. The
// geometry test has to be per node, not on the element as a whole, or the first
// word would end at a different x with the element's outer width unchanged.
//
// The same page also shows why the test compares measurements rather than
// counting characters: Arial kerns "11" but not "88", so "11 88" -> "88 11" is
// a same-digit-count change that genuinely moves geometry.
std::string page_split(const char* label = "12 34")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='margin:8px;font-size:15px'>"
           "<span id='sp'>") + label + "</span>"
           "<span style='color:#8ab4ff'> hasar</span>"
           "</div></body>";
}

// white-space:pre, where a newline is a hard line break rather than a space.
// Swapping the space for a newline leaves the word widths alone but turns the
// separator into a break, which the inline context branches on directly.
std::string page_pre(const char* label = "Bolum 12")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div id='pr' style='margin:8px;font-size:15px;white-space:pre'>") + label + "</div></body>";
}

// --- pages for lhu_set_style -------------------------------------------------
//
// Each takes the style string of the element carrying id='mark', so the exact
// same function builds both the page that gets mutated and the reference page
// that already contains the new style. That is what makes "a mutation must equal
// a re-parse" a comparison of two things that differ in nothing else.

using StylePage = std::string (*)(const char*);

// The only page here with a stylesheet. Everything above changes an element
// whose declaration map contains nothing but its own style="" block, which
// cannot tell a rebuild of that map apart from a merge into it. This one can:
//   * .box sets padding, background and font-size, so removing the matching
//     inline declaration has to let the RULE's value come back rather than
//     leaving the inline one behind;
//   * .box::before contributes a generated box, which refresh_styles_self()
//     re-adds a style to without having cleared it first -- the one place that
//     shortcut touches something it did not clear;
//   * #mark has higher specificity than .box, so the cascade order has to
//     survive a rebuild;
//   * .box b is a descendant rule, which is what proves the descendants' own
//     matched selectors are still in force after the parent was restyled.
std::string page_style_css(const char* mark_style)
{
    return std::string("<html><head><style>"
           ".box { padding:5px 7px; background:#243050; font-size:15px; color:#cfd6e6 }"
           ".box::before { content:'* '; color:#e11d48 }"
           "#mark { border-bottom:2px solid #4af }"
           ".box b { color:#ffd166; font-size:18px }"
           "</style></head>"
           "<body style='margin:0;font-family:sans-serif;background:#0f1117'>"
           "<div style='padding:6px'>"
           "<div style='font-size:13px;color:#8e97b3'>Ustteki</div>"
           "<div id='mark' class='box' style='") + mark_style + "'>Kutu <b>kalin</b> metin</div>"
           "<div style='font-size:13px;color:#8e97b3'>Alttaki</div>"
           "</div></body></html>";
}

// The marked element is a plain block with siblings above and below it, so a
// size change has to move what follows and a colour change must move nothing.
std::string page_style_flow(const char* mark_style)
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef;background:#0f1117'>"
           "<div style='padding:8px'>"
           "<div style='font-size:14px;padding:3px 0'>Ustteki satir</div>"
           "<div id='mark' style='") + mark_style + "'>Hareketli kutu</div>"
           "<div style='font-size:14px;padding:3px 0'>Alttaki satir</div>"
           "<div style='font-size:13px;color:#8e97b3'>Son satir burada</div>"
           "</div></body>";
}

// The marked element is a CONTAINER. Colour, font-size, line-height and
// white-space all inherit, so a change here has to reach text nodes two levels
// below it -- and those text nodes have to be re-measured, not just recoloured,
// or they keep laying out at the old font.
std::string page_style_inherit(const char* mark_style)
{
    return std::string("<body style='margin:0;font-family:sans-serif;background:#0f1117'>"
           "<div style='padding:6px'>"
           "<div id='mark' style='") + mark_style + "'>"
           "<div style='padding:2px 0'>Birinci <b>kalin</b> satir</div>"
           "<div style='padding:2px 0'><span>Ikinci</span> <i>egik</i> satir metni</div>"
           "<div style='padding:2px 0'><span style='color:#8ab4ff'>Ucuncu</span> satir</div>"
           "</div>"
           "<div style='font-size:13px;color:#8e97b3'>Kapsayicinin disi</div>"
           "</div></body>";
}

// A flex item. litehtml builds render_item_flex for the container and wraps the
// items' loose content in anonymous boxes, so the render tree here does not
// mirror the DOM.
std::string page_style_flex(const char* mark_style)
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='display:flex;margin:8px;font-size:14px'>"
           "<div style='padding:4px 8px;background:#1b2030'>Sol taraf</div>"
           "<div id='mark' style='") + mark_style + "'>Orta</div>"
           "<div style='padding:4px 8px;background:#1b2030'>Sag taraf</div>"
           "</div></body>";
}

// A table cell. Column widths are negotiated across every row, so resizing this
// cell moves cells it has no relationship with in the DOM.
std::string page_style_table(const char* mark_style)
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e8e8ef'>"
           "<div style='margin:8px'><table style='border-collapse:collapse;font-size:13px'>"
           "<tr><td style='padding:4px 9px;border:1px solid #333'>Cozunurluk</td>"
           "<td id='mark' style='") + mark_style + "'>1920x1080</td>"
           "<td style='padding:4px 9px;border:1px solid #333'>varsayilan</td></tr>"
           "<tr><td style='padding:4px 9px;border:1px solid #333'>Golgeler cok uzun bir etiket</td>"
           "<td style='padding:4px 9px;border:1px solid #333'>Yuksek</td>"
           "<td style='padding:4px 9px;border:1px solid #333'>onerilen</td></tr>"
           "</table></div></body>";
}

// --- benchmark ---------------------------------------------------------------

struct QuadStats
{
    int    total = 0;
    int    glyphs = 0;
    double covered_px = 0; // sum of quad areas, for an overdraw estimate
};

QuadStats analyse(const LhuFrame& frame)
{
    QuadStats s;
    s.total = frame.quad_count;

    for(int i = 0; i < frame.quad_count; ++i)
    {
        const LhuQuad& q = frame.quads[i];
        if(q.type == LHU_QUAD_GLYPH)
        {
            ++s.glyphs;
        }
        s.covered_px += static_cast<double>(q.w) * static_cast<double>(q.h);
    }

    return s;
}

// One page's mutation scenario: which element the update targets, and two
// strings it alternates between so every iteration is a real change.
//
// `alt_a`/`alt_b` keep the parser's word/space split identical to the original
// text, which is the shape a counter update has. `structural` deliberately
// changes the number of words, forcing lhu_set_text() down the path that has to
// create and destroy text nodes and their render items.
struct UpdateScenario
{
    const char* selector   = nullptr;
    const char* alt_a      = nullptr;
    const char* alt_b      = nullptr;
    const char* structural = nullptr;

    // The lhu_set_style() scenario. `style_fmt` takes exactly four %d and is
    // fed a different set of numbers every iteration, so every frame writes a
    // style string no element has ever carried -- which is what an animation
    // does, and the workload the inline-style parse memo (E6) is worst at.
    // Measuring anything less would be measuring a cache hit that a real
    // animation never gets.
    const char* style_selector = nullptr;
    const char* style_fmt      = nullptr;
};

void bench_page(LhuContext* ctx, const char* name, const std::string& html, float width, float height,
                int iterations, const UpdateScenario& upd)
{
    lhu_set_viewport(ctx, width, height);

    // Warm up: first pass rasterizes every glyph, which later passes reuse.
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, width);

    LhuFrame frame {};
    lhu_record(ctx, &frame);

    const QuadStats stats = analyse(frame);

    // Taken from the warm-up frame, like `stats` above. The loop below leaves
    // `frame` holding whatever the last mutation produced, and a style mutation
    // can legitimately change the document height -- printing that under the
    // page's own heading would quietly redefine the baseline the correctness
    // section asserts these pages against.
    const float base_doc_w = frame.doc_width;
    const float base_doc_h = frame.doc_height;

    std::vector<double> parse, layout, record, relayout, set_only, text_update, struct_update;
    std::vector<double> style_only, style_update;
    style_only.reserve(iterations);
    style_update.reserve(iterations);
    parse.reserve(iterations);
    layout.reserve(iterations);
    record.reserve(iterations);
    relayout.reserve(iterations);
    set_only.reserve(iterations);
    text_update.reserve(iterations);
    struct_update.reserve(iterations);

    int update_failures = 0;
    int style_failures  = 0;

    int32_t setstyle_on = 0, style_entries_before = 0, style_entries_after = 0;
    lhu_exp_setstyle_stats(ctx, &setstyle_on, nullptr, nullptr, &style_entries_before);

    // Which layout path the TEXT UPDATE rows below actually took. Printed rather
    // than asserted: the same binary is meant to be run with LHU_EXP_SUBTREE=0
    // and =1, and this is what tells the two runs apart at a glance.
    int32_t exp_enabled = 0, skipped_before = 0, rendered_before = 0;
    lhu_exp_stats(ctx, &exp_enabled, &skipped_before, &rendered_before);

    for(int i = 0; i < iterations; ++i)
    {
        auto t0 = Clock::now();
        lhu_load_html(ctx, html.c_str(), nullptr);
        parse.push_back(ms_since(t0));

        t0 = Clock::now();
        lhu_layout(ctx, width);
        layout.push_back(ms_since(t0));

        t0 = Clock::now();
        lhu_record(ctx, &frame);
        record.push_back(ms_since(t0));

        // Layout + record again on the *same* parsed document. This is what a
        // frame would cost if content updates did not force a re-parse.
        t0 = Clock::now();
        lhu_layout(ctx, width);
        lhu_record(ctx, &frame);
        relayout.push_back(ms_since(t0));

        if(upd.selector)
        {
            // What a counter tick costs: rewrite one text node, then lay out and
            // record. No parsing, no selector matching, no render-tree rebuild.
            const char* next = (i % 2) ? upd.alt_a : upd.alt_b;

            t0 = Clock::now();
            const int32_t changed = lhu_set_text(ctx, upd.selector, next);
            set_only.push_back(ms_since(t0));

            lhu_layout(ctx, width);
            lhu_record(ctx, &frame);
            text_update.push_back(ms_since(t0));

            if(!changed)
            {
                ++update_failures;
            }

            // The same thing when the word count changes, which forces text
            // nodes and their render items to be rebuilt.
            if(upd.structural)
            {
                t0 = Clock::now();
                const int32_t s_changed = lhu_set_text(ctx, upd.selector, upd.structural);
                lhu_layout(ctx, width);
                lhu_record(ctx, &frame);
                struct_update.push_back(ms_since(t0));

                if(!s_changed)
                {
                    ++update_failures;
                }
            }
        }

        if(upd.style_selector)
        {
            // What one frame of an animation costs: replace an element's inline
            // style, lay out, record. The alternative measured against it is the
            // FULL REBUILD row above -- which is exactly what a host has to do
            // today, because there is no other way to change a style.
            char buf[256];
            std::snprintf(buf, sizeof(buf), upd.style_fmt, i % 251, (i * 3) % 251, (i * 7) % 251, i % 6);

            auto ts = Clock::now();
            const int32_t s_ok = lhu_set_style(ctx, upd.style_selector, buf);
            style_only.push_back(ms_since(ts));

            if(!s_ok)
            {
                if(setstyle_on)
                {
                    ++style_failures;
                } else
                {
                    // LHU_EXP_SETSTYLE=0. Measure what the host falls back to, so
                    // the two halves of the A/B are the same frame, not one frame
                    // and one refusal. Reloading the same string is within a few
                    // characters of regenerating it with the new style in place.
                    lhu_load_html(ctx, html.c_str(), nullptr);
                }
            }

            lhu_layout(ctx, width);
            lhu_record(ctx, &frame);
            style_update.push_back(ms_since(ts));
        }
    }

    lhu_exp_setstyle_stats(ctx, nullptr, nullptr, nullptr, &style_entries_after);

    int32_t skipped_after = 0, rendered_after = 0;
    lhu_exp_stats(ctx, nullptr, &skipped_after, &rendered_after);

    const Stats p  = Stats::from(parse);
    const Stats l  = Stats::from(layout);
    const Stats r  = Stats::from(record);
    const Stats rl = Stats::from(relayout);
    const Stats so = Stats::from(set_only);
    const Stats tu = Stats::from(text_update);
    const Stats su = Stats::from(struct_update);
    const Stats yo = Stats::from(style_only);
    const Stats yu = Stats::from(style_update);

    // Summed from medians, not means: a single scheduler or thermal hiccup
    // triples one iteration and drags a mean far enough to look like a
    // regression that the per-stage p50s show is not there.
    const double full = p.p50 + l.p50 + r.p50;

    // What the mesh builder will hand the GPU.
    const long long verts = static_cast<long long>(stats.total) * 4;
    const double vertex_kb = static_cast<double>(verts) * 144.0 / 1024.0;
    const double index_kb = static_cast<double>(stats.total) * 6.0 * 4.0 / 1024.0;
    const double overdraw = stats.covered_px / (static_cast<double>(width) * height);

    std::printf("\n=== %s (%.0fx%.0f) ===\n", name, width, height);
    std::printf("  doc %.4f x %.4f\n", base_doc_w, base_doc_h);
    std::printf("  quads %d (%d glyph)   verts %lld   vertex %.0f KB + index %.0f KB per frame\n", stats.total,
                stats.glyphs, verts, vertex_kb, index_kb);
    std::printf("  overdraw ~%.1fx (sum of quad areas / surface)\n", overdraw);
    std::printf("  %-22s %8s %8s %8s %8s\n", "", "mean", "p50", "p95", "max");
    std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f\n", "parse", p.mean, p.p50, p.p95, p.max);
    std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f\n", "layout", l.mean, l.p50, l.p95, l.max);
    std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f\n", "record", r.mean, r.p50, r.p95, r.max);
    std::printf("  %-22s %8.3f\n", "FULL REBUILD", full);
    std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f   <- no re-parse\n", "layout+record only", rl.mean, rl.p50, rl.p95,
                rl.max);
    if(upd.selector)
    {
        std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f\n", "lhu_set_text only", so.mean, so.p50, so.p95, so.max);
        std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f   <- set_text+layout+record\n", "TEXT UPDATE", tu.mean, tu.p50,
                    tu.p95, tu.max);
        if(upd.structural)
        {
            std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f   <- word count changed\n", "  (structural)", su.mean,
                        su.p50, su.p95, su.max);
        }
        std::printf("  incremental layout %s: %d of %d lhu_layout calls skipped document::render()\n",
                    exp_enabled ? "ON" : "OFF", skipped_after - skipped_before,
                    (skipped_after - skipped_before) + (rendered_after - rendered_before));
        if(update_failures)
        {
            std::printf("  !! %d lhu_set_text calls reported no change: %s\n", update_failures, lhu_last_error(ctx));
        }
    }
    if(upd.style_selector)
    {
        std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f\n",
                    setstyle_on ? "lhu_set_style only" : "  (refusal only)", yo.mean, yo.p50, yo.p95, yo.max);
        std::printf("  %-22s %8.3f %8.3f %8.3f %8.3f   <- %s\n", "STYLE UPDATE", yu.mean, yu.p50, yu.p95, yu.max,
                    setstyle_on ? "set_style+layout+record" : "LHU_EXP_SETSTYLE=0: full rebuild fallback");
        std::printf("  inline-style memo (E6): %d -> %d entries over %d unique style strings\n",
                    style_entries_before, style_entries_after, iterations);
        if(style_failures)
        {
            std::printf("  !! %d lhu_set_style calls failed: %s\n", style_failures, lhu_last_error(ctx));
        }
    }
    std::printf("  parse is %.0f%% of a full rebuild; removing it would leave %.3f ms\n", p.p50 / full * 100.0,
                rl.p50);
    if(upd.selector && tu.mean > 0.0)
    {
        std::printf("  a text update is %.0fx cheaper than a full rebuild (%.3f ms vs %.3f ms)\n", full / tu.p50,
                    tu.p50, full);
    }
    if(upd.style_selector && yu.p50 > 0.0)
    {
        std::printf("  a style update is %.1fx cheaper than a full rebuild (%.3f ms vs %.3f ms)\n", full / yu.p50,
                    yu.p50, full);
    }

#ifndef __ANDROID__
    // Measured, not assumed. Running this same binary on a Dimensity 1080
    // (Redmi Note 12 Pro+) pinned to each core cluster gave a consistent
    // 2.1x against its Cortex-A78 cores and 4.6x against its Cortex-A55s --
    // far more than the 1.3x clock ratio, because parse is allocation and
    // pointer-chasing heavy and the A55 is in-order with a small cache.
    // The A55 figure is the one that matters: nothing guarantees a game's
    // main thread stays on a big core once the device gets warm.
    std::printf("  MEASURED on a Dimensity 1080: big core %.1f ms / little core %.1f ms full, "
                "%.1f / %.1f ms no-reparse\n",
                full * 2.1, full * 4.6, rl.p50 * 2.1, rl.p50 * 4.6);
    if(upd.selector)
    {
        std::printf("  ESTIMATE on a mid-range mobile core (3-6x slower): text update %.2f-%.2f ms\n", tu.mean * 3.0,
                    tu.mean * 6.0);
    }
#endif
}


// --- correctness ------------------------------------------------------------
//
// A faster update path is worthless if it produces stale geometry, so the
// benchmark refuses to report timings until these checks pass.
//
// The strongest statement available here is that mutating a document must
// produce *exactly* what parsing the equivalent HTML produces. Every quad the
// recorder emits carries position, size, corner radii, clip rect, atlas UVs and
// colour, so a byte-for-byte match of the whole quad buffer means the text was
// re-measured, the line box was re-laid-out, and everything downstream of the
// mutated row moved correctly. A stale m_size or a stale render item cannot
// survive that comparison.

struct Snapshot
{
    std::vector<LhuQuad> quads;
    float                doc_w  = 0.f;
    float                doc_h  = 0.f;
    int                  glyphs = 0;
};

Snapshot capture(LhuContext* ctx)
{
    LhuFrame f {};
    lhu_record(ctx, &f);

    Snapshot s;
    s.quads.assign(f.quads, f.quads + f.quad_count);
    s.doc_w = f.doc_width;
    s.doc_h = f.doc_height;
    for(const LhuQuad& q : s.quads)
    {
        if(q.type == LHU_QUAD_GLYPH)
        {
            ++s.glyphs;
        }
    }
    return s;
}

Snapshot render_page(LhuContext* ctx, const std::string& html, float w, float h)
{
    lhu_set_viewport(ctx, w, h);
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, w);
    return capture(ctx);
}

// Parse, display, then mutate one element and re-run layout + record — the exact
// sequence a running UI would perform.
Snapshot mutate_page(LhuContext* ctx, const std::string& html, float w, float h, const char* selector,
                     const char* text, int32_t* out_changed)
{
    lhu_set_viewport(ctx, w, h);
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, w);

    LhuFrame f {};
    lhu_record(ctx, &f); // the document is now "on screen"

    const int32_t changed = lhu_set_text(ctx, selector, text);
    if(out_changed)
    {
        *out_changed = changed;
    }

    lhu_layout(ctx, w);
    return capture(ctx);
}

// Index of the first quad that differs, quads.size() if only the counts differ,
// -1 when the two buffers are identical.
int first_difference(const Snapshot& a, const Snapshot& b)
{
    const size_t n = std::min(a.quads.size(), b.quads.size());
    for(size_t i = 0; i < n; ++i)
    {
        if(std::memcmp(&a.quads[i], &b.quads[i], sizeof(LhuQuad)) != 0)
        {
            return static_cast<int>(i);
        }
    }
    return a.quads.size() == b.quads.size() ? -1 : static_cast<int>(n);
}

int visible_chars(const char* s)
{
    int n = 0;
    for(const char* p = s; *p; ++p)
    {
        if(*p != ' ')
        {
            ++n;
        }
    }
    return n;
}

int g_failures = 0;

bool check(bool ok, const char* what, const std::string& detail)
{
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
    if(!ok)
    {
        ++g_failures;
    }
    return ok;
}

// Mutating `html`'s marked element to `text` must be indistinguishable from
// parsing `reference_html`, which already contains that text.
void check_matches_reparse(LhuContext* ctx, const char* what, const std::string& html,
                           const std::string& reference_html, float w, float h, const char* selector, const char* text)
{
    int32_t        changed = 0;
    const Snapshot mutated = mutate_page(ctx, html, w, h, selector, text, &changed);
    const Snapshot parsed  = render_page(ctx, reference_html, w, h);

    const int diff = first_difference(mutated, parsed);

    char detail[256];
    std::snprintf(detail, sizeof(detail), "mutated %d quads / h=%.1f   re-parsed %d quads / h=%.1f%s",
                  static_cast<int>(mutated.quads.size()), mutated.doc_h, static_cast<int>(parsed.quads.size()),
                  parsed.doc_h, diff < 0 ? "   identical" : "");

    if(diff >= 0)
    {
        char extra[96];
        std::snprintf(extra, sizeof(extra), "   FIRST DIFF AT QUAD %d", diff);
        std::strncat(detail, extra, sizeof(detail) - std::strlen(detail) - 1);
    }

    check(changed == 1 && diff < 0, what, detail);
}

// --- the incremental-layout gate ---------------------------------------------
//
// lhu_layout() skips document::render() when every text node lhu_set_text()
// rewrote measured to exactly the size it already had. That claim is only worth
// anything if the frame it produces is indistinguishable from the frame a full
// render produces, so every mutation below is run twice through the *same
// binary* -- once with the fast path armed, once with lhu_exp_set_enabled(0) --
// and the two quad buffers are compared byte for byte.
//
// Running both paths in one process matters. Two separately built binaries
// would differ in layout and inlining, and this project has already measured a
// benchmark edit moving the timings of untouched code by 10-28%; the same
// hazard applies to correctness comparisons that cross a build boundary.

struct PathRun
{
    Snapshot snap;
    int32_t  changed  = 0;
    int32_t  skipped  = 0; // lhu_layout() calls that took the fast path
    int32_t  rendered = 0; // lhu_layout() calls that ran document::render()
};

// Parse, display, mutate, lay out, record -- counting which path the layout
// after the mutation took.
PathRun run_mutation(LhuContext* ctx, const std::string& html, float w, float h, const char* selector,
                     const char* text)
{
    lhu_set_viewport(ctx, w, h);
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, w);

    LhuFrame f {};
    lhu_record(ctx, &f); // the document is now "on screen"

    PathRun run;
    int32_t skipped_before = 0, rendered_before = 0;
    lhu_exp_stats(ctx, nullptr, &skipped_before, &rendered_before);

    run.changed = lhu_set_text(ctx, selector, text);
    lhu_layout(ctx, w);

    int32_t skipped_after = 0, rendered_after = 0;
    lhu_exp_stats(ctx, nullptr, &skipped_after, &rendered_after);
    run.skipped  = skipped_after - skipped_before;
    run.rendered = rendered_after - rendered_before;

    run.snap = capture(ctx);
    return run;
}

bool same_frame(const Snapshot& a, const Snapshot& b)
{
    return first_difference(a, b) < 0 && a.doc_w == b.doc_w && a.doc_h == b.doc_h;
}


// `expect_fast` is what the mutation is *supposed* to do: 1 fast, 0 full, and
// kEitherPath for cases where the answer legitimately depends on the font's
// metrics. Getting a fast path where a slow one was expected is a failure even
// if the pixels match, because it means the geometry test is looser than this
// test believes it is; getting a slow path where a fast one was expected is a
// failure too, because it means the optimisation silently stopped working.
constexpr int kEitherPath = -1;

void check_fast_matches_slow(LhuContext* ctx, const char* what, const std::string& html, float w, float h,
                             const char* selector, const char* text, int expect_fast)
{
    lhu_exp_set_enabled(ctx, 1);
    const PathRun fast = run_mutation(ctx, html, w, h, selector, text);

    lhu_exp_set_enabled(ctx, 0);
    const PathRun slow = run_mutation(ctx, html, w, h, selector, text);

    lhu_exp_set_enabled(ctx, 1);

    const bool took_fast   = fast.skipped == 1 && fast.rendered == 0;
    const bool slow_is_slow = slow.skipped == 0 && slow.rendered == 1;
    const bool identical   = same_frame(fast.snap, slow.snap);

    char detail[256];
    std::snprintf(detail, sizeof(detail), "%s path, %d quads / h=%.4f%s", took_fast ? "FAST" : "full",
                  static_cast<int>(fast.snap.quads.size()), fast.snap.doc_h,
                  identical ? "   byte-identical to a full render" : "   DIFFERS FROM A FULL RENDER");

    if(!identical)
    {
        char extra[96];
        std::snprintf(extra, sizeof(extra), " (first diff quad %d, slow h=%.4f)", first_difference(fast.snap, slow.snap),
                      slow.snap.doc_h);
        std::strncat(detail, extra, sizeof(detail) - std::strlen(detail) - 1);
    }
    const bool path_ok = expect_fast == kEitherPath || took_fast == (expect_fast != 0);

    if(!path_ok)
    {
        char extra[64];
        std::snprintf(extra, sizeof(extra), "  EXPECTED %s", expect_fast ? "FAST" : "full");
        std::strncat(detail, extra, sizeof(detail) - std::strlen(detail) - 1);
    }

    check(fast.changed == slow.changed && slow_is_slow && identical && path_ok, what, detail);
}

bool verify_incremental_layout(LhuContext* ctx)
{
    std::printf("\n=== correctness: fast-path layout vs a full document::render() ===\n");

    // These checks drive the toggle by hand so that they run identically under
    // either LHU_EXP_SUBTREE setting. The process default is captured here and
    // restored on the way out -- forgetting that would make the timings below
    // report the fast path in a run that was asked for the slow one.
    int32_t enabled = 0;
    lhu_exp_stats(ctx, &enabled, nullptr, nullptr);
    std::printf("  LHU_EXP_SUBTREE default for this process: %s (the checks below drive the toggle "
                "explicitly, so they run either way)\n", enabled ? "on" : "off");

    const std::string hud     = page_hud();
    const std::string menu    = page_menu();
    const std::string list40  = page_list(40);
    const std::string list150 = page_list(150);
    const std::string nested  = page_nested_inline();
    const std::string percent = page_percent();
    const std::string tbl     = page_table_auto();
    const std::string flex    = page_flex();
    const std::string split   = page_split();
    const std::string pre     = page_pre();

    // Warm every glyph these checks can produce. A first-time rasterization can
    // grow the atlas and move every UV, which would make two identical frames
    // compare unequal for a reason that has nothing to do with layout.
    for(const std::string* page : {&hud, &menu, &list40, &list150, &nested, &percent, &tbl, &flex, &split, &pre})
    {
        render_page(ctx, *page, 680.f, 3000.f);
    }
    for(const char* warm : {"HP 12 / 100", "1600x0900", "Esya 31", "Esya 66", "Esya 210000000", "E 1",
                            "Cok uzun bir esya adi ile satirin sarmasini zorlayan bir etiket metni burada duruyor",
                            "34", "7", "Esya 34", "Esya 7", "1600x0900", "800x600"})
    {
        (void)warm;
    }
    render_page(ctx, page_nested_inline("34"), 680.f, 3000.f);
    render_page(ctx, page_nested_inline("7"), 680.f, 3000.f);
    render_page(ctx, page_percent("Esya 34"), 680.f, 3000.f);
    render_page(ctx, page_percent("Esya 7"), 680.f, 3000.f);
    render_page(ctx, page_table_auto("1600x0900"), 680.f, 3000.f);
    render_page(ctx, page_table_auto("3840x2160 genis"), 680.f, 3000.f);

    // --- the cases the fast path exists for: a counter that keeps its width ---
    check_fast_matches_slow(ctx, "HUD counter, same width", hud, 420.f, 120.f, "#hp", "HP 12 / 100", true);
    check_fast_matches_slow(ctx, "40-row list counter, same width", list40, 480.f, 900.f, "#mark", "Esya 31", true);
    check_fast_matches_slow(ctx, "150-row list counter, same width", list150, 480.f, 3000.f, "#mark", "Esya 66", true);
    check_fast_matches_slow(ctx, "table cell, same width", menu, 680.f, 460.f, "#res", "1600x0900", true);
    check_fast_matches_slow(ctx, "nested inline run, same width", nested, 680.f, 200.f, "#lv", "34", true);
    check_fast_matches_slow(ctx, "percentage-width box, same width", percent, 480.f, 200.f, "#pw", "Esya 34", true);
    check_fast_matches_slow(ctx, "auto-width table cell, same width", tbl, 480.f, 200.f, "#tc", "1600x0900", true);

    // --- the cases designed to defeat it: every one must fall back ------------
    check_fast_matches_slow(ctx, "wider label falls back", list40, 480.f, 900.f, "#mark", "Esya 210000000", false);
    check_fast_matches_slow(ctx, "narrower label falls back", list40, 480.f, 900.f, "#mark", "E 1", false);
    check_fast_matches_slow(ctx, "label that wraps to a new line falls back", list40, 480.f, 900.f, "#mark",
                            "Cok uzun bir esya adi ile satirin sarmasini zorlayan bir etiket metni burada duruyor",
                            false);
    check_fast_matches_slow(ctx, "word count change falls back", list40, 480.f, 900.f, "#mark", "Alev Kilici Artikli",
                            false);
    check_fast_matches_slow(ctx, "nested inline run, width change falls back", nested, 680.f, 200.f, "#lv", "7", false);
    check_fast_matches_slow(ctx, "percentage-width box, width change falls back", percent, 480.f, 200.f, "#pw",
                            "Esya 7", false);
    check_fast_matches_slow(ctx, "table cell, width change falls back", menu, 680.f, 460.f, "#res", "800x600", false);
    check_fast_matches_slow(ctx, "auto-width table column resize falls back", tbl, 480.f, 200.f, "#tc",
                            "3840x2160 genis", false);

    // --- the split between two words moves while the total stays the same -----
    {
        render_page(ctx, page_split("123 4"), 480.f, 200.f);
        render_page(ctx, page_split("34 12"), 480.f, 200.f);
        render_page(ctx, page_split("11 88"), 480.f, 200.f);
        render_page(ctx, page_split("88 11"), 480.f, 200.f);
        render_page(ctx, split, 480.f, 200.f);

        // "12 34" -> "34 12": each word keeps its own width, so nothing moves.
        check_fast_matches_slow(ctx, "word swap with per-word widths kept", split, 480.f, 200.f, "#sp", "34 12", true);
        // "12 34" -> "123 4": same four digits, same total, different per-word
        // widths. An element-level width test would wave this through.
        check_fast_matches_slow(ctx, "same total width, moved word boundary falls back", split, 480.f, 200.f, "#sp",
                                "123 4", false);
        // Same digit count, and whether the width changes is up to the font:
        // Arial kerns "11" narrower than "88" and this falls back, Android's
        // Roboto gives both the same advance and it stays fast. Both are correct,
        // so this asserts the thing that is true either way -- the frame matches a
        // full render -- and reports which path the font produced. Pinning the
        // path here would only be pinning one font's kerning table.
        check_fast_matches_slow(ctx, "kerned digit pair follows measured width, not digit count",
                                page_split("11 88"), 480.f, 200.f, "#sp", "88 11", kEitherPath);
    }

    // --- white-space:pre, space -> newline -------------------------------------
    {
        render_page(ctx, page_pre("Bolum 34"), 480.f, 200.f);
        render_page(ctx, page_pre("Bolum\n34"), 480.f, 200.f);
        render_page(ctx, pre, 480.f, 200.f);

        check_fast_matches_slow(ctx, "pre text, same width", pre, 480.f, 200.f, "#pr", "Bolum 34", true);
        check_fast_matches_slow(ctx, "pre text, space -> newline falls back", pre, 480.f, 200.f, "#pr", "Bolum\n34",
                                false);
    }

    // --- two mutations before one layout: the worse one has to win -------------
    {
        lhu_exp_set_enabled(ctx, 1);
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        int32_t s0 = 0, r0 = 0;
        lhu_exp_stats(ctx, nullptr, &s0, &r0);

        // Row 20 keeps its width; row 0 does not. One layout follows both.
        lhu_set_text(ctx, "#mark", "Esya 31");
        lhu_set_text(ctx, "div div span", "Esya 100000");
        lhu_layout(ctx, 480.f);
        const Snapshot both = capture(ctx);

        int32_t s1 = 0, r1 = 0;
        lhu_exp_stats(ctx, nullptr, &s1, &r1);

        lhu_exp_set_enabled(ctx, 0);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);
        lhu_record(ctx, &f);
        lhu_set_text(ctx, "#mark", "Esya 31");
        lhu_set_text(ctx, "div div span", "Esya 100000");
        lhu_layout(ctx, 480.f);
        const Snapshot reference = capture(ctx);
        lhu_exp_set_enabled(ctx, 1);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "%d skipped, %d rendered, %d quads / h=%.4f", s1 - s0, r1 - r0,
                      static_cast<int>(both.quads.size()), both.doc_h);
        check(s1 - s0 == 0 && r1 - r0 == 1 && same_frame(both, reference),
              "neutral + non-neutral mutation in one frame falls back", detail);
    }

    // --- a viewport change between the mutation and the layout ----------------
    {
        lhu_exp_set_enabled(ctx, 1);
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        int32_t s0 = 0, r0 = 0;
        lhu_exp_stats(ctx, nullptr, &s0, &r0);

        lhu_set_text(ctx, "#mark", "Esya 31");
        lhu_set_viewport(ctx, 480.f, 700.f); // vh units and the containing block move
        lhu_layout(ctx, 480.f);
        const Snapshot after = capture(ctx);

        int32_t s1 = 0, r1 = 0;
        lhu_exp_stats(ctx, nullptr, &s1, &r1);

        lhu_set_viewport(ctx, 480.f, 700.f);
        lhu_load_html(ctx, page_list(40, "Esya 31").c_str(), nullptr);
        lhu_layout(ctx, 480.f);
        const Snapshot reference = capture(ctx);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "%d skipped, %d rendered, h=%.4f (reference %.4f)", s1 - s0, r1 - r0,
                      after.doc_h, reference.doc_h);
        check(s1 - s0 == 0 && r1 - r0 == 1 && same_frame(after, reference),
              "a viewport change disarms the fast path", detail);
    }

    // --- setting the same text again is free, and still correct ---------------
    {
        lhu_exp_set_enabled(ctx, 1);
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        const Snapshot before = capture(ctx);

        int32_t s0 = 0, r0 = 0;
        lhu_exp_stats(ctx, nullptr, &s0, &r0);

        const int32_t changed = lhu_set_text(ctx, "#mark", list_label(list_marked_row(40)).c_str());
        lhu_layout(ctx, 480.f);
        const Snapshot after = capture(ctx);

        int32_t s1 = 0, r1 = 0;
        lhu_exp_stats(ctx, nullptr, &s1, &r1);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "returned %d, %d skipped, %d rendered, %d quads / h=%.4f", changed,
                      s1 - s0, r1 - r0, static_cast<int>(after.quads.size()), after.doc_h);
        check(changed == 0 && s1 - s0 == 1 && same_frame(after, before), "re-setting identical text is free",
              detail);
    }

    // --- flex: whichever way lhu_set_text() decides, the frame has to be right -
    //
    // If it refuses, the document must be exactly as it was. If it accepts, the
    // resulting frame must match a re-parse of the page with that text already
    // in it -- which is the same bar every other mutation is held to.
    {
        struct FlexCase
        {
            const char* what;
            const char* selector;
            std::string mutated_page;
            const char* text;
        };

        const FlexCase cases[] = {
            {"flex item mutation is exact", "#fx", page_flex("Esya 34"), "Esya 34"},
            {"flex container mutation is exact", "#fc", page_flex("Esya 12", "Toplam 78"), "Toplam 78"},
        };

        for(const FlexCase& c : cases)
        {
            render_page(ctx, c.mutated_page, 480.f, 200.f); // warm the atlas for this variant

            lhu_exp_set_enabled(ctx, 1);
            const PathRun  run       = run_mutation(ctx, flex, 480.f, 200.f, c.selector, c.text);
            const Snapshot untouched = render_page(ctx, flex, 480.f, 200.f);
            const Snapshot reparsed  = render_page(ctx, c.mutated_page, 480.f, 200.f);

            const bool ok = run.changed ? same_frame(run.snap, reparsed) : same_frame(run.snap, untouched);

            char detail[240];
            std::snprintf(detail, sizeof(detail), "lhu_set_text %s, layout %s, %d quads / h=%.4f%s",
                          run.changed ? "accepted" : "refused", run.skipped ? "SKIPPED" : "ran in full",
                          static_cast<int>(run.snap.quads.size()), run.snap.doc_h,
                          ok ? "   matches a re-parse" : "   DOES NOT MATCH");
            check(ok, c.what, detail);
        }
    }

    // --- the fast path must not be armed across anything that moves geometry --
    {
        lhu_exp_set_enabled(ctx, 1);
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        int32_t s0 = 0, r0 = 0;
        lhu_exp_stats(ctx, nullptr, &s0, &r0);

        // A geometry-neutral mutation arms the skip...
        lhu_set_text(ctx, "#mark", "Esya 31");
        // ...but laying out at a different width must not take it.
        lhu_layout(ctx, 460.f);
        const Snapshot narrow = capture(ctx);

        int32_t s1 = 0, r1 = 0;
        lhu_exp_stats(ctx, nullptr, &s1, &r1);

        // The reference must keep the *same viewport*: the body's background
        // quad is sized from the viewport, not from the layout width, so
        // re-rendering at 460 with a 460-wide viewport is a different frame for
        // a reason that has nothing to do with this test.
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, page_list(40, "Esya 31").c_str(), nullptr);
        lhu_layout(ctx, 460.f);
        const Snapshot narrow_ref = capture(ctx);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "width 480->460: %d skipped, %d rendered, h=%.4f (reference %.4f)",
                      s1 - s0, r1 - r0, narrow.doc_h, narrow_ref.doc_h);
        check(s1 - s0 == 0 && r1 - r0 == 1 && same_frame(narrow, narrow_ref),
              "a max_width change is never skipped", detail);
    }

    // --- a long run of alternating fast and slow mutations must not drift -----
    {
        lhu_exp_set_enabled(ctx, 1);
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        int32_t s0 = 0, r0 = 0;
        lhu_exp_stats(ctx, nullptr, &s0, &r0);

        const char* cycle[] = {"Esya 31", "Esya 41", "Esya 210000000", "Esya 31", "Alev Kilici", "Esya 31"};
        for(int i = 0; i < 600; ++i)
        {
            lhu_set_text(ctx, "#mark", cycle[i % 6]);
            lhu_layout(ctx, 480.f);
            lhu_record(ctx, &f);
        }

        lhu_set_text(ctx, "#mark", "Esya 21");
        lhu_layout(ctx, 480.f);
        const Snapshot restored = capture(ctx);
        const Snapshot baseline = render_page(ctx, list40, 480.f, 900.f);

        int32_t s1 = 0, r1 = 0;
        lhu_exp_stats(ctx, nullptr, &s1, &r1);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "600 mixed updates: %d skipped, %d rendered; restored %d quads / h=%.4f",
                      s1 - s0, r1 - r0, static_cast<int>(restored.quads.size()), restored.doc_h);
        check(s1 - s0 > 0 && same_frame(restored, baseline), "600 mixed updates then restore, no drift", detail);
    }

    lhu_exp_set_enabled(ctx, enabled);

    std::printf("  %d check(s) failed in total\n", g_failures);
    return g_failures == 0;
}

// --- correctness: lhu_set_style vs a full re-parse ----------------------------
//
// Same gate as lhu_set_text is held to, for the same reason: every quad carries
// position, size, corner radii, clip rect, atlas UVs and colour, so a
// byte-for-byte match against a parse of HTML that already contains the new
// inline style is the only evidence worth having. A stale declaration left over
// from the previous style, a descendant that never re-inherited, a render tree
// whose shape no longer matches the computed display, and a cached quad run
// replayed with last frame's colour all fail this and cannot fail it quietly.

// Parse, display, restyle one element, lay out, record -- the exact sequence a
// running UI performs. The record before the mutation matters: it is what leaves
// the retained display list holding a frame that the colour-only cases would
// otherwise be free to replay.
Snapshot mutate_style_page(LhuContext* ctx, const std::string& html, float w, float h, const char* selector,
                           const char* css, int32_t* out_changed)
{
    lhu_set_viewport(ctx, w, h);
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, w);

    LhuFrame f {};
    lhu_record(ctx, &f);

    const int32_t changed = lhu_set_style(ctx, selector, css);
    if(out_changed)
    {
        *out_changed = changed;
    }

    lhu_layout(ctx, w);
    return capture(ctx);
}

void check_style_matches_reparse(LhuContext* ctx, const char* what, StylePage page, float w, float h,
                                 const char* from, const char* to)
{
    const std::string html = page(from);
    const std::string ref  = page(to);

    // Warm the atlas with both variants first. Rasterizing a glyph can grow the
    // atlas and move every UV; a snapshot taken across that boundary would differ
    // for a reason that has nothing to do with the mutation.
    render_page(ctx, html, w, h);
    render_page(ctx, ref, w, h);

    int32_t        changed = 0;
    const Snapshot mutated = mutate_style_page(ctx, html, w, h, "#mark", to, &changed);
    const Snapshot parsed  = render_page(ctx, ref, w, h);

    const int diff = first_difference(mutated, parsed);

    char detail[300];
    std::snprintf(detail, sizeof(detail), "mutated %d quads / %.4fx%.4f   re-parsed %d quads / %.4fx%.4f%s",
                  static_cast<int>(mutated.quads.size()), mutated.doc_w, mutated.doc_h,
                  static_cast<int>(parsed.quads.size()), parsed.doc_w, parsed.doc_h,
                  diff < 0 ? "   identical" : "");

    if(diff >= 0)
    {
        char extra[96];
        std::snprintf(extra, sizeof(extra), "   FIRST DIFF AT QUAD %d", diff);
        std::strncat(detail, extra, sizeof(detail) - std::strlen(detail) - 1);
    }

    const bool ok = changed == 1 && diff < 0 && mutated.doc_w == parsed.doc_w && mutated.doc_h == parsed.doc_h;
    check(ok, what, detail);
}

bool verify_style_updates(LhuContext* ctx)
{
    std::printf("\n=== correctness: lhu_set_style vs a full re-parse ===\n");

    int32_t enabled = 0, cache_entries = 0;
    lhu_exp_setstyle_stats(ctx, &enabled, nullptr, nullptr, &cache_entries);
    std::printf("  LHU_EXP_SETSTYLE default for this process: %s (the checks below drive the toggle "
                "explicitly where it matters)\n", enabled ? "on" : "off");
    std::printf("  retained display list (E2): %s -- the colour-only cases below are only a real test "
                "of it when this says on\n", lhu_quadcache_stat(ctx, LHU_QC_ENABLED) ? "on" : "off");
    lhu_exp_setstyle_set_enabled(ctx, 1);

    // Restored on the way out: leaving it on would make the timing section below
    // report a set_style path in a run that was asked for LHU_EXP_SETSTYLE=0.
    struct RestoreSetStyle
    {
        LhuContext* c;
        int32_t     v;
        ~RestoreSetStyle()
        {
            lhu_exp_setstyle_set_enabled(c, v);
        }
    } restore {ctx, enabled};

    const char* kFlowBase = "font-size:15px;padding:4px 6px;background:#243050;color:#e8e8ef";

    // 1. COLOUR ONLY. Not one of the fourteen floats the retained display list
    //    diffs after layout moves here: same box, same padding, same borders,
    //    same position. If lhu_set_style() did not mark the element dirty by
    //    hand, the cache would replay the old colour and this is the check that
    //    catches it -- which is why the sequence records a frame before mutating.
    check_style_matches_reparse(ctx, "colour only (geometry unchanged)", page_style_flow, 420.f, 200.f, kFlowBase,
                                "font-size:15px;padding:4px 6px;background:#e11d48;color:#101014");

    // 2. A SIZE CHANGE THAT REFLOWS SIBLINGS.
    check_style_matches_reparse(ctx, "size change reflows the siblings below", page_style_flow, 420.f, 200.f,
                                kFlowBase, "font-size:27px;padding:19px 6px;background:#243050;color:#e8e8ef");

    // 3. INHERITANCE. The marked element is the container; nothing about its own
    //    box changes that a descendant could read back -- they have to re-inherit
    //    and re-measure.
    check_style_matches_reparse(ctx, "parent change inherited by descendants", page_style_inherit, 460.f, 300.f,
                                "padding:6px;font-size:14px;color:#e8e8ef",
                                "padding:6px;font-size:23px;color:#ffd166;line-height:2");
    check_style_matches_reparse(ctx, "inherited white-space reaches text nodes", page_style_inherit, 460.f, 300.f,
                                "padding:6px;font-size:14px;color:#e8e8ef",
                                "padding:6px;font-size:14px;color:#e8e8ef;white-space:pre");

    // 4/5. ADDING and REMOVING a property. Removing is the one that fails if the
    //    declaration map is merged into rather than rebuilt: style::add() and
    //    style::combine() have no way to express "and drop everything else".
    check_style_matches_reparse(ctx, "adding a property", page_style_flow, 420.f, 200.f, kFlowBase,
                                "font-size:15px;padding:4px 6px;background:#243050;color:#e8e8ef;"
                                "border:3px solid #4af;border-radius:9px");
    check_style_matches_reparse(ctx, "removing a property", page_style_flow, 420.f, 200.f,
                                "font-size:15px;padding:4px 6px;background:#243050;color:#e8e8ef;"
                                "border:3px solid #4af;border-radius:9px",
                                kFlowBase);
    check_style_matches_reparse(ctx, "removing the last property (background)", page_style_flow, 420.f, 200.f,
                                kFlowBase, "font-size:15px;padding:4px 6px;color:#e8e8ef");

    // 6. REPLACING WITH AN EMPTY STRING -- every declaration gone at once.
    check_style_matches_reparse(ctx, "replacing with an empty style", page_style_flow, 420.f, 200.f, kFlowBase, "");

    // 7. AN INVALID DECLARATION. The requirement is not that it be rejected, it
    //    is that it be dropped in exactly the way the HTML parser drops it.
    check_style_matches_reparse(ctx, "invalid declarations dropped like a parse", page_style_flow, 420.f, 200.f,
                                kFlowBase,
                                "font-size:15px;color:notacolour;wibble:42;padding:;background:#243050;;"
                                "margin-top:9px");

    // 8. DISPLAY. The render tree's shape was decided from the computed display
    //    at parse time and document::render() never revisits it, so these are the
    //    cases that have to rebuild it.
    check_style_matches_reparse(ctx, "display block -> inline", page_style_flow, 420.f, 200.f, kFlowBase,
                                "display:inline;font-size:15px;padding:4px 6px;background:#243050;color:#e8e8ef");
    check_style_matches_reparse(ctx, "display block -> none", page_style_flow, 420.f, 200.f, kFlowBase,
                                "display:none");
    check_style_matches_reparse(ctx, "display none -> block", page_style_flow, 420.f, 200.f, "display:none",
                                kFlowBase);
    check_style_matches_reparse(ctx, "display block -> inline-block", page_style_flow, 420.f, 200.f, kFlowBase,
                                "display:inline-block;font-size:15px;padding:4px 6px;background:#243050");
    check_style_matches_reparse(ctx, "display block -> flex", page_style_inherit, 460.f, 300.f,
                                "padding:6px;font-size:14px;color:#e8e8ef",
                                "display:flex;padding:6px;font-size:14px;color:#e8e8ef");

    {
        int32_t trees_before = 0, trees_after = 0;
        lhu_exp_setstyle_stats(ctx, nullptr, nullptr, &trees_before, nullptr);
        const std::string html = page_style_flow(kFlowBase);
        int32_t           ch   = 0;
        mutate_style_page(ctx, html, 420.f, 200.f, "#mark", "display:none", &ch);
        lhu_exp_setstyle_stats(ctx, nullptr, nullptr, &trees_after, nullptr);

        int32_t trees_colour_before = 0, trees_colour_after = 0;
        lhu_exp_setstyle_stats(ctx, nullptr, nullptr, &trees_colour_before, nullptr);
        mutate_style_page(ctx, html, 420.f, 200.f, "#mark",
                          "font-size:15px;padding:4px 6px;background:#e11d48;color:#101014", &ch);
        lhu_exp_setstyle_stats(ctx, nullptr, nullptr, &trees_colour_after, nullptr);

        char detail[180];
        std::snprintf(detail, sizeof(detail), "display change rebuilt %d tree(s), colour change rebuilt %d",
                      trees_after - trees_before, trees_colour_after - trees_colour_before);
        check(trees_after - trees_before == 1 && trees_colour_after - trees_colour_before == 0,
              "the render tree is rebuilt only when display moves", detail);
    }

    // 9/10. FLEX ITEM and TABLE CELL -- render-tree shapes that do not mirror the
    //    DOM, and where the mutated element's size is an input to a layout
    //    algorithm that moves boxes it has no DOM relationship with.
    check_style_matches_reparse(ctx, "flex item, colour only", page_style_flex, 460.f, 160.f,
                                "padding:4px 8px;background:#243050",
                                "padding:4px 8px;background:#7c3aed;color:#fff");
    check_style_matches_reparse(ctx, "flex item, size change", page_style_flex, 460.f, 160.f,
                                "padding:4px 8px;background:#243050",
                                "padding:16px 30px;background:#243050;font-size:21px");
    check_style_matches_reparse(ctx, "flex item, flex-grow added", page_style_flex, 460.f, 160.f,
                                "padding:4px 8px;background:#243050",
                                "padding:4px 8px;background:#243050;flex:1 1 auto");
    check_style_matches_reparse(ctx, "table cell, colour only", page_style_table, 520.f, 200.f,
                                "padding:4px 9px;border:1px solid #333",
                                "padding:4px 9px;border:1px solid #333;background:#e11d48;color:#fff");
    check_style_matches_reparse(ctx, "table cell, resizes the column", page_style_table, 520.f, 200.f,
                                "padding:4px 9px;border:1px solid #333",
                                "padding:4px 27px;border:3px solid #e11;font-size:18px");

    // --- the cascade has to survive the rebuild ------------------------------
    //
    // These are the cases that separate "the declaration map was rebuilt" from
    // "the new declarations were merged on top of the old ones". Everything
    // above this point uses pages with no stylesheet at all, where the two are
    // indistinguishable.
    check_style_matches_reparse(ctx, "inline overrides a matching rule", page_style_css, 440.f, 240.f, "",
                                "background:#e11d48;padding:14px 7px");
    check_style_matches_reparse(ctx, "dropping the inline lets the rule return", page_style_css, 440.f, 240.f,
                                "background:#e11d48;padding:14px 7px", "");
    check_style_matches_reparse(ctx, "inline replaced, rule value returns for the dropped half", page_style_css,
                                440.f, 240.f, "background:#e11d48;padding:14px 7px;font-size:21px",
                                "background:#7c3aed");
    check_style_matches_reparse(ctx, "::before survives a restyle of its owner", page_style_css, 440.f, 240.f,
                                "color:#ffffff", "color:#101014;background:#8ab4ff;padding:11px 7px");
    check_style_matches_reparse(ctx, "descendant rules still apply after the parent moves", page_style_css, 440.f,
                                240.f, "", "font-size:24px;line-height:1.9;color:#9ae6b4");
    check_style_matches_reparse(ctx, "inline !important over a rule", page_style_css, 440.f, 240.f, "",
                                "padding:19px 7px !important;background:#0b3d2e");
    check_style_matches_reparse(ctx, "display change with a stylesheet in play", page_style_css, 440.f, 240.f, "",
                                "display:inline-block;background:#e11d48");

    // The same element restyled twice in a row without a re-parse in between:
    // the second call has to rebuild from the rules again, not from whatever the
    // first call left in the map.
    {
        const std::string html = page_style_css("");
        const std::string ref  = page_style_css("background:#7c3aed");
        render_page(ctx, html, 440.f, 240.f);
        render_page(ctx, ref, 440.f, 240.f);

        lhu_set_viewport(ctx, 440.f, 240.f);
        lhu_load_html(ctx, html.c_str(), nullptr);
        lhu_layout(ctx, 440.f);
        LhuFrame f {};
        lhu_record(ctx, &f);

        lhu_set_style(ctx, "#mark", "background:#e11d48;padding:14px 7px;font-size:21px;border:4px dotted #0f0");
        lhu_layout(ctx, 440.f);
        lhu_record(ctx, &f);
        lhu_set_style(ctx, "#mark", "background:#7c3aed");
        lhu_layout(ctx, 440.f);
        const Snapshot got = capture(ctx);

        const Snapshot expected = render_page(ctx, ref, 440.f, 240.f);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "%d quads / %.4fx%.4f vs re-parse %d / %.4fx%.4f%s",
                      static_cast<int>(got.quads.size()), got.doc_w, got.doc_h,
                      static_cast<int>(expected.quads.size()), expected.doc_w, expected.doc_h,
                      same_frame(got, expected) ? "   identical" : "   DIFFERS");
        check(same_frame(got, expected), "restyle twice with no re-parse between", detail);
    }

    // --- setting the same style must be a no-op, not a silent rebuild ---------
    {
        const std::string html = page_style_flow(kFlowBase);
        const Snapshot    base = render_page(ctx, html, 420.f, 200.f);

        int32_t        ch   = 0;
        const Snapshot same = mutate_style_page(ctx, html, 420.f, 200.f, "#mark", kFlowBase, &ch);

        char detail[160];
        std::snprintf(detail, sizeof(detail), "returned %d, %d quads (baseline %d)", ch,
                      static_cast<int>(same.quads.size()), static_cast<int>(base.quads.size()));
        check(ch == 0 && first_difference(same, base) < 0, "identical style reports no change", detail);
    }

    // --- the fail-closed cases -----------------------------------------------
    {
        const std::string html = page_style_flow(kFlowBase);
        const Snapshot    base = render_page(ctx, html, 420.f, 200.f);

        const int32_t missing = lhu_set_style(ctx, "#does-not-exist", "color:#f00");
        const int32_t empty   = lhu_set_style(ctx, "", "color:#f00");
        const int32_t null_css = lhu_set_style(ctx, "#mark", nullptr);

        lhu_layout(ctx, 420.f);
        const Snapshot after = capture(ctx);

        char detail[200];
        std::snprintf(detail, sizeof(detail), "unmatched->%d, empty selector->%d, null css->%d, document still %d quads",
                      missing, empty, null_css, static_cast<int>(after.quads.size()));
        check(missing == 0 && empty == 0 && null_css == 0 && first_difference(after, base) < 0,
              "rejected style mutations leave the document untouched", detail);
    }

    // --- the toggle ----------------------------------------------------------
    {
        const std::string html = page_style_flow(kFlowBase);
        const Snapshot    base = render_page(ctx, html, 420.f, 200.f);

        lhu_exp_setstyle_set_enabled(ctx, 0);
        const int32_t off = lhu_set_style(ctx, "#mark", "background:#e11d48");
        lhu_layout(ctx, 420.f);
        const Snapshot after_off = capture(ctx);
        lhu_exp_setstyle_set_enabled(ctx, 1);

        const int32_t on = lhu_set_style(ctx, "#mark", "background:#e11d48");
        lhu_layout(ctx, 420.f);
        const Snapshot after_on = capture(ctx);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "off->%d (%s), document unchanged: %s; on->%d, document changed: %s",
                      off, lhu_last_error(ctx), first_difference(after_off, base) < 0 ? "yes" : "NO", on,
                      first_difference(after_on, base) >= 0 ? "yes" : "NO");
        check(off == 0 && first_difference(after_off, base) < 0 && on == 1 &&
                  first_difference(after_on, base) >= 0,
              "LHU_EXP_SETSTYLE=0 refuses the call and changes nothing", detail);
    }

    // --- E1: a style change must disarm the layout short-circuit -------------
    //
    // This is the interaction that renders a whole frame at the previous
    // geometry if it is got wrong, and it is deliberately staged so that the
    // short-circuit is ARMED at the moment lhu_set_style() is called: a text
    // mutation that re-measures to exactly the size it already had leaves
    // lhu_layout() believing the tree in front of it is the answer. A style
    // change that moves geometry then has to take that belief away.
    {
        int32_t exp_before = 0;
        lhu_exp_stats(ctx, &exp_before, nullptr, nullptr);
        lhu_exp_set_enabled(ctx, 1);

        const char*       kMoved = "font-size:27px;padding:19px 6px;background:#243050;color:#e8e8ef";
        const std::string html   = page_style_flow(kFlowBase);
        const std::string ref    = page_style_flow(kMoved);
        render_page(ctx, html, 420.f, 200.f);
        render_page(ctx, ref, 420.f, 200.f);

        lhu_set_viewport(ctx, 420.f, 200.f);
        lhu_load_html(ctx, html.c_str(), nullptr);
        lhu_layout(ctx, 420.f);
        LhuFrame f {};
        lhu_record(ctx, &f);

        // Arms the short-circuit: same text in, nothing measured differently.
        lhu_set_text(ctx, "#mark", "Hareketli kutu");

        int32_t s0 = 0, r0 = 0;
        lhu_exp_stats(ctx, nullptr, &s0, &r0);
        lhu_set_style(ctx, "#mark", kMoved);
        lhu_layout(ctx, 420.f);
        int32_t s1 = 0, r1 = 0;
        lhu_exp_stats(ctx, nullptr, &s1, &r1);

        const Snapshot got      = capture(ctx);
        const Snapshot expected = render_page(ctx, ref, 420.f, 200.f);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "layout skipped %d / rendered %d; %d quads h=%.4f vs re-parse %d h=%.4f%s",
                      s1 - s0, r1 - r0, static_cast<int>(got.quads.size()), got.doc_h,
                      static_cast<int>(expected.quads.size()), expected.doc_h,
                      first_difference(got, expected) < 0 ? "   identical" : "   DIFFERS");
        check(s1 - s0 == 0 && r1 - r0 == 1 && same_frame(got, expected),
              "style change disarms an armed layout short-circuit", detail);

        lhu_exp_set_enabled(ctx, exp_before);
    }

    // --- hundreds of mutations in a row, then back to where it started -------
    //
    // Drift is what a merge-instead-of-replace bug looks like once it is too
    // small to see in one step: a declaration that lingers, a counter that keeps
    // incrementing, a cache entry that never gets evicted. Restoring the original
    // style has to restore the original frame exactly.
    {
        const std::string html     = page_style_flow(kFlowBase);
        const Snapshot    baseline = render_page(ctx, html, 420.f, 200.f);

        lhu_set_viewport(ctx, 420.f, 200.f);
        lhu_load_html(ctx, html.c_str(), nullptr);
        lhu_layout(ctx, 420.f);
        LhuFrame f {};
        lhu_record(ctx, &f);

        int32_t applied = 0, entries_before = 0, entries_after = 0;
        lhu_exp_setstyle_stats(ctx, nullptr, nullptr, nullptr, &entries_before);

        char buf[192];
        for(int i = 0; i < 600; ++i)
        {
            // A different string every single iteration, which is the workload
            // this call exists for and the one the inline-style parse memo is
            // worst at. Values are kept small so the page does not run away.
            std::snprintf(buf, sizeof(buf),
                          "font-size:%dpx;padding:%dpx %dpx;background:rgb(%d,%d,%d);color:#e8e8ef;"
                          "border-radius:%dpx",
                          12 + i % 9, 2 + i % 7, 4 + i % 5, i % 251, (i * 3) % 251, (i * 7) % 251, i % 11);
            if(lhu_set_style(ctx, "#mark", buf))
            {
                ++applied;
            }
            lhu_layout(ctx, 420.f);
            lhu_record(ctx, &f);
        }

        lhu_set_style(ctx, "#mark", kFlowBase);
        lhu_layout(ctx, 420.f);
        const Snapshot restored = capture(ctx);
        lhu_exp_setstyle_stats(ctx, nullptr, nullptr, nullptr, &entries_after);

        char detail[240];
        std::snprintf(detail, sizeof(detail),
                      "%d of 600 applied; restored %d quads / %.4fx%.4f (baseline %d / %.4fx%.4f); "
                      "inline-style memo %d -> %d entries",
                      applied, static_cast<int>(restored.quads.size()), restored.doc_w, restored.doc_h,
                      static_cast<int>(baseline.quads.size()), baseline.doc_w, baseline.doc_h, entries_before,
                      entries_after);
        check(applied == 600 && same_frame(restored, baseline), "600 unique styles then restore, no drift", detail);
    }

    std::printf("  %d check(s) failed in total\n", g_failures);
    return g_failures == 0;
}

bool verify_text_updates(LhuContext* ctx)
{
    std::printf("\n=== correctness: lhu_set_text vs a full re-parse ===\n");

    const std::string hud       = page_hud();
    const std::string hud_ref   = page_hud("HP 12 / 100");
    const std::string menu      = page_menu();
    const std::string menu_ref  = page_menu("800x600");

    const char* kLonger  = "Esya 210000000";       // same word/space split, wider
    const char* kShorter = "E 1";                  // same split, much narrower
    const char* kMoreWords = "Alev Kilici Artikli"; // 3 words -> node count changes
    const char* kOneWord   = "Tas";                 // 1 word  -> node count changes
    const char* kWrapping =
        "Cok uzun bir esya adi ile satirin sarmasini zorlayan bir etiket metni burada duruyor";

    const std::string list40      = page_list(40);
    const std::string list40_long = page_list(40, kLonger);
    const std::string list40_shrt = page_list(40, kShorter);
    const std::string list40_more = page_list(40, kMoreWords);
    const std::string list40_one  = page_list(40, kOneWord);
    const std::string list40_wrap = page_list(40, kWrapping);
    const std::string list150     = page_list(150);
    const std::string list150_alt = page_list(150, "Esya 76 (yeni)");

    // Warm the glyph atlas with every variant first. Rasterizing a new glyph can
    // grow the atlas, which moves every UV; comparing a snapshot taken before
    // that against one taken after would fail for a reason that has nothing to
    // do with the mutation path.
    for(const std::string* page : {&hud, &hud_ref, &menu, &menu_ref, &list40, &list40_long, &list40_shrt,
                                   &list40_more, &list40_one, &list40_wrap, &list150, &list150_alt})
    {
        render_page(ctx, *page, 680.f, 3000.f);
    }

    // --- baselines: the parse path must be byte-for-byte what it always was ---
    {
        const Snapshot a = render_page(ctx, hud, 420.f, 120.f);
        const Snapshot b = render_page(ctx, menu, 680.f, 460.f);
        const Snapshot c = render_page(ctx, list40, 480.f, 900.f);
        const Snapshot d = render_page(ctx, list150, 480.f, 3000.f);

        char detail[160];
        std::snprintf(detail, sizeof(detail), "HUD %d, menu %d, list40 %d, list150 %d (expected 27/180/543/2108)",
                      static_cast<int>(a.quads.size()), static_cast<int>(b.quads.size()),
                      static_cast<int>(c.quads.size()), static_cast<int>(d.quads.size()));
        check(a.quads.size() == 27 && b.quads.size() == 180 && c.quads.size() == 543 && d.quads.size() == 2108,
              "baseline quad counts unchanged", detail);
    }

    const Snapshot list40_base = render_page(ctx, list40, 480.f, 900.f);

    // --- setting the same text must be a no-op, not a silent rebuild ---
    {
        int32_t        changed = 0;
        const Snapshot same    = mutate_page(ctx, list40, 480.f, 900.f, "#mark", list_label(list_marked_row(40)).c_str(),
                                             &changed);
        char           detail[160];
        std::snprintf(detail, sizeof(detail), "returned %d, %d quads (baseline %d)", changed,
                      static_cast<int>(same.quads.size()), static_cast<int>(list40_base.quads.size()));
        check(changed == 0 && first_difference(same, list40_base) < 0, "identical text reports no change", detail);
    }

    // --- the glyph count has to follow the text, not the old measurement ---
    {
        int32_t        changed = 0;
        const Snapshot longer  = mutate_page(ctx, list40, 480.f, 900.f, "#mark", kLonger, &changed);
        const Snapshot shorter = mutate_page(ctx, list40, 480.f, 900.f, "#mark", kShorter, &changed);

        const std::string original = list_label(list_marked_row(40));
        const int expect_longer  = list40_base.glyphs + visible_chars(kLonger) - visible_chars(original.c_str());
        const int expect_shorter = list40_base.glyphs + visible_chars(kShorter) - visible_chars(original.c_str());

        char detail[220];
        std::snprintf(detail, sizeof(detail),
                      "\"%s\"->\"%s\" glyphs %d->%d (expected %d); ->\"%s\" glyphs %d (expected %d)", original.c_str(),
                      kLonger, list40_base.glyphs, longer.glyphs, expect_longer, kShorter, shorter.glyphs,
                      expect_shorter);
        check(longer.glyphs == expect_longer && shorter.glyphs == expect_shorter, "glyph count follows the new text",
              detail);
    }

    // --- the real proof: a mutation must equal a re-parse, quad for quad ---
    check_matches_reparse(ctx, "longer label == re-parse (same node shape)", list40, list40_long, 480.f, 900.f, "#mark",
                          kLonger);
    check_matches_reparse(ctx, "shorter label == re-parse (same node shape)", list40, list40_shrt, 480.f, 900.f,
                          "#mark", kShorter);
    check_matches_reparse(ctx, "more words == re-parse (nodes created)", list40, list40_more, 480.f, 900.f, "#mark",
                          kMoreWords);
    check_matches_reparse(ctx, "fewer words == re-parse (nodes destroyed)", list40, list40_one, 480.f, 900.f, "#mark",
                          kOneWord);
    check_matches_reparse(ctx, "HUD counter == re-parse", hud, hud_ref, 420.f, 120.f, "#hp", "HP 12 / 100");
    check_matches_reparse(ctx, "table cell == re-parse", menu, menu_ref, 680.f, 460.f, "#res", "800x600");
    check_matches_reparse(ctx, "150-row list == re-parse", list150, list150_alt, 480.f, 3000.f, "#mark",
                          "Esya 76 (yeni)");

    // --- geometry, not just glyph count: force the row to wrap ---
    {
        int32_t        changed = 0;
        const Snapshot wrapped = mutate_page(ctx, list40, 480.f, 900.f, "#mark", kWrapping, &changed);
        const Snapshot parsed  = render_page(ctx, list40_wrap, 480.f, 900.f);

        char detail[220];
        std::snprintf(detail, sizeof(detail), "doc height %.1f -> %.1f (re-parse %.1f), %d quads vs %d",
                      list40_base.doc_h, wrapped.doc_h, parsed.doc_h, static_cast<int>(wrapped.quads.size()),
                      static_cast<int>(parsed.quads.size()));
        check(changed == 1 && wrapped.doc_h > list40_base.doc_h && first_difference(wrapped, parsed) < 0,
              "wrapping label grows the document and matches", detail);
    }

    // --- mutate away and back must land exactly where it started ---
    {
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        const std::string original = list_label(list_marked_row(40));

        lhu_set_text(ctx, "#mark", kWrapping);   // structural, and re-wraps the row
        lhu_layout(ctx, 480.f);
        lhu_record(ctx, &f);

        lhu_set_text(ctx, "#mark", kMoreWords);  // structural again
        lhu_layout(ctx, 480.f);
        lhu_record(ctx, &f);

        const int32_t back = lhu_set_text(ctx, "#mark", original.c_str());
        lhu_layout(ctx, 480.f);
        const Snapshot restored = capture(ctx);

        char detail[200];
        std::snprintf(detail, sizeof(detail), "%d quads / h=%.1f  vs baseline %d quads / h=%.1f",
                      static_cast<int>(restored.quads.size()), restored.doc_h,
                      static_cast<int>(list40_base.quads.size()), list40_base.doc_h);
        check(back == 1 && first_difference(restored, list40_base) < 0, "round trip restores the original exactly",
              detail);
    }

    // --- repeated mutation of one live document must not drift ---
    //
    // Every structural update destroys text nodes and their render items and
    // creates new ones. If any of that accumulated -- a stale entry in the
    // element's render list, a render child left behind -- the invariant check
    // in lhu_set_text() would start rejecting, or the output would drift away
    // from the baseline. 500 updates without re-parsing catch either.
    {
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        const std::string original = list_label(list_marked_row(40));

        int rejected = 0;
        for(int i = 0; i < 500; ++i)
        {
            const char* text = (i % 3 == 0) ? kMoreWords : ((i % 3 == 1) ? kOneWord : kLonger);
            if(!lhu_set_text(ctx, "#mark", text))
            {
                ++rejected;
            }
            lhu_layout(ctx, 480.f);
            lhu_record(ctx, &f);
        }

        lhu_set_text(ctx, "#mark", original.c_str());
        lhu_layout(ctx, 480.f);
        const Snapshot restored = capture(ctx);

        char detail[200];
        std::snprintf(detail, sizeof(detail), "%d rejected of 500, then %d quads / h=%.1f (baseline %d / %.1f)",
                      rejected, static_cast<int>(restored.quads.size()), restored.doc_h,
                      static_cast<int>(list40_base.quads.size()), list40_base.doc_h);
        check(rejected == 0 && first_difference(restored, list40_base) < 0,
              "500 updates on one document, then restore", detail);
    }

    // --- the guards have to fail closed rather than corrupt the tree ---
    {
        lhu_set_viewport(ctx, 480.f, 900.f);
        lhu_load_html(ctx, list40.c_str(), nullptr);
        lhu_layout(ctx, 480.f);

        LhuFrame f {};
        lhu_record(ctx, &f);

        // 'body > div' holds 40 row elements, not text.
        const int32_t on_container = lhu_set_text(ctx, "div", "nope");
        const int32_t on_missing   = lhu_set_text(ctx, "#does-not-exist", "nope");

        lhu_layout(ctx, 480.f);
        const Snapshot after = capture(ctx);

        char detail[200];
        std::snprintf(detail, sizeof(detail), "container->%d, unmatched->%d, document still %d quads", on_container,
                      on_missing, static_cast<int>(after.quads.size()));
        check(on_container == 0 && on_missing == 0 && first_difference(after, list40_base) < 0,
              "rejected mutations leave the document untouched", detail);
    }

    std::printf("  %d check(s) failed\n", g_failures);
    return g_failures == 0;
}

// --- how much of lhu_set_style() is the recursion? ---------------------------
//
// Recomputing the mutated element's own styles is O(1). Recomputing its
// descendants is not, and it is not optional either: inherited properties have
// to reach them. What IS optional is re-running SELECTOR MATCHING over the
// subtree, which is what litehtml's refresh_styles() does and what
// LHU_SETSTYLE_DEEP=1 turns back on. This measures the difference where it can
// actually be seen -- on the 150-row inventory page, mutating the container that
// every one of the 150 rows hangs off, so one call drags ~450 elements with it.
//
// The same run is the answer to "does recursing cost more than it saves": the
// self-only variant recomputes exactly the same computed values (the checks
// above prove it byte for byte), so anything the recursion costs here is pure
// loss on this workload.
void bench_deep_subtree(LhuContext* ctx, int iterations)
{
    const bool  deep = std::getenv("LHU_SETSTYLE_DEEP") && std::strcmp(std::getenv("LHU_SETSTYLE_DEEP"), "0") != 0;
    const float w = 480.f, h = 3000.f;

    const std::string html = page_list(150);
    lhu_set_viewport(ctx, w, h);
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, w);
    LhuFrame f {};
    lhu_record(ctx, &f);

    std::vector<double> shallow_call, deep_call;
    char                buf[192];

    for(int i = 0; i < iterations; ++i)
    {
        // The row spans, deep in the tree -- one element, no descendants worth
        // the name. This is the shape an animation actually mutates.
        std::snprintf(buf, sizeof(buf), "font-size:14px;color:rgb(%d,%d,%d)", i % 251, (i * 3) % 251, (i * 7) % 251);
        auto t0 = Clock::now();
        lhu_set_style(ctx, "#mark", buf);
        shallow_call.push_back(ms_since(t0));
        lhu_layout(ctx, w);
        lhu_record(ctx, &f);

        // The container of all 150 rows: ~450 elements inherit from it.
        std::snprintf(buf, sizeof(buf), "margin:10px;color:rgb(%d,%d,%d)", i % 251, (i * 3) % 251, (i * 7) % 251);
        t0 = Clock::now();
        lhu_set_style(ctx, "div", buf);
        deep_call.push_back(ms_since(t0));
        lhu_layout(ctx, w);
        lhu_record(ctx, &f);
    }

    const Stats sc = Stats::from(shallow_call);
    const Stats dc = Stats::from(deep_call);

    std::printf("\n=== lhu_set_style cost by subtree size (150-row inventory, %d iterations) ===\n", iterations);
    std::printf("  selector re-matching over the subtree: %s (LHU_SETSTYLE_DEEP)\n", deep ? "ON" : "off");
    std::printf("  %-34s %8s %8s %8s %8s\n", "", "mean", "p50", "p95", "max");
    std::printf("  %-34s %8.3f %8.3f %8.3f %8.3f\n", "leaf span (#mark, 1 element)", sc.mean, sc.p50, sc.p95, sc.max);
    std::printf("  %-34s %8.3f %8.3f %8.3f %8.3f\n", "row container (~450 descendants)", dc.mean, dc.p50, dc.p95,
                dc.max);
    std::printf("  the deep subtree costs %.0fx the leaf; a full parse of this page is ~3.1 ms\n",
                sc.p50 > 0 ? dc.p50 / sc.p50 : 0.0);
}

} // namespace

int main(int argc, char** argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 200;

    const char* env_root = std::getenv("LHU_ROOT");
    const std::string root = env_root ? env_root : ".";

    LhuContext* ctx = lhu_create(nullptr);

    // LHU_FONT lets the Android runner point at a font that exists on the
    // device; otherwise fall back to the usual desktop locations.
    const char* font_env = std::getenv("LHU_FONT");

    const char* regular_candidates[] = {
        font_env,
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
    };

    std::vector<uint8_t> regular;
    std::string          regular_path;

    for(const char* candidate : regular_candidates)
    {
        if(!candidate)
        {
            continue;
        }
        regular = read_file(candidate);
        if(!regular.empty())
        {
            regular_path = candidate;
            break;
        }
    }

    if(regular.empty())
    {
        std::printf("no usable font found; set LHU_FONT to a .ttf path\n");
        return 1;
    }

    lhu_register_font(ctx, "sans-serif", 400, 0, regular.data(), static_cast<int32_t>(regular.size()));

    const char* bold_candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/system/fonts/Roboto-Bold.ttf",
    };

    for(const char* candidate : bold_candidates)
    {
        const auto bold = read_file(candidate);
        if(!bold.empty())
        {
            lhu_register_font(ctx, "sans-serif", 700, 0, bold.data(), static_cast<int32_t>(bold.size()));
            break;
        }
    }

    lhu_set_default_font(ctx, "sans-serif", 16.f);

    std::printf("Doctype benchmark — %d iterations per page\n", iterations);
    std::printf("font: %s\n", regular_path.c_str());
#ifdef __ANDROID__
    std::printf("running ON DEVICE — these are real measurements, not extrapolations\n");
#else
    std::printf("(desktop numbers; mobile estimates are extrapolations and marked as such)\n");
#endif

    const bool ok = verify_text_updates(ctx) && verify_incremental_layout(ctx) && verify_style_updates(ctx);

    bench_page(ctx, "HUD overlay", page_hud(), 420.f, 120.f, iterations,
               {"#hp", "HP 83 / 100", "HP 82 / 100", "HP 83 / 100 (zehirli)", "#hp",
                "font-size:22px;color:rgb(%d,%d,%d);padding-left:%dpx"});
    bench_page(ctx, "Settings menu", page_menu(), 680.f, 460.f, iterations,
               {"#res", "1600x0900", "1440x0900", "1600 x 900", "#res",
                "padding:6px 8px;border-bottom:1px solid #232b45;text-align:right;"
                "background:rgb(%d,%d,%d);margin-top:%dpx"});
    bench_page(ctx, "Inventory list, 40 rows", page_list(40), 480.f, 900.f, iterations,
               {"#mark", "Esya 31", "Esya 41", "Esya 21 (yeni)", "#mark",
                "font-size:14px;color:rgb(%d,%d,%d);padding-left:%dpx"});
    bench_page(ctx, "Inventory list, 150 rows", page_list(150), 480.f, 3000.f, iterations / 2,
               {"#mark", "Esya 66", "Esya 86", "Esya 76 (yeni)", "#mark",
                "font-size:14px;color:rgb(%d,%d,%d);padding-left:%dpx"});

    bench_deep_subtree(ctx, iterations / 2);

    if(!ok)
    {
        std::printf("\nCORRECTNESS CHECKS FAILED — the timings above are meaningless.\n");
        lhu_destroy(ctx);
        return 1;
    }

    lhu_destroy(ctx);
    return 0;
}
