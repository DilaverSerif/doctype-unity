// EXPERIMENT E2, STEP 1 — where does lhu_record()'s time actually go, and what
// is the ceiling for a retained display list?
//
// Splits the recording cost into the two halves a quad cache treats differently:
//
//   * the *traversal*: litehtml's three-pass walk over the render tree
//     (draw_block / draw_floats / draw_inlines), the virtual draw() dispatch,
//     the clip push/pop. A per-node cache still pays all of this.
//   * the *emission*: turning a draw_* callback into LhuQuads — the memset in
//     push_quad, effective_clip, and above all walk_text's per-glyph decode +
//     glyph-cache + kern-cache lookups.
//
// A cache that replaces emission with a memcpy can only ever win the second
// half, so measuring the split first tells us the ceiling before a line of
// cache code is written.
//
// Modes measured per page:
//   full       normal record (traversal + emission)
//   no-text    traversal + everything except draw_text's glyph walk
//   walk-only  traversal, every draw_* is a no-op
//   memcpy     the whole previous frame's quad buffer copied verbatim
//   keep       what a whole-document cache hit would cost: nothing at all
//
// Not wired into build_macos.sh (same as probe_text.cpp). Build after
// ./build_macos.sh with:
//
//   L=third_party/litehtml
//   clang++ -std=c++17 -O2 -DNDEBUG -arch arm64 -mmacosx-version-min=11.0 \
//     -I$L/include -I$L/include/litehtml -I$L/src -I$L/src/gumbo/include \
//     -I$L/src/gumbo/include/gumbo -Ithird_party -Isrc -Itests \
//     tests/probe_record.cpp build/macos/obj/*.o -o build/macos/bin/lhu_probe_record

#include "lhu_container.h"
#include "lhu_font.h"
#include "lhu_master_css.h"

#include <litehtml.h>
#include <litehtml/render_item.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

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

enum class Mode
{
    Full,      // real emission
    NoText,    // draw_text suppressed, everything else emitted
    WalkOnly,  // every draw_* suppressed
    TextMemcpy // draw_text replaced by appending its cached quad run
};

// Wraps the real Container so no production code has to be instrumented.
class ProbeContainer : public lhu::Container
{
  public:
    using lhu::Container::Container;

    Mode mode = Mode::Full;

    // Per draw_text call: how many quads the real emission produced. Filled in
    // by a Full pass, replayed by the TextMemcpy pass.
    std::vector<int>     text_run_len;
    size_t               text_call = 0;
    bool                 recording_lengths = false;

    // Where TextMemcpy appends to; stands in for Container::m_quads, which is
    // private. Same memory traffic, same allocator behaviour (reserved once).
    std::vector<LhuQuad> spliced;
    std::vector<LhuQuad> source;

    long long text_calls_total = 0;
    long long text_quads_total = 0;

    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont, litehtml::web_color color,
                   const litehtml::position& pos) override
    {
        switch(mode)
        {
        case Mode::Full:
        {
            const size_t before = quads().size();
            lhu::Container::draw_text(hdc, text, hFont, color, pos);
            if(recording_lengths)
            {
                text_run_len.push_back(static_cast<int>(quads().size() - before));
            }
            ++text_calls_total;
            text_quads_total += static_cast<long long>(quads().size() - before);
            break;
        }
        case Mode::NoText:
        case Mode::WalkOnly:
            break;
        case Mode::TextMemcpy:
        {
            const int n = text_call < text_run_len.size() ? text_run_len[text_call] : 0;
            ++text_call;
            if(n > 0 && spliced.size() + static_cast<size_t>(n) <= source.size())
            {
                spliced.insert(spliced.end(), source.begin() + static_cast<long>(spliced.size()),
                               source.begin() + static_cast<long>(spliced.size()) + n);
            }
            break;
        }
        }
    }

    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                         const litehtml::web_color& color) override
    {
        if(mode == Mode::WalkOnly)
            return;
        lhu::Container::draw_solid_fill(hdc, layer, color);
    }

    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos,
                      bool root) override
    {
        if(mode == Mode::WalkOnly)
            return;
        lhu::Container::draw_borders(hdc, borders, draw_pos, root);
    }

    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override
    {
        if(mode == Mode::WalkOnly)
            return;
        lhu::Container::draw_list_marker(hdc, marker);
    }

    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::linear_gradient& g) override
    {
        if(mode == Mode::WalkOnly)
            return;
        lhu::Container::draw_linear_gradient(hdc, layer, g);
    }

    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::radial_gradient& g) override
    {
        if(mode == Mode::WalkOnly)
            return;
        lhu::Container::draw_radial_gradient(hdc, layer, g);
    }

    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                             const litehtml::background_layer::conic_gradient& g) override
    {
        if(mode == Mode::WalkOnly)
            return;
        lhu::Container::draw_conic_gradient(hdc, layer, g);
    }
};

// --- the same four pages the benchmark uses ---------------------------------

