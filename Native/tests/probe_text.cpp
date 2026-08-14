// EXPERIMENT D, STEP 1 — is text measurement actually where layout time goes?
//
// Wraps the real Container in a subclass that intercepts text_width(), so no
// production code has to be instrumented. Reports, per bench page:
//
//   * how many times text_width() is called during layout, and during draw,
//   * how many of those are duplicates (same string + same font handle),
//   * the string-length distribution (short strings are the case where a hash
//     could plausibly cost as much as the measurement it replaces),
//   * wall time spent inside text_width() as a share of layout wall time.
//
// Timing text_width from inside itself adds clock overhead to the very thing it
// measures, so the share is reported as an upper bound and the run is repeated
// without timing to show the observer effect.
//
// Not wired into build_macos.sh (same as probe_parse.cpp) so it does not slow
// the normal build. Build it after ./build_macos.sh with:
//
//   L=third_party/litehtml
//   clang++ -std=c++17 -O2 -DNDEBUG -arch arm64 -mmacosx-version-min=11.0 \
//     -I$L/include -I$L/include/litehtml -I$L/src -I$L/src/gumbo/include \
//     -I$L/src/gumbo/include/gumbo -Ithird_party -Isrc -Itests \
//     tests/probe_text.cpp build/macos/obj/*.o -o build/macos/bin/lhu_probe_text

#include "lhu_container.h"
#include "lhu_font.h"
#include "lhu_master_css.h"

#include <litehtml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
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

struct Counters
{
    long long calls        = 0;
    long long chars        = 0;
    double    seconds      = 0;
    std::map<size_t, long long>                          len_hist;
    std::unordered_map<std::string, long long>           distinct; // "<font>|<text>"
};

class ProbeContainer : public lhu::Container
{
  public:
    using lhu::Container::Container;

    bool     timing = true;
    Counters parse_c;
    Counters layout_c;
    Counters draw_c;
    Counters* active = &parse_c;

    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override
    {
        Counters& c = *active;
        ++c.calls;

        const size_t len = text ? std::strlen(text) : 0;
        c.chars += static_cast<long long>(len);
        ++c.len_hist[len];

        {
            char keybuf[32];
            std::snprintf(keybuf, sizeof(keybuf), "%llu|", static_cast<unsigned long long>(hFont));
            std::string key(keybuf);
            key.append(text ? text : "");
            ++c.distinct[key];
        }

        if(!timing)
        {
            return lhu::Container::text_width(text, hFont);
        }

        const auto t0 = Clock::now();
        const auto w  = lhu::Container::text_width(text, hFont);
        c.seconds += std::chrono::duration<double>(Clock::now() - t0).count();
        return w;
    }

    void reset()
    {
        parse_c  = Counters {};
        layout_c = Counters {};
        draw_c   = Counters {};
    }
};

// --- the same four pages the benchmark uses ---------------------------------

std::string page_hud()
{
    return "<body style='margin:0;font-family:sans-serif;color:#fff'>"
           "<div style='padding:8px'>"
           "<div style='font-size:22px'>HP 84 / 100</div>"
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
           "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>1920x1080</td></tr>"
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

    for(int i = 0; i < rows; ++i)
    {
        out += "<div style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span style='font-size:14px'>Esya " +
               std::to_string(i + 1) +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }

    out += "</div></body>";
    return out;
}

void report(const char* phase, const Counters& c)
{
    long long unique = static_cast<long long>(c.distinct.size());
    long long dupes  = c.calls - unique;

    std::printf("  %-7s calls %6lld   unique(str,font) %5lld   duplicate %6lld (%.1f%%)   chars/call %.1f\n", phase,
                c.calls, unique, dupes, c.calls ? 100.0 * static_cast<double>(dupes) / static_cast<double>(c.calls) : 0.0,
                c.calls ? static_cast<double>(c.chars) / static_cast<double>(c.calls) : 0.0);
}

void len_report(const Counters& c)
{
    long long le4 = 0, le8 = 0, le16 = 0, gt16 = 0;
    for(const auto& kv : c.len_hist)
    {
        if(kv.first <= 4)
            le4 += kv.second;
        else if(kv.first <= 8)
            le8 += kv.second;
        else if(kv.first <= 16)
            le16 += kv.second;
        else
            gt16 += kv.second;
    }
    std::printf("          string length: <=4 %lld, 5-8 %lld, 9-16 %lld, >16 %lld\n", le4, le8, le16, gt16);
}

