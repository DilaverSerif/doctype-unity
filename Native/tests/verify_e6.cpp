// Byte-identity verifier for experiment E6 (inline style="..." parse cache).
//
// Runs a scripted sequence of scenarios and writes the RAW BYTES of every
// recorded quad -- all sizeof(LhuQuad) of them, in emission order -- plus the
// quad count and the document extents, to a binary file. Run the same binary
// twice, once with LHU_EXP_STYLECACHE=1 and once with 0, and `cmp` the two
// files: any difference in any quad field, count or ordering shows up.
//
// A separate binary on purpose. tests/bench.cpp is not touched, because adding
// passes to it has been observed to move the timings of untouched code.
//
//   ./verify_e6 <out.bin>          binary dump (compare with cmp)
//   ./verify_e6 <out.bin> -v       also print a per-scenario summary to stdout

#include "lhu_api.h"

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

std::FILE* g_out     = nullptr;
bool       g_verbose = false;

// Appends: scenario name, quad count, doc extents, then every quad verbatim.
void dump(const char* scenario, LhuContext* ctx)
{
    LhuFrame f {};
    lhu_record(ctx, &f);

    const uint32_t namelen = static_cast<uint32_t>(std::strlen(scenario));
    std::fwrite(&namelen, sizeof(namelen), 1, g_out);
    std::fwrite(scenario, 1, namelen, g_out);
    std::fwrite(&f.quad_count, sizeof(f.quad_count), 1, g_out);
    std::fwrite(&f.doc_width, sizeof(f.doc_width), 1, g_out);
    std::fwrite(&f.doc_height, sizeof(f.doc_height), 1, g_out);
    std::fwrite(f.quads, sizeof(LhuQuad), static_cast<size_t>(f.quad_count), g_out);

    if(g_verbose)
    {
        std::printf("  %-58s %6d quads  %.4f x %.4f\n", scenario, f.quad_count, f.doc_width, f.doc_height);
    }
}

void render(LhuContext* ctx, const char* scenario, const std::string& html, float w, float h)
{
    lhu_set_viewport(ctx, w, h);
    lhu_load_html(ctx, html.c_str(), nullptr);
    lhu_layout(ctx, w);
    dump(scenario, ctx);
}

// --- bench pages, copied verbatim from tests/bench.cpp -----------------------

std::string page_hud(const char* hp = "HP 84 / 100")
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#fff'>"
                       "<div style='padding:8px'>"
                       "<div id='hp' style='font-size:22px'>") +
           hp +
           "</div>"
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
           resolution +
           "</td></tr>"
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
    const int   marked = rows / 2;
    for(int i = 0; i < rows; ++i)
    {
        const bool is_marked = i == marked;
        out += "<div style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span" +
               std::string(is_marked ? " id='mark'" : "") + " style='font-size:14px'>Esya " + std::to_string(i + 1) +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }
    out += "</div></body>";
    return out;
}

// --- adversarial pages -------------------------------------------------------

// The same declaration block on different tags and under different parents. If
// the cache keyed on anything element-shaped, or if combine() interacted with
// what the master sheet had already put on the element, div/span/p/td/li would
// stop differing the way they must.
const char* kPageSameStringDifferentTags =
    "<body style='margin:0;font-family:sans-serif'>"
    "<div style='padding:4px;color:#3a7'>A</div>"
    "<span style='padding:4px;color:#3a7'>B</span>"
    "<p style='padding:4px;color:#3a7'>C</p>"
    "<div style='display:flex'><span style='padding:4px;color:#3a7'>D</span>"
    "<div style='padding:4px;color:#3a7'>E</div></div>"
    "<table><tr><td style='padding:4px;color:#3a7'>F</td>"
    "<td><span style='padding:4px;color:#3a7'>G</span></td></tr></table>"
    "<ul><li style='padding:4px;color:#3a7'>H</li></ul>"
    "<div style='font-size:9px'><span style='padding:4px;color:#3a7'>I</span></div>"
    "<div style='font-size:31px'><span style='padding:4px;color:#3a7'>J</span></div>"
    "</body>";