std::string page_hud()
{
    return "<body style='margin:0;font-family:sans-serif;color:#fff'>"
           "<div style='padding:8px'>"
           "<div id='hp' style='font-size:22px'>HP 84 / 100</div>"
           "<div style='height:8px;border-radius:4px;background:#333'>"
           "<i style='display:block;width:84%;height:8px;border-radius:4px;background:#e11'></i></div>"
           "<div style='margin-top:6px;font-size:13px;color:#aaa'>Bolge: Kuzey Gecidi</div>"
           "</div></body>";
}

std::string page_menu()
{
    return "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
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
           "<td id='res' style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>1920x1080</td></tr>"
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

std::string page_list(int rows)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div style='margin:10px'>";

    const int marked = rows / 2;
    for(int i = 0; i < rows; ++i)
    {
        out += "<div style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span" +
               std::string(i == marked ? " id='mark'" : "") + " style='font-size:14px'>Esya " +
               std::to_string(i + 1) +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }

    out += "</div></body>";
    return out;
}

void probe_page(lhu::FontManager& fonts, const char* name, const std::string& html, float width, float height,
                int iterations)
{
    LhuHostCallbacks host {};
    ProbeContainer   container(fonts, host);
    container.set_viewport(width, height);
    container.set_default_font("sans-serif", 16.f);

    const std::string master(lhu::trimmed_master_css());

    const litehtml::estring src(html, litehtml::encoding::utf_8);
    auto doc = litehtml::document::createFromString(src, &container, master, "");
    doc->render(litehtml::pixel_t(width));

    litehtml::position clip(litehtml::pixel_t(0.f), litehtml::pixel_t(0.f),
                            litehtml::pixel_t(static_cast<float>(doc->width())),
                            litehtml::pixel_t(static_cast<float>(doc->height())));

    auto record = [&](Mode m) {
        container.mode      = m;
        container.text_call = 0;
        container.spliced.clear();
        container.begin_record();
        doc->draw(0, litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), &clip);
        container.end_record();
    };

    // Warm the atlas, then capture the reference frame and the per-draw_text run
    // lengths that TextMemcpy replays.
    record(Mode::Full);
    record(Mode::Full);
    container.recording_lengths = true;
    container.text_run_len.clear();
    record(Mode::Full);
    container.recording_lengths = false;

    const size_t quad_count = container.quads().size();
    container.source        = container.quads();
    container.spliced.reserve(quad_count + 16);

    std::vector<LhuQuad> dst(quad_count);

    // --- how expensive is the dirty-detection walk a subtree cache needs? ----
    //
    // A subtree cache that skips litehtml's traversal has to learn, after every
    // lhu_layout(), which subtrees moved or resized. The cheapest sound way is a
    // single flat pass over the render tree comparing each node's geometry with
    // the previous frame's. This measures exactly that pass -- if it is not
    // dramatically cheaper than the draw traversal it replaces, the whole idea
    // is dead.
    struct Geom
    {
        float x, y, w, h;
        float pl, pt, pr, pb;
        float bl, bt, br, bb;
        float sl, st;
    };

    std::vector<Geom> geom_a, geom_b;
    std::function<void(const std::shared_ptr<litehtml::render_item>&, std::vector<Geom>&)> collect =
        [&](const std::shared_ptr<litehtml::render_item>& ri, std::vector<Geom>& out) {
            const litehtml::position& p  = ri->pos();
            const litehtml::margins&  pd = ri->get_paddings();
            const litehtml::margins&  bd = ri->get_borders();
            out.push_back(Geom {(float)p.x, (float)p.y, (float)p.width, (float)p.height, (float)pd.left, (float)pd.top,
                                (float)pd.right, (float)pd.bottom, (float)bd.left, (float)bd.top, (float)bd.right,
                                (float)bd.bottom, (float)ri->get_scroll_left(), (float)ri->get_scroll_top()});
            for(const auto& c : ri->children())
            {
                collect(c, out);
            }
        };

    collect(doc->root_render(), geom_a);
    const size_t node_count = geom_a.size();
    geom_b.reserve(node_count);

    std::vector<double> geomwalk;
    for(int i = 0; i < iterations; ++i)
    {
        auto t0 = Clock::now();
        geom_b.clear();
        collect(doc->root_render(), geom_b);
        const bool same = geom_b.size() == geom_a.size() &&
                          std::memcmp(geom_a.data(), geom_b.data(), geom_a.size() * sizeof(Geom)) == 0;
        geomwalk.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        if(!same)
        {
            std::printf("  (geometry walk saw a change -- unexpected on a static document)\n");
        }
    }
    const double gw = median(geomwalk);

    std::vector<double> full, notext, walkonly, memcpy_ms, textmemcpy;
    for(int i = 0; i < iterations; ++i)
    {
        auto t0 = Clock::now();
        record(Mode::Full);
        full.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

        t0 = Clock::now();
        record(Mode::NoText);
        notext.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

        t0 = Clock::now();
        record(Mode::WalkOnly);
        walkonly.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

        t0 = Clock::now();
        record(Mode::TextMemcpy);
        textmemcpy.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

        t0 = Clock::now();
        std::memcpy(dst.data(), container.source.data(), quad_count * sizeof(LhuQuad));
        memcpy_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }

    const double f  = median(full);
    const double nt = median(notext);
    const double wo = median(walkonly);
    const double mc = median(memcpy_ms);
    const double tm = median(textmemcpy);

    const long long tcalls = container.text_calls_total;
    const long long tquads = container.text_quads_total;

    std::printf("\n=== %s (%.0fx%.0f)  quads=%zu  doc %.4f x %.4f ===\n", name, width, height, quad_count,
                static_cast<double>(doc->width()), static_cast<double>(doc->height()));
    std::printf("  draw_text calls/frame ~%lld, glyph quads/frame ~%lld\n",
                tcalls ? tcalls / (long long)(iterations + 3) : 0, tquads ? tquads / (long long)(iterations + 3) : 0);
    std::printf("  %-34s %8s %8s\n", "", "ms", "% of full");
    std::printf("  %-34s %8.4f %8.1f\n", "full record", f, 100.0);
    std::printf("  %-34s %8.4f %8.1f\n", "traversal + non-text emission", nt, nt / f * 100.0);
    std::printf("  %-34s %8.4f %8.1f\n", "traversal only (no emission)", wo, wo / f * 100.0);
    std::printf("  %-34s %8.4f %8.1f\n", "traversal + text runs memcpy'd", tm, tm / f * 100.0);
    std::printf("  %-34s %8.4f %8.1f\n", "memcpy whole frame buffer", mc, mc / f * 100.0);
    std::printf("  -> emission is %.1f%% of record; text emission alone %.1f%%; traversal floor %.1f%%\n",
                (f - wo) / f * 100.0, (f - nt) / f * 100.0, wo / f * 100.0);
    std::printf("  -> best case for a per-node cache that keeps the traversal: %.4f ms (%.2fx)\n", tm, f / tm);
    std::printf("  -> best case for a whole-document cache (return last buffer): 0.0000 ms\n");
    std::printf("  render-tree nodes %zu; post-layout geometry diff walk %.4f ms (%.1f%% of record,"
                " %.1f%% of the traversal it replaces)\n",
                node_count, gw, gw / f * 100.0, gw / wo * 100.0);
}