void probe_page(lhu::FontManager& fonts, const char* name, const std::string& html, float width, float height,
                int iterations)
{
    LhuHostCallbacks host {};
    ProbeContainer   container(fonts, host);
    container.set_viewport(width, height);
    container.set_default_font("sans-serif", 16.f);

    const std::string master(lhu::trimmed_master_css());

    auto build = [&] {
        const litehtml::estring src(html, litehtml::encoding::utf_8);
        return litehtml::document::createFromString(src, &container, master, "");
    };

    // Warm the glyph atlas so first-touch rasterization is not counted.
    {
        auto doc = build();
        doc->render(litehtml::pixel_t(width));
        litehtml::position clip(litehtml::pixel_t(0.f), litehtml::pixel_t(0.f),
                                litehtml::pixel_t(static_cast<float>(doc->width())),
                                litehtml::pixel_t(static_cast<float>(doc->height())));
        container.begin_record();
        doc->draw(0, litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), &clip);
        container.end_record();
    }

    // --- pass 1: counts + in-call timing (has observer overhead) -------------
    container.reset();
    container.timing = true;

    std::vector<double> layout_ms, draw_ms, parse_ms;
    double              layout_tw_s = 0, draw_tw_s = 0, parse_tw_s = 0;

    litehtml::document::ptr doc;

    for(int i = 0; i < iterations; ++i)
    {
        container.active   = &container.parse_c;
        const double pw0   = container.parse_c.seconds;
        auto         tp0   = Clock::now();
        doc                = build();
        parse_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - tp0).count());
        parse_tw_s += container.parse_c.seconds - pw0;

        container.active = &container.layout_c;
        const double tw0 = container.layout_c.seconds;
        auto         t0  = Clock::now();
        doc->render(litehtml::pixel_t(width));
        layout_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        layout_tw_s += container.layout_c.seconds - tw0;

        litehtml::position clip(litehtml::pixel_t(0.f), litehtml::pixel_t(0.f),
                                litehtml::pixel_t(static_cast<float>(doc->width())),
                                litehtml::pixel_t(static_cast<float>(doc->height())));

        container.active = &container.draw_c;
        const double dw0 = container.draw_c.seconds;
        t0               = Clock::now();
        container.begin_record();
        doc->draw(0, litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), &clip);
        container.end_record();
        draw_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        draw_tw_s += container.draw_c.seconds - dw0;
    }

    const double parse_med  = median(parse_ms);
    const double layout_med = median(layout_ms);
    const double draw_med   = median(draw_ms);
    const double parse_tw_per_iter  = parse_tw_s * 1000.0 / iterations;
    const double layout_tw_per_iter = layout_tw_s * 1000.0 / iterations;
    const double draw_tw_per_iter   = draw_tw_s * 1000.0 / iterations;

    // Per-iteration counts.
    Counters pc = container.parse_c, lc = container.layout_c, dc = container.draw_c;
    for(Counters* c : {&pc, &lc, &dc})
    {
        c->calls /= iterations;
        c->chars /= iterations;
        for(auto& kv : c->len_hist)
            kv.second /= iterations;
    }

    std::printf("\n=== %s (%.0fx%.0f)  quads=%zu  doc %.2f x %.2f ===\n", name, width, height,
                container.quads().size(), static_cast<double>(doc->width()), static_cast<double>(doc->height()));
    report("parse", pc);
    len_report(pc);
    report("layout", lc);
    report("draw", dc);

    std::printf("  parse  %.3f ms (instrumented), of which text_width %.3f ms  -> %.0f%%\n", parse_med,
                parse_tw_per_iter, parse_med > 0 ? parse_tw_per_iter / parse_med * 100.0 : 0.0);
    std::printf("  layout %.3f ms (instrumented), of which text_width %.3f ms  -> %.0f%% UPPER BOUND\n", layout_med,
                layout_tw_per_iter, layout_med > 0 ? layout_tw_per_iter / layout_med * 100.0 : 0.0);
    std::printf("  draw   %.3f ms (instrumented), of which text_width %.3f ms  -> %.0f%%\n", draw_med,
                draw_tw_per_iter, draw_med > 0 ? draw_tw_per_iter / draw_med * 100.0 : 0.0);

    // --- kerning cache hit rate, per phase, on a warm cache -----------------
    {
        fonts.reset_kern_stats();
        auto d = build();
        const long long ph = fonts.kern_hits(), pm = fonts.kern_misses();

        fonts.reset_kern_stats();
        d->render(litehtml::pixel_t(width));
        const long long lh = fonts.kern_hits(), lm = fonts.kern_misses();

        fonts.reset_kern_stats();
        litehtml::position clip2(litehtml::pixel_t(0.f), litehtml::pixel_t(0.f),
                                 litehtml::pixel_t(static_cast<float>(d->width())),
                                 litehtml::pixel_t(static_cast<float>(d->height())));
        container.begin_record();
        d->draw(0, litehtml::pixel_t(0.f), litehtml::pixel_t(0.f), &clip2);
        container.end_record();
        const long long dh = fonts.kern_hits(), dm = fonts.kern_misses();

        auto rate = [](long long h, long long m) {
            const long long t = h + m;
            return t ? 100.0 * static_cast<double>(h) / static_cast<double>(t) : 0.0;
        };

        std::printf("  kern cache (warm): parse %lld lookups %.1f%% hit | layout %lld lookups %.1f%% hit "
                    "| record %lld lookups %.1f%% hit | %zu entries resident\n",
                    ph + pm, rate(ph, pm), lh + lm, rate(lh, lm), dh + dm, rate(dh, dm),
                    fonts.kern_cache_size());
    }

    // --- pass 2: no per-call clock, to size the observer effect --------------
    container.reset();
    container.timing = false;

    std::vector<double> layout2;
    for(int i = 0; i < iterations; ++i)
    {
        container.active = &container.parse_c;
        auto d = build();
        container.active = &container.layout_c;
        auto t0 = Clock::now();
        d->render(litehtml::pixel_t(width));
        layout2.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::printf("  layout %.3f ms with counting but no per-call clock (observer effect %.3f ms)\n", median(layout2),
                layout_med - median(layout2));
}

