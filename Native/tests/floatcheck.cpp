// LHU experiment E5 -- float placement correctness + scan-count harness.
//
// Not built by build_macos.sh. Build it against the objects that script leaves
// behind:
//
//   ./build_macos.sh
//   clang++ -std=c++17 -O2 -DNDEBUG -arch arm64 -mmacosx-version-min=11.0 \
//     -Ithird_party/litehtml/include -Ithird_party/litehtml/include/litehtml \
//     -Ithird_party/litehtml/src -Ithird_party/litehtml/src/gumbo/include \
//     -Ithird_party/litehtml/src/gumbo/include/gumbo -Ithird_party -Isrc -Itests \
//     tests/floatcheck.cpp build/macos/obj/lh_*.o build/macos/obj/gumbo_*.o \
//     build/macos/obj/lhu_*.o -o build/macos/bin/lhu_floatcheck
//
// Two jobs:
//   * `dump` writes every recorded quad byte for a list of float-stress pages,
//     so the same binary run with LHU_EXP_FLOATS=1 and =0 can be diffed.
//   * `stats` reports the float entries each query loop scanned. It needs the
//     whole tree rebuilt with -DLHU_FLOAT_STATS.

#include "lhu_api.h"
#include "litehtml/formatting_context.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if(!f) return {};
    const std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    return out;
}

// --- the four benchmark pages, verbatim from tests/bench.cpp ----------------

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

std::string page_list(int rows, const char* mark_label = nullptr)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div style='margin:10px'>";
    const int marked = rows / 2;
    for(int i = 0; i < rows; ++i)
    {
        const bool is_marked = i == marked;
        const std::string label = (is_marked && mark_label) ? std::string(mark_label)
                                                            : ("Esya " + std::to_string(i + 1));
        out += "<div style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span" + std::string(is_marked ? " id='mark'" : "") +
               " style='font-size:14px'>" + label +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }
    out += "</div></body>";
    return out;
}

// --- float stress pages ------------------------------------------------------

const char* kHead = "<body style='margin:0;font-family:sans-serif;color:#fff;font-size:14px'>";

// left + right float on the same line, several lines deep, widths chosen so the
// two lists both hold many distinct inner edges.
std::string s_both_sides(int rows)
{
    std::string out = kHead;
    out += "<div style='margin:4px'>";
    for(int i = 0; i < rows; ++i)
    {
        out += "<div style='padding:3px;border:1px solid #345'>"
               "<span style='float:left;background:#a11'>L" + std::string((size_t)(i % 5) + 1, 'x') + "</span>"
               "<span style='float:right;background:#1a1'>R" + std::string((size_t)(i % 3) + 1, 'y') + "</span>"
               "orta metin " + std::to_string(i) + "</div>";
    }
    return out + "</div></body>";
}

// floats wider than their container.
std::string s_overwide()
{
    return std::string(kHead) +
           "<div style='width:60px;border:1px solid #555'>"
           "<span style='float:left;width:200px;height:20px;background:#a11'>WIDE</span>"
           "<span style='float:right;width:180px;height:14px;background:#1a1'>WIDER</span>"
           "kucuk kutu icinde tasan floatlar"
           "</div>"
           "<div style='width:40px'>"
           "<span style='float:left;width:300px;height:12px;background:#22a'></span>x</div>"
           "</body>";
}

// a float that overflows its block's bottom, then more content underneath.
std::string s_overflow_bottom()
{
    return std::string(kHead) +
           "<div style='border:1px solid #555'>"
           "<span style='float:left;width:40px;height:220px;background:#a11'></span>kisa</div>"
           "<div>sonraki blok birinci</div>"
           "<div>sonraki blok ikinci</div>"
           "<div>sonraki blok ucuncu</div>"
           "<div>sonraki blok dorduncu</div>"
           "</body>";
}