// Identical declarations, written three ways: reordered, extra whitespace,
// different case. These must stay three distinct cache entries that each parse
// to the same thing -- and the rendering must not depend on which one an
// element used.
const char* kPageWhitespaceAndOrder =
    "<body style='margin:0;font-family:sans-serif'>"
    "<div style='color:#c33;font-size:17px;padding:3px'>one</div>"
    "<div style='font-size:17px;padding:3px;color:#c33'>two</div>"
    "<div style='  color : #c33 ;  font-size : 17px ;   padding : 3px  '>three</div>"
    "<div style='COLOR:#C33;FONT-SIZE:17px;PADDING:3px'>four</div>"
    "<div style='color:#c33;font-size:17px;padding:3px;'>five</div>"
    "</body>";

// var(). The declaration text is identical on every child, but the custom
// property it reads is defined at a different value on each parent, so the
// substituted result MUST differ per element. This is the case that breaks a
// cache which shares the substituted value instead of the raw tokens.
const char* kPageVars =
    "<body style='margin:0;font-family:sans-serif;--pad:2px;--fg:#111'>"
    "<div style='--pad:4px;--fg:#a11'><span style='padding:var(--pad);color:var(--fg);font-size:15px'>v1</span></div>"
    "<div style='--pad:14px;--fg:#1a1'><span style='padding:var(--pad);color:var(--fg);font-size:15px'>v2</span></div>"
    "<div style='--pad:24px;--fg:#11a'><span style='padding:var(--pad);color:var(--fg);font-size:15px'>v3</span></div>"
    "<div><span style='padding:var(--pad);color:var(--fg);font-size:15px'>v4 inherits body</span></div>"
    "<div><span style='padding:var(--nope,7px);color:var(--nope,#753);font-size:15px'>v5 fallback</span></div>"
    "<div><span style='padding:var(--nope);color:var(--nope);font-size:15px'>v6 undefined</span></div>"
    "</body>";

// !important, in every arrangement that exercises add_parsed_property's rule:
// against a stylesheet declaration, twice in one block, important-then-normal
// and normal-then-important.
const char* kPageImportant =
    "<style>div{color:#0a0 !important;font-size:19px !important;padding:11px}"
    "span{color:#00a;padding:2px !important}</style>"
    "<body style='margin:0;font-family:sans-serif'>"
    "<div style='color:#a00;font-size:9px;padding:3px'>i1 loses to sheet</div>"
    "<div style='color:#a00 !important;font-size:9px !important;padding:3px'>i2 wins</div>"
    "<div style='color:#a00 !important;color:#00a'>i3 important then normal</div>"
    "<div style='color:#a00;color:#00a !important'>i4 normal then important</div>"
    "<div style='color:#a00 !important;color:#00a !important'>i5 both important</div>"
    "<span style='color:#a00;padding:33px'>i6 span</span>"
    "</body>";

// Malformed and hostile declarations. Whatever litehtml does with these, it has
// to do the same thing whether the block was parsed now or an hour ago.
const char* kPageMalformed =
    "<body style='margin:0;font-family:sans-serif'>"
    "<div style='color'>m1 no colon</div>"
    "<div style='color:'>m2 empty value</div>"
    "<div style=':#f00'>m3 no name</div>"
    "<div style=';;;color:#c60;;;'>m4 stray semicolons</div>"
    "<div style='color:#c60;;font-size:'>m5</div>"
    "<div style='padding:8px;@media screen{color:red}'>m6 at-rule in a style block</div>"
    "<div style='color:#zzz;font-size:9qq;padding:8px'>m7 bad values</div>"
    "<div style='font-size:15px;color:rgb(10,'>m8 unterminated function</div>"
    "<div style='width:calc(50% - ;padding:5px'>m9 unterminated block</div>"
    "<div style='--x:;color:#c60'>m10 empty custom property</div>"
    "<div style='color:#c60 !importantnot;padding:4px'>m11 bogus bang</div>"
    "</body>";

