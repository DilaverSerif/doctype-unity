// EXPERIMENT E2 — what a *frame* costs, as opposed to what a page load costs.
//
// tests/bench.cpp re-parses the document on every iteration, so the record it
// times is always the first record of a brand new render tree: the one frame a
// retained display list cannot help with, and has to pay to fill. That number
// matters and is reported here too, but it is not the number a running game
// spends its time in.
//
// This measures the three frames a game UI actually alternates between, on a
// document that is parsed once and then lives:
//
//   idle     lhu_layout + lhu_record with nothing changed
//   tick     lhu_set_text (a counter, same word/space split) + layout + record
//   hover    lhu_mouse_move onto a row a :hover rule restyles + layout + record
//
// and reports how many quads came out of the cache rather than out of litehtml.
//
// REBASED ONTO E1. Two numbers are reported per case, not one:
//
//   record   lhu_record alone -- what E2 attacks
//   frame    lhu_layout + lhu_record -- what a game actually pays per frame
//
// They are needed separately because E1 and E2 remove different halves of the
// frame: E1 deletes document::render() when a mutation provably moved nothing,
// E2 deletes the draw traversal when nothing drawn has changed. Timing only the
// record (as this bench originally did) makes E1 invisible and overstates E2's
// share of a real frame; timing only the whole frame hides which half moved.
// One extra timestamp is taken before lhu_layout, i.e. outside and before the
// record region, so the record numbers stay comparable with the pre-rebase run.
//
// Deliberately a separate binary: adding passes to bench.cpp has already been
// shown on this project to move the timings of untouched code by 10-28%.
//
//   LHU_EXP_QUADCACHE=0 ./build/macos/bin/lhu_bench_frames
//   LHU_EXP_QUADCACHE=1 ./build/macos/bin/lhu_bench_frames
//
// Build after ./build_macos.sh with:
//
//   L=third_party/litehtml
//   clang++ -std=c++17 -O2 -DNDEBUG -arch arm64 -mmacosx-version-min=11.0 \
//     -I$L/include -I$L/include/litehtml -I$L/src -I$L/src/gumbo/include \
//     -I$L/src/gumbo/include/gumbo -Ithird_party -Isrc -Itests \
//     tests/bench_frames.cpp build/macos/obj/*.o -o build/macos/bin/lhu_bench_frames

#include "lhu_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