// clear:left / clear:right / clear:both in every combination.
std::string s_clears()
{
    std::string out = kHead;
    out += "<div style='width:300px'>";
    for(int i = 0; i < 12; ++i)
    {
        const char* clr = (i % 4 == 0) ? "left" : (i % 4 == 1) ? "right" : (i % 4 == 2) ? "both" : "none";
        out += "<span style='float:left;width:" + std::to_string(30 + (i % 3) * 17) +
               "px;height:" + std::to_string(18 + (i % 5) * 6) + "px;background:#a11;clear:" + clr + "'></span>";
        out += "<span style='float:right;width:" + std::to_string(25 + (i % 4) * 13) +
               "px;height:" + std::to_string(15 + (i % 3) * 9) + "px;background:#1a1;clear:" + clr + "'></span>";
        out += "<p style='margin:0;clear:" + std::string(clr) + "'>satir " + std::to_string(i) + "</p>";
    }
    return out + "</div></body>";
}

// nested block formatting contexts, each with its own floats.
std::string s_nested_bfc()
{
    std::string out = kHead;
    out += "<div style='overflow:hidden;border:1px solid #555'>"
           "<span style='float:left;width:50px;height:40px;background:#a11'></span>dis bfc metni"
           "<div style='overflow:hidden;border:1px solid #777;margin-left:60px'>"
           "<span style='float:right;width:40px;height:30px;background:#1a1'></span>"
           "<span style='float:left;width:20px;height:25px;background:#22a'></span>"
           "ic bfc metni burada biraz daha uzun olsun ki sarilsin"
           "<div style='overflow:auto'>"
           "<span style='float:left;width:15px;height:15px;background:#aa1'></span>"
           "<span style='float:left;width:35px;height:22px;background:#a1a'></span>en icteki bfc"
           "</div>"
           "</div>"
           "<span style='float:right;width:30px;height:60px;background:#1aa'></span>"
           "dis bfc devam eden metin"
           "</div>"
           "<div>bfc sonrasi</div>";
    return out + "</body>";
}

// floats interleaved with position:absolute (absolutes must not enter the lists).
std::string s_absolute()
{
    return std::string(kHead) +
           "<div style='position:relative;width:280px;border:1px solid #555'>"
           "<span style='float:left;width:40px;height:30px;background:#a11'></span>"
           "<span style='position:absolute;left:10px;top:5px;width:60px;height:20px;background:#333'>abs</span>"
           "<span style='float:right;width:35px;height:26px;background:#1a1'></span>"
           "<span style='position:absolute;right:0;bottom:0;width:50px;height:18px;background:#444'>abs2</span>"
           "<span style='float:left;width:22px;height:44px;background:#22a'></span>"
           "metin akisi burada devam ediyor ve floatlarin etrafina sariliyor olmali"
           "<span style='position:absolute;left:50%;top:50%;background:#555'>abs3</span>"
           "<span style='float:right;width:18px;height:12px;background:#aa1'></span>"
           "ikinci paragraf metni"
           "</div></body>";
}

// a float inside a table cell.
std::string s_table_cell()
{
    return std::string(kHead) +
           "<table style='width:340px;border-collapse:collapse'>"
           "<tr><td style='border:1px solid #555'>"
           "<span style='float:right;width:30px;height:20px;background:#1a1'></span>hucre bir metni</td>"
           "<td style='border:1px solid #555'>"
           "<span style='float:left;width:26px;height:34px;background:#a11'></span>hucre iki metni biraz uzun</td></tr>"
           "<tr><td style='border:1px solid #555'>"
           "<span style='float:left;width:44px;height:16px;background:#22a'></span>"
           "<span style='float:right;width:20px;height:16px;background:#aa1'></span>hucre uc</td>"
           "<td style='border:1px solid #555'>hucre dort</td></tr>"
           "</table>"
           "<div><span style='float:left;width:30px;height:20px;background:#a1a'></span>tablo sonrasi</div>"
           "</body>";
}