// Isolate raw cost: measuring N strings directly vs. hashing them.
void microbench(lhu::FontManager& fonts)
{
    lhu::Font* f14 = fonts.create_font("sans-serif", 14.f, 400, false, 0, 0, 0, 0.f);
    lhu::Font* f13 = fonts.create_font("sans-serif", 13.f, 400, false, 0, 0, 0, 0.f);

    const char* samples[] = {"x", "12", "Esya", "1920x1080", "Cozunurluk", "Performans", "Kuzey", "84"};
    const int   n         = 8;

    // Warm glyphs.
    for(int i = 0; i < n; ++i)
    {
        fonts.text_width(f14, samples[i]);
        fonts.text_width(f13, samples[i]);
    }

    const int reps = 200000;

    volatile float sink = 0.f;
    auto           t0   = Clock::now();
    for(int r = 0; r < reps; ++r)
    {
        sink = sink + fonts.text_width(f14, samples[r % n]);
    }
    const double measure_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / reps;

    volatile uint64_t hsink = 0;
    t0                      = Clock::now();
    for(int r = 0; r < reps; ++r)
    {
        const char* s = samples[r % n];
        uint64_t    h = 1469598103934665603ull;
        while(*s)
        {
            h ^= static_cast<unsigned char>(*s++);
            h *= 1099511628211ull;
        }
        hsink = hsink + h;
    }
    const double hash_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / reps;

    std::printf("\n=== micro: cost of one text_width vs. one FNV-1a hash of the same string ===\n");
    std::printf("  text_width (warm glyph cache): %.1f ns/call\n", measure_ns);
    std::printf("  FNV-1a hash only:              %.1f ns/call\n", hash_ns);
    std::printf("  hash is %.0f%% of the measurement it would replace\n", hash_ns / measure_ns * 100.0);

    // Per-character breakdown: where does text_width's time go?
    const char* one = "E";
    for(int i = 0; i < 1000; ++i)
        fonts.text_width(f14, one);

    t0 = Clock::now();
    for(int r = 0; r < reps; ++r)
        sink = sink + fonts.text_width(f14, one);
    const double one_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / reps;

    const char* ten = "Cozunurluk";
    t0              = Clock::now();
    for(int r = 0; r < reps; ++r)
        sink = sink + fonts.text_width(f14, ten);
    const double ten_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / reps;

    std::printf("  1-char string  %.1f ns   10-char string %.1f ns  -> ~%.1f ns fixed + %.1f ns/char\n", one_ns,
                ten_ns, one_ns - (ten_ns - one_ns) / 9.0, (ten_ns - one_ns) / 9.0);

    // Where does the per-character cost live? kern() calls
    // stbtt_GetCodepointKernAdvance, which re-resolves both code points to glyph
    // indices and re-walks the kern/GPOS table on every single call.
    {
        const char* pairs = "Cozunurluk";
        const int   np    = 9;

        volatile float ksink = 0.f;
        auto           k0    = Clock::now();
        for(int r = 0; r < reps; ++r)
        {
            for(int i = 0; i < np; ++i)
            {
                ksink = ksink + lhu::kern(f14, static_cast<uint32_t>(pairs[i]),
                                          static_cast<uint32_t>(pairs[i + 1]));
            }
        }
        const double kern_ns =
            std::chrono::duration<double, std::nano>(Clock::now() - k0).count() / (reps * np);

        volatile float gsink = 0.f;
        k0                   = Clock::now();
        for(int r = 0; r < reps; ++r)
        {
            for(int i = 0; i < np; ++i)
            {
                gsink = gsink + fonts.glyph(f14, static_cast<uint32_t>(pairs[i])).advance;
            }
        }
        const double glyph_ns =
            std::chrono::duration<double, std::nano>(Clock::now() - k0).count() / (reps * np);

        std::printf("  breakdown per character: kern() %.1f ns   glyph() cache hit %.1f ns\n", kern_ns, glyph_ns);
    }

    fonts.destroy_font(f14);
    fonts.destroy_font(f13);
}

} // namespace

int main(int argc, char** argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 100;

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

    std::printf("EXPERIMENT D step 1 — text_width profile (%d iterations/page)\n", iterations);

    probe_page(fonts, "HUD overlay", page_hud(), 420.f, 120.f, iterations);
    probe_page(fonts, "Settings menu", page_menu(), 680.f, 460.f, iterations);
    probe_page(fonts, "Inventory list, 40 rows", page_list(40), 480.f, 900.f, iterations);
    probe_page(fonts, "Inventory list, 150 rows", page_list(150), 480.f, 3000.f, iterations / 2);

    microbench(fonts);
    return 0;
}