double p95(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[static_cast<size_t>(static_cast<double>(v.size()) * 0.95)];
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

std::string page_hud()
{
    return "<body style='margin:0;font-family:sans-serif;color:#fff'>"
           "<div style='padding:8px'>"
           "<div id='mark' style='font-size:22px'>HP 84 / 100</div>"
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
           "<td id='mark' style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>1920x1080</td></tr>"
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
        out += "<div class='row' style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
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

const char* kHoverCss = ".row:hover { background: #3a1020; border-color: #ff2255; color: #ffd0d8; }";

struct Page
{
    const char* name;
    std::string html;
    float       w, h;
    const char* tick_a;
    const char* tick_b;
};

void run(LhuContext* ctx, const Page& pg, int iterations)
{
    LhuFrame frame {};

    lhu_set_viewport(ctx, pg.w, pg.h);

    // --- cold: the first record of a freshly parsed document ---------------
    std::vector<double> cold;
    for(int i = 0; i < iterations; ++i)
    {
        lhu_load_html(ctx, pg.html.c_str(), kHoverCss);
        lhu_layout(ctx, pg.w);
        const auto t0 = Clock::now();
        lhu_record(ctx, &frame);
        cold.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }

    const int quads = frame.quad_count;

    // From here on the document is parsed once and lives, which is the whole
    // point.
    lhu_load_html(ctx, pg.html.c_str(), kHoverCss);
    lhu_layout(ctx, pg.w);
    lhu_record(ctx, &frame);
    lhu_layout(ctx, pg.w);
    lhu_record(ctx, &frame);

    lhu_quadcache_stat(ctx, LHU_QC_RESET);

    // --- idle: nothing changed ---------------------------------------------
    std::vector<double> idle, idle_frame;
    for(int i = 0; i < iterations; ++i)
    {
        const auto tf = Clock::now();
        lhu_layout(ctx, pg.w);
        const auto t0 = Clock::now();
        lhu_record(ctx, &frame);
        const auto t1 = Clock::now();
        idle.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        idle_frame.push_back(std::chrono::duration<double, std::milli>(t1 - tf).count());
    }
    const long long idle_replayed = lhu_quadcache_stat(ctx, LHU_QC_QUADS_REPLAYED);
    const long long idle_emitted  = lhu_quadcache_stat(ctx, LHU_QC_QUADS_EMITTED);
    lhu_quadcache_stat(ctx, LHU_QC_RESET);

    // --- tick: one counter changes, same word/space split -------------------
    std::vector<double> tick, tick_frame;
    for(int i = 0; i < iterations; ++i)
    {
        const auto tf = Clock::now();
        lhu_set_text(ctx, "#mark", (i % 2) ? pg.tick_a : pg.tick_b);
        lhu_layout(ctx, pg.w);
        const auto t0 = Clock::now();
        lhu_record(ctx, &frame);
        const auto t1 = Clock::now();
        tick.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        tick_frame.push_back(std::chrono::duration<double, std::milli>(t1 - tf).count());
    }
    const long long tick_replayed = lhu_quadcache_stat(ctx, LHU_QC_QUADS_REPLAYED);
    const long long tick_emitted  = lhu_quadcache_stat(ctx, LHU_QC_QUADS_EMITTED);
    lhu_quadcache_stat(ctx, LHU_QC_RESET);

    // --- hover: the pointer walks down the page -----------------------------
    std::vector<double> hover, hover_frame;
    for(int i = 0; i < iterations; ++i)
    {
        const auto tf = Clock::now();
        lhu_mouse_move(ctx, 60.f, 30.f + static_cast<float>((i * 41) % 400));
        lhu_layout(ctx, pg.w);
        const auto t0 = Clock::now();
        lhu_record(ctx, &frame);
        const auto t1 = Clock::now();
        hover.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        hover_frame.push_back(std::chrono::duration<double, std::milli>(t1 - tf).count());
    }
    const long long hover_replayed = lhu_quadcache_stat(ctx, LHU_QC_QUADS_REPLAYED);
    const long long hover_emitted  = lhu_quadcache_stat(ctx, LHU_QC_QUADS_EMITTED);

    auto pct = [](long long a, long long b) {
        const long long t = a + b;
        return t ? 100.0 * static_cast<double>(a) / static_cast<double>(t) : 0.0;
    };

    std::printf("\n=== %s (%.0fx%.0f)  %d quads  doc %.4f x %.4f ===\n", pg.name, pg.w, pg.h, quads, frame.doc_width,
                frame.doc_height);
    std::printf("  %-34s %9s %9s %9s %12s\n", "", "record p50", "record p95", "frame p50", "from cache");
    std::printf("  %-34s %9.4f %9.4f %9s %11s\n", "cold (first record after parse)", median(cold), p95(cold), "-", "-");
    std::printf("  %-34s %9.4f %9.4f %9.4f %10.1f%%\n", "idle (nothing changed)", median(idle), p95(idle),
                median(idle_frame), pct(idle_replayed, idle_emitted));
    std::printf("  %-34s %9.4f %9.4f %9.4f %10.1f%%\n", "tick (one counter changed)", median(tick), p95(tick),
                median(tick_frame), pct(tick_replayed, tick_emitted));
    std::printf("  %-34s %9.4f %9.4f %9.4f %10.1f%%\n", "hover (pointer moves rows)", median(hover), p95(hover),
                median(hover_frame), pct(hover_replayed, hover_emitted));
    std::printf("  cache holds %.0f KB (retained snapshot + entry table)\n",
                static_cast<double>(lhu_quadcache_stat(ctx, LHU_QC_BYTES)) / 1024.0);
}

} // namespace

int main(int argc, char** argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 400;

    LhuContext* ctx = lhu_create(nullptr);

    const char* font_env = std::getenv("LHU_FONT");
    const char* candidates[] = {font_env, "/System/Library/Fonts/Supplemental/Arial.ttf",
                                "/system/fonts/Roboto-Regular.ttf"};
    std::vector<uint8_t> regular;
    for(const char* c : candidates)
    {
        if(c)
        {
            regular = read_file(c);
        }
        if(!regular.empty())
        {
            break;
        }
    }
    if(regular.empty())
    {
        std::printf("no usable font; set LHU_FONT\n");
        return 1;
    }
    lhu_register_font(ctx, "sans-serif", 400, 0, regular.data(), static_cast<int32_t>(regular.size()));

    const auto bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
    if(!bold.empty())
    {
        lhu_register_font(ctx, "sans-serif", 700, 0, bold.data(), static_cast<int32_t>(bold.size()));
    }
    lhu_set_default_font(ctx, "sans-serif", 16.f);

    std::printf("EXPERIMENT E2 — steady-state frame cost, LHU_EXP_QUADCACHE=%s (%d frames per case)\n",
                lhu_quadcache_stat(ctx, LHU_QC_ENABLED) ? "on" : "off", iterations);

    const Page pages[] = {
        {"HUD overlay", page_hud(), 420.f, 120.f, "HP 83 / 100", "HP 82 / 100"},
        {"Settings menu", page_menu(), 680.f, 460.f, "1600x0900", "1440x0900"},
        {"Inventory list, 40 rows", page_list(40), 480.f, 900.f, "Esya 31", "Esya 41"},
        {"Inventory list, 150 rows", page_list(150), 480.f, 3000.f, "Esya 66", "Esya 86"},
    };

    for(const Page& pg : pages)
    {
        run(ctx, pg, iterations);
    }

    lhu_destroy(ctx);
    return 0;
}