// text wrapping tightly around a float: narrow container, long text.
std::string s_tight_wrap()
{
    return std::string(kHead) +
           "<div style='width:150px;border:1px solid #555'>"
           "<span style='float:left;width:70px;height:70px;background:#a11'></span>"
           "cok uzun bir metin akisi burada satirlarin float kutusunun etrafina "
           "sikica sarilmasini zorluyor ve her satir icin ayri bir sorgu yapiliyor "
           "boylece line left ve line right yollari da denenmis oluyor"
           "</div>"
           "<div style='width:150px;border:1px solid #555'>"
           "<span style='float:right;width:65px;height:90px;background:#1a1'></span>"
           "sag tarafta duran bir float ile ayni sekilde uzun bir metin akisi ve "
           "cok sayida satir kirilimi olusuyor burada"
           "</div></body>";
}

// float bottoms landing exactly on a line top: line-height and float heights are
// chosen so the edges coincide.
std::string s_edge_touch()
{
    std::string out = kHead;
    out += "<div style='width:200px;line-height:20px;font-size:12px'>";
    for(int i = 0; i < 10; ++i)
    {
        out += "<span style='float:left;width:30px;height:" + std::to_string(20 * (i % 4 + 1)) +
               "px;background:#a11'></span>";
        out += "<span style='float:right;width:24px;height:" + std::to_string(20 * (i % 3 + 1)) +
               "px;background:#1a1'></span>";
    }
    out += "satir bir<br>satir iki<br>satir uc<br>satir dort<br>satir bes<br>satir alti<br>"
           "satir yedi<br>satir sekiz<br>satir dokuz<br>satir on<br>satir onbir<br>satir oniki";
    return out + "</div></body>";
}

// zero-height and zero-width floats, plus floats with margins.
std::string s_degenerate()
{
    return std::string(kHead) +
           "<div style='width:240px'>"
           "<span style='float:left;width:40px;height:0;background:#a11'></span>"
           "<span style='float:right;width:0;height:30px;background:#1a1'></span>"
           "<span style='float:left;width:0;height:0'></span>"
           "<span style='float:left;width:30px;height:20px;margin:5px 7px;background:#22a'></span>"
           "<span style='float:right;width:30px;height:20px;margin:3px 11px;background:#aa1'></span>"
           "dejenere float testi metni biraz uzun olsun ki birkac satir olsun"
           "</div></body>";
}

// many floats whose inner edges are nearly identical -- exercises the index's
// group boundaries and its bail-out path.
std::string s_near_equal_edges(int n)
{
    std::string out = kHead;
    out += "<div style='width:300px'>";
    for(int i = 0; i < n; ++i)
    {
        // widths differing by a hundredth of a pixel
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.4f", 40.0 + 0.00003 * (double)i);
        out += "<span style='float:right;width:" + std::string(buf) + "px;height:12px;background:#1a1'></span>";
        out += "<p style='margin:0'>satir " + std::to_string(i) + "</p>";
    }
    return out + "</div></body>";
}

// float:left and float:right of identical width stacked deep -- the shape the
// index is meant to make fast, but on both sides at once.
std::string s_deep_stack(int n)
{
    std::string out = kHead;
    out += "<div style='width:320px'>";
    for(int i = 0; i < n; ++i)
    {
        out += "<div style='padding:2px'>"
               "<span style='float:left;width:30px;height:14px;background:#a11'></span>"
               "<span style='float:right;width:30px;height:14px;background:#1a1'></span>"
               "s" + std::to_string(i) + "</div>";
    }
    return out + "</div></body>";
}

// inline-block and flex containers next to floats: different render paths that
// still consult the same formatting context.
std::string s_mixed_display()
{
    return std::string(kHead) +
           "<div style='width:300px'>"
           "<span style='float:left;width:40px;height:60px;background:#a11'></span>"
           "<span style='display:inline-block;width:60px;height:20px;background:#333'>ib</span>"
           "<span style='display:inline-block;width:80px;height:24px;background:#444'>ib2</span>"
           "<div style='display:flex'><div style='flex:1'>f1</div><div style='flex:2'>f2</div></div>"
           "<span style='float:right;width:35px;height:40px;background:#1a1'></span>"
           "<ul><li>bir</li><li>iki</li></ul>"
           "son metin akisi"
           "</div></body>";
}