// How expensive is the bookkeeping itself, per node?
void micro(size_t quads)
{
    const int reps = 20000;

    // (a) emitting one quad the way push_quad does: grow the vector, memset 144
    //     bytes, set three fields.
    std::vector<LhuQuad> v;
    v.reserve(quads + 16);
    auto t0 = Clock::now();
    for(int r = 0; r < reps; ++r)
    {
        v.clear();
        for(size_t i = 0; i < quads; ++i)
        {
            v.emplace_back();
            LhuQuad& q = v.back();
            std::memset(&q, 0, sizeof(q));
            q.type     = 2;
            q.grad_row = -1;
            q.clip_w   = -1.f;
        }
    }
    const double emit_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / (reps * (double)quads);

    // (b) appending the same quads from a cached run.
    std::vector<LhuQuad> srcv(quads);
    std::vector<LhuQuad> dstv;
    dstv.reserve(quads + 16);
    t0 = Clock::now();
    for(int r = 0; r < reps; ++r)
    {
        dstv.clear();
        dstv.insert(dstv.end(), srcv.begin(), srcv.end());
    }
    const double copy_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / (reps * (double)quads);

    std::printf("\n=== micro: emit one quad vs copy one cached quad (%zu-quad runs) ===\n", quads);
    std::printf("  push_quad-shaped emission  %6.2f ns/quad\n", emit_ns);
    std::printf("  cached-run copy            %6.2f ns/quad\n", copy_ns);
    std::printf("  copy is %.0f%% of an emission -- and that is *before* the glyph walk,\n"
                "  which the copy also skips.\n",
                copy_ns / emit_ns * 100.0);
}

} // namespace

int main(int argc, char** argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 200;

    lhu::FontManager fonts;

    const auto regular = read_file("/System/Library/Fonts/Supplemental/Arial.ttf");
    if(regular.empty())
    {
        std::printf("no system font\n");
        return 1;
    }
    fonts.register_font("sans-serif", 400, false, regular.data(), regular.size());

    const auto bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
    if(!bold.empty())
    {
        fonts.register_font("sans-serif", 700, false, bold.data(), bold.size());
    }
    fonts.set_default_family("sans-serif");

    std::printf("EXPERIMENT E2 step 1 — record cost split (%d iterations/page)\n", iterations);

    probe_page(fonts, "HUD overlay", page_hud(), 420.f, 120.f, iterations);
    probe_page(fonts, "Settings menu", page_menu(), 680.f, 460.f, iterations);
    probe_page(fonts, "Inventory list, 40 rows", page_list(40), 480.f, 900.f, iterations);
    probe_page(fonts, "Inventory list, 150 rows", page_list(150), 480.f, 3000.f, iterations / 2);

    micro(8);
    micro(2108);
    return 0;
}