// THE load-bearing one: one declaration string, a computed value that must come
// out different on every element that carries it. font-size in em/% is relative
// to the parent's, so 'font-size:1.25em' nested nine deep is nine different
// computed sizes from one identical string; the glyph quads make any sharing of
// the computed result immediately visible. `color:inherit` and a percentage
// line-height ride along.
std::string page_inherited_differs()
{
    std::string out = "<body style='margin:0;font-family:sans-serif;font-size:10px;color:#246'>";
    for(int i = 0; i < 9; ++i)
    {
        out += "<div style='font-size:1.25em;line-height:120%;padding-left:2px;color:inherit'>"
               "<span style='font-size:1.25em;color:inherit'>L" +
               std::to_string(i) + "</span>";
    }
    for(int i = 0; i < 9; ++i)
    {
        out += "</div>";
    }
    // Siblings, so the same string is also seen at the same depth twice.
    out += "<div style='font-size:2em;color:#642'><span style='font-size:1.25em;color:inherit'>S1</span></div>"
           "<div style='font-size:0.5em;color:#642'><span style='font-size:1.25em;color:inherit'>S2</span></div>"
           "</body>";
    return out;
}

// Nine styled elements that repeat two strings, used to prove the cache warms
// across page loads without leaking values between documents.
const char* kPageSmallA = "<body style='margin:0;font-family:sans-serif;font-size:12px'>"
                          "<div style='padding:3px;background:#123'>a</div>"
                          "<div style='padding:3px;background:#123'>b</div>"
                          "<div style='padding:7px;background:#321'>c</div></body>";
const char* kPageSmallB = "<body style='margin:0;font-family:sans-serif;font-size:12px'>"
                          "<span style='padding:3px;background:#123'>d</span>"
                          "<div style='padding:7px;background:#321'>e</div>"
                          "<div style='padding:3px;background:#123'>f</div></body>";


// More distinct style strings than the cache can hold, so the "cache full ->
// parse directly" fallback is exercised inside one document, with the earlier
// (cached) and later (uncached) elements interleaved by repeat.
std::string page_cache_overflow()
{
    std::string out = "<body style='margin:0;font-family:sans-serif;font-size:11px'>";
    for(int i = 0; i < 1400; ++i)
    {
        out += "<div style='padding-left:" + std::to_string(i % 37) + "px;margin-top:" + std::to_string(i % 5) +
               "px;color:#" + std::to_string(100000 + i) + "'>o" + std::to_string(i) + "</div>";
    }
    // Repeat a prefix, so strings that were cached early are used again after
    // the cache filled up.
    for(int i = 0; i < 40; ++i)
    {
        out += "<div style='padding-left:" + std::to_string(i % 37) + "px;margin-top:" + std::to_string(i % 5) +
               "px;color:#" + std::to_string(100000 + i) + "'>r" + std::to_string(i) + "</div>";
    }
    return out + "</body>";
}

// url() values (which take the baseurl argument style::add is given) and colour
// names that only document_container::resolve_color can answer -- the two
// things that make a parse depend on something other than the string.
const char* kPageUrlsAndNamedColors =
    "<body style='margin:0;font-family:sans-serif;font-size:13px'>"
    "<div style='background-image:url(a.png);padding:5px'>u1</div>"
    "<div style='background-image:url(a.png);padding:5px'>u2 same string</div>"
    "<div style='background-image:url(sub/dir/a.png);width:40px;height:9px'>u3</div>"
    "<ul style='list-style-image:url(bullet.png)'><li style='padding:2px'>u4</li>"
    "<li style='padding:2px'>u5</li></ul>"
    "<div style='color:menutext;padding:3px'>c1 system colour</div>"
    "<div style='color:menutext;padding:3px'>c2 same</div>"
    "<div style='color:rebeccapurple;padding:3px'>c3</div>"
    "<div style='color:notacolour;padding:3px'>c4 unknown</div>"
    "<div style='background:highlight;padding:3px'>c5</div>"
    "</body>";