// A block whose content min-width forces a second render pass: that is the
// path that calls clear_floats() and re-runs add_float on a context that has
// already been populated.
std::string s_rerender()
{
    std::string out = kHead;
    out += "<div style='width:70px;border:1px solid #555'>"
           "<span style='float:left;width:20px;height:18px;background:#a11'></span>"
           "<span style='float:right;width:18px;height:22px;background:#1a1'></span>"
           "<table style='width:100%'><tr>"
           "<td style='min-width:140px'>uzunkelimeuzunkelimeuzunkelime</td>"
           "<td>ikincihucreuzunkelime</td></tr></table>"
           "<div style='width:30px'>"
           "<span style='float:left;width:25px;height:16px;background:#22a'></span>"
           "cokuzunbirkelimeburada</div>"
           "</div>";
    // max-width / min-width on a plain block is what actually sets
    // requires_rerender in render_item_block::render(), for both the
    // is_block_formatting_context() branch (clear_floats(-1)) and the other one.
    for(int i = 0; i < 8; ++i)
    {
        out += "<div style='max-width:" + std::to_string(60 + i * 9) + "px;border:1px solid #345'>"
               "<span style='float:right;width:" + std::to_string(12 + i * 3) + "px;height:14px;background:#1a1'></span>"
               "<span style='float:left;width:" + std::to_string(10 + i * 2) + "px;height:11px;background:#a11'></span>"
               "kutu " + std::to_string(i) + " icinde metin akisi</div>";
        out += "<div style='overflow:hidden;max-width:" + std::to_string(70 + i * 7) + "px;border:1px solid #543'>"
               "<span style='float:right;width:" + std::to_string(14 + i * 2) + "px;height:13px;background:#1a1'></span>"
               "bfc kutu " + std::to_string(i) + " metni</div>";
        out += "<div style='min-width:" + std::to_string(120 + i * 13) + "px;border:1px solid #354'>"
               "<span style='float:left;width:" + std::to_string(16 + i * 3) + "px;height:15px;background:#22a'></span>"
               "genis kutu " + std::to_string(i) + "</div>";
    }
    return out + "</body>";
}

// Collapsing top margins move already-placed floats: update_floats().
std::string s_margin_collapse()
{
    std::string out = kHead;
    out += "<div style='width:300px'>";
    for(int i = 0; i < 10; ++i)
    {
        out += "<div style='margin-top:" + std::to_string(2 + i) + "px'>"
               "<span style='float:left;width:" + std::to_string(20 + (i % 3) * 9) +
               "px;height:" + std::to_string(14 + (i % 4) * 7) + "px;background:#a11'></span>"
               "<span style='float:right;width:" + std::to_string(18 + (i % 2) * 13) +
               "px;height:" + std::to_string(12 + (i % 5) * 5) + "px;background:#1a1'></span>"
               "<p style='margin-top:" + std::to_string(6 + i * 3) + "px'>paragraf " + std::to_string(i) +
               " metni biraz uzun olsun ki satirlar sarilsin</p>"
               "</div>";
    }
    return out + "</div></body>";
}


// Outer floats survive while an inner block's floats are cleared and replaced:
// the partial clear_floats() that leaves the index holding live iterators.
std::string s_partial_clear()
{
    std::string out = kHead;
    out += "<div style='width:340px'>";
    for(int i = 0; i < 10; ++i)
    {
        out += "<div>"
               "<span style='float:right;width:" + std::to_string(28 + i * 3) + "px;height:16px;background:#a11'></span>"
               "<span style='float:left;width:" + std::to_string(21 + i * 2) + "px;height:19px;background:#a1a'></span>"
               "dis metin " + std::to_string(i) +
               "<div style='max-width:" + std::to_string(90 + i * 6) + "px'>"
               "<span style='float:right;width:" + std::to_string(13 + i) + "px;height:12px;background:#1a1'></span>"
               "<span style='float:left;width:" + std::to_string(17 + i) + "px;height:14px;background:#22a'></span>"
               "ic metin " + std::to_string(i) + " biraz uzun olsun"
               "</div>"
               "<div style='min-width:" + std::to_string(200 + i * 9) + "px'>"
               "<span style='float:right;width:" + std::to_string(11 + i * 2) + "px;height:13px;background:#aa1'></span>"
               "genis ic metin " + std::to_string(i) +
               "</div>"
               "kuyruk metni " + std::to_string(i) +
               "</div>";
    }
    return out + "</div></body>";
}

struct Page
{
    const char* name;
    std::string html;
    float       w, h;
};

std::vector<Page> pages()
{
    return {
        { "hud",              page_hud(),            420.f, 120.f  },
        { "menu",             page_menu(),           680.f, 460.f  },
        { "list40",           page_list(40),         480.f, 900.f  },
        { "list150",          page_list(150),        480.f, 3000.f },
        { "both-sides-30",    s_both_sides(30),      400.f, 900.f  },
        { "both-sides-120",   s_both_sides(120),     400.f, 3000.f },
        { "overwide",         s_overwide(),          400.f, 600.f  },
        { "overflow-bottom",  s_overflow_bottom(),   400.f, 600.f  },
        { "clears",           s_clears(),            400.f, 1400.f },
        { "nested-bfc",       s_nested_bfc(),        400.f, 700.f  },
        { "absolute",         s_absolute(),          400.f, 600.f  },
        { "table-cell",       s_table_cell(),        400.f, 600.f  },
        { "tight-wrap",       s_tight_wrap(),        400.f, 900.f  },
        { "edge-touch",       s_edge_touch(),        400.f, 900.f  },
        { "degenerate",       s_degenerate(),        400.f, 600.f  },
        { "near-equal-40",    s_near_equal_edges(40),400.f, 1200.f },
        { "near-equal-150",   s_near_equal_edges(150),400.f,3000.f },
        { "deep-stack-40",    s_deep_stack(40),      400.f, 1200.f },
        { "deep-stack-150",   s_deep_stack(150),     400.f, 3000.f },
        { "mixed-display",    s_mixed_display(),     400.f, 700.f  },
        { "rerender",         s_rerender(),          400.f, 900.f  },
        { "margin-collapse",  s_margin_collapse(),   400.f, 1200.f },
        { "partial-clear",    s_partial_clear(),     400.f, 1600.f },
        // narrow viewports force different wrap points through the same pages
        { "tight-wrap-narrow",s_tight_wrap(),        120.f, 1400.f },
        { "clears-narrow",    s_clears(),            180.f, 2600.f },
        { "both-sides-narrow",s_both_sides(30),      140.f, 2000.f },
    };
}

void dump_frame(const char* label, const LhuFrame& f)
{
    std::printf("[%s] quads=%d doc=%.6fx%.6f bytes=%d\n", label, f.quad_count, f.doc_width, f.doc_height,
                (int)(f.quad_count * (int)sizeof(LhuQuad)));
    const unsigned char* p = reinterpret_cast<const unsigned char*>(f.quads);
    const size_t         n = (size_t)f.quad_count * sizeof(LhuQuad);
    for(size_t i = 0; i < n; ++i)
    {
        std::printf("%02x", p[i]);
        if((i % 48) == 47 || i + 1 == n) std::printf("\n");
    }
}