LhuContext* make_ctx(const std::vector<uint8_t>& regular, const std::vector<uint8_t>& bold)
{
    LhuContext* ctx = lhu_create(nullptr);
    lhu_register_font(ctx, "sans-serif", 400, 0, regular.data(), static_cast<int32_t>(regular.size()));
    if(!bold.empty())
    {
        lhu_register_font(ctx, "sans-serif", 700, 0, bold.data(), static_cast<int32_t>(bold.size()));
    }
    lhu_set_default_font(ctx, "sans-serif", 16.f);
    return ctx;
}

// Every scenario, run against one context. `tag` distinguishes the run.
void run_suite(LhuContext* ctx, const std::string& tag)
{
    render(ctx, (tag + "/hud").c_str(), page_hud(), 420.f, 120.f);
    render(ctx, (tag + "/menu").c_str(), page_menu(), 680.f, 460.f);
    render(ctx, (tag + "/list40").c_str(), page_list(40), 480.f, 900.f);
    render(ctx, (tag + "/list150").c_str(), page_list(150), 480.f, 900.f);

    render(ctx, (tag + "/same-string-different-tags").c_str(), kPageSameStringDifferentTags, 400.f, 600.f);
    render(ctx, (tag + "/whitespace-and-order").c_str(), kPageWhitespaceAndOrder, 400.f, 600.f);
    render(ctx, (tag + "/vars").c_str(), kPageVars, 400.f, 600.f);
    render(ctx, (tag + "/important").c_str(), kPageImportant, 400.f, 600.f);
    render(ctx, (tag + "/malformed").c_str(), kPageMalformed, 400.f, 600.f);
    render(ctx, (tag + "/inherited-differs").c_str(), page_inherited_differs(), 400.f, 700.f);
    render(ctx, (tag + "/urls-and-named-colors").c_str(), kPageUrlsAndNamedColors, 400.f, 600.f);
    render(ctx, (tag + "/cache-overflow").c_str(), page_cache_overflow(), 400.f, 800.f);
    // and again, now that the cache is full, so the fallback is taken from the start
    render(ctx, (tag + "/cache-overflow-2").c_str(), page_cache_overflow(), 400.f, 800.f);
    render(ctx, (tag + "/after-overflow-hud").c_str(), page_hud(), 420.f, 120.f);

    // Quirks mode (no doctype) vs standards mode, same body.
    const std::string body = std::string(kPageSameStringDifferentTags) + std::string(kPageWhitespaceAndOrder);
    render(ctx, (tag + "/quirks").c_str(), body, 400.f, 700.f);
    render(ctx, (tag + "/standards").c_str(), "<!DOCTYPE html><html>" + body + "</html>", 400.f, 700.f);
    // ...and again in the other order, so a cache warmed by one mode is used by the other.
    render(ctx, (tag + "/standards-then-quirks/std").c_str(), "<!DOCTYPE html><html>" + body + "</html>", 400.f, 700.f);
    render(ctx, (tag + "/standards-then-quirks/qks").c_str(), body, 400.f, 700.f);

    // Sequential loads in one context, interleaved so entries inserted by one
    // page are consumed by the next.
    for(int round = 0; round < 3; ++round)
    {
        const std::string p = tag + "/seq" + std::to_string(round);
        render(ctx, (p + "/A").c_str(), kPageSmallA, 300.f, 300.f);
        render(ctx, (p + "/B").c_str(), kPageSmallB, 300.f, 300.f);
        render(ctx, (p + "/hud").c_str(), page_hud("HP 12 / 100"), 420.f, 120.f);
        render(ctx, (p + "/A2").c_str(), kPageSmallA, 300.f, 300.f);
        render(ctx, (p + "/list40").c_str(), page_list(40), 480.f, 900.f);
    }

    // A mutation after a cached parse: lhu_set_text re-runs compute_styles on
    // the touched subtree, so it must see the same styles.
    lhu_set_viewport(ctx, 480.f, 900.f);
    lhu_load_html(ctx, page_list(40).c_str(), nullptr);
    lhu_layout(ctx, 480.f);
    lhu_set_text(ctx, "#mark", "Esya 21 degistirildi");
    lhu_layout(ctx, 480.f);
    dump((tag + "/set_text").c_str(), ctx);

    // Hit testing walks refresh_styles/compute_styles again.
    lhu_mouse_move(ctx, 100.f, 100.f);
    lhu_layout(ctx, 480.f);
    dump((tag + "/after-mouse-move").c_str(), ctx);
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        std::printf("usage: verify_e6 <out.bin> [-v]\n");
        return 2;
    }
    g_verbose = argc > 2 && std::strcmp(argv[2], "-v") == 0;

    const char* font_env = std::getenv("LHU_FONT");
    const char* regular_candidates[] = {font_env, "/System/Library/Fonts/Supplemental/Arial.ttf",
                                        "/system/fonts/Roboto-Regular.ttf"};
    std::vector<uint8_t> regular;
    for(const char* c : regular_candidates)
    {
        if(c && !(regular = read_file(c)).empty())
        {
            break;
        }
    }
    if(regular.empty())
    {
        std::printf("no usable font found; set LHU_FONT\n");
        return 1;
    }
    std::vector<uint8_t> bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");

    g_out = std::fopen(argv[1], "wb");
    if(!g_out)
    {
        std::printf("cannot write %s\n", argv[1]);
        return 1;
    }

    // Master CSS mode GAME_UI, one context.
    {
        LhuContext* ctx = make_ctx(regular, bold);
        lhu_set_master_css(ctx, LHU_MASTER_CSS_GAME_UI);
        run_suite(ctx, "gameui");
        lhu_destroy(ctx);
    }

    // Master CSS mode FULL, one context.
    {
        LhuContext* ctx = make_ctx(regular, bold);
        lhu_set_master_css(ctx, LHU_MASTER_CSS_FULL);
        run_suite(ctx, "full");
        lhu_destroy(ctx);
    }

    // Mode switched mid-life on a single context, with pages either side.
    {
        LhuContext* ctx = make_ctx(regular, bold);
        render(ctx, "switch/gameui-before", kPageWhitespaceAndOrder, 400.f, 600.f);
        lhu_set_master_css(ctx, LHU_MASTER_CSS_FULL);
        render(ctx, "switch/full-after", kPageWhitespaceAndOrder, 400.f, 600.f);
        lhu_set_master_css(ctx, LHU_MASTER_CSS_GAME_UI);
        render(ctx, "switch/gameui-again", kPageWhitespaceAndOrder, 400.f, 600.f);
        render(ctx, "switch/important", kPageImportant, 400.f, 600.f);
        lhu_destroy(ctx);
    }

    // Two contexts alive at once, interleaved, on different master CSS modes.
    // Their caches must not see each other.
    {
        LhuContext* a = make_ctx(regular, bold);
        LhuContext* b = make_ctx(regular, bold);
        lhu_set_master_css(a, LHU_MASTER_CSS_GAME_UI);
        lhu_set_master_css(b, LHU_MASTER_CSS_FULL);

        render(a, "twoctx/a-important", kPageImportant, 400.f, 600.f);
        render(b, "twoctx/b-important", kPageImportant, 400.f, 600.f);
        render(a, "twoctx/a-vars", kPageVars, 400.f, 600.f);
        render(b, "twoctx/b-vars", kPageVars, 400.f, 600.f);
        render(b, "twoctx/b-inherited", page_inherited_differs(), 400.f, 700.f);
        render(a, "twoctx/a-inherited", page_inherited_differs(), 400.f, 700.f);
        render(a, "twoctx/a-list150", page_list(150), 480.f, 900.f);
        render(b, "twoctx/b-list150", page_list(150), 480.f, 900.f);

        lhu_destroy(a);
        lhu_destroy(b);
    }

    std::fclose(g_out);
    return 0;
}