#ifdef LHU_FLOAT_STATS
void dump_stats(const char* label)
{
    const auto& s = litehtml::g_float_scan_stats;
    std::printf("%-20s scanned=%-8llu place=%-8llu (L=%llu R=%llu) add=%-6llu line=%llu/%llu "
                "fh=%llu lfh=%llu rfh=%llu minL=%llu minR=%llu upd=%llu rel=%llu clr=%llu "
                "rebuild=%llu/%llu bsearch=%llu\n",
                label,
                s.add_scan + s.place_left + s.place_right + s.line_left + s.line_right + s.floats_height +
                    s.left_height + s.right_height + s.min_left + s.min_right + s.update + s.rel_shift + s.clear,
                s.place_left + s.place_right, s.place_left, s.place_right, s.add_scan, s.line_left, s.line_right,
                s.floats_height, s.left_height, s.right_height, s.min_left, s.min_right, s.update, s.rel_shift,
                s.clear, s.rebuild_calls, s.index_rebuild, s.bsearch);
}
#endif

} // namespace

int main(int argc, char** argv)
{
    const bool stats_mode = (argc > 1 && std::strcmp(argv[1], "stats") == 0);

    const char* font_env = std::getenv("LHU_FONT");
    const char* cands[]  = { font_env, "/System/Library/Fonts/Supplemental/Arial.ttf",
                             "/system/fonts/Roboto-Regular.ttf" };
    std::vector<uint8_t> reg;
    for(const char* c : cands) { if(c) { reg = read_file(c); if(!reg.empty()) break; } }
    if(reg.empty()) { std::printf("no font\n"); return 1; }
    const auto bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");

    LhuContext* ctx = lhu_create(nullptr);
    lhu_register_font(ctx, "sans-serif", 400, 0, reg.data(), (int32_t)reg.size());
    if(!bold.empty()) lhu_register_font(ctx, "sans-serif", 700, 0, bold.data(), (int32_t)bold.size());
    lhu_set_default_font(ctx, "sans-serif", 16.f);

    for(const auto& pg : pages())
    {
        lhu_set_viewport(ctx, pg.w, pg.h);
        if(!lhu_load_html(ctx, pg.html.c_str(), nullptr))
        {
            std::printf("[%s] LOAD FAILED: %s\n", pg.name, lhu_last_error(ctx));
            continue;
        }
#ifdef LHU_FLOAT_STATS
        litehtml::g_float_scan_stats = {};
#endif
        lhu_layout(ctx, pg.w);
        LhuFrame f {};
        lhu_record(ctx, &f);

        if(stats_mode)
        {
#ifdef LHU_FLOAT_STATS
            dump_stats(pg.name);
#else
            std::printf("%s: rebuild with -DLHU_FLOAT_STATS\n", pg.name);
#endif
        } else
        {
            dump_frame(pg.name, f);

            // A second layout on the same document, then a narrower one, then
            // back: re-layout re-runs add_float on a context that has already
            // been cleared, which is where a stale index would show.
            lhu_layout(ctx, pg.w);
            lhu_record(ctx, &f);
            dump_frame((std::string(pg.name) + "/relayout").c_str(), f);

            lhu_layout(ctx, pg.w * 0.6f);
            lhu_record(ctx, &f);
            dump_frame((std::string(pg.name) + "/narrow").c_str(), f);

            lhu_layout(ctx, pg.w);
            lhu_record(ctx, &f);
            dump_frame((std::string(pg.name) + "/wide-again").c_str(), f);
        }
    }

    if(!stats_mode)
    {
        // Text mutation: the path that cannot use the layout-skip and therefore
        // pays full float placement.
        const char* labels[] = { "Esya 210000000", "E 1", "Alev Kilici Artikli", "Tas" };
        for(int r : { 40, 150 })
        {
            lhu_set_viewport(ctx, 480.f, r == 40 ? 900.f : 3000.f);
            lhu_load_html(ctx, page_list(r).c_str(), nullptr);
            lhu_layout(ctx, 480.f);
            for(const char* t : labels)
            {
                lhu_set_text(ctx, "#mark", t);
                lhu_layout(ctx, 480.f);
                LhuFrame f {};
                lhu_record(ctx, &f);
                dump_frame((std::string("mutate") + std::to_string(r) + "/" + t).c_str(), f);
            }
        }
    }

    lhu_destroy(ctx);
    std::printf("done\n");
    return 0;
}
