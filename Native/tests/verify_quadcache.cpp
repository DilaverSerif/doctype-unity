// EXPERIMENT E2 — proof that the retained display list changes nothing.
//
// The claim the cache has to earn is narrow and absolute: for the same sequence
// of API calls, the buffer lhu_record() hands back with LHU_EXP_QUADCACHE on
// must be byte-for-byte what it hands back with it off. Same count, same order,
// all 144 bytes of every quad. Not "visually the same", not "the same set".
//
// So this drives two contexts through the identical script in lockstep -- one
// created with the cache off, one with it on -- and memcmp's the whole buffer
// after every single record. LhuContext reads LHU_EXP_QUADCACHE in its
// constructor, so setenv() before lhu_create() is enough to get both out of one
// binary; nothing here compares separately built executables.
//
// Scenarios, in the order the experiment brief asks for them:
//   1  a fresh document, all four bench pages
//   2  after a text mutation (same shape, and structural)
//   3  hover entering and leaving an element that a :hover rule restyles
//   4  after a scroll
//   5  after the glyph atlas grows on a long-lived document
//   6  all of the above in sequence on one document that is never re-parsed
//   7  records the API contract does not require (no intervening layout)
//   8  E1's layout fast path and E2's cache composing -- see below
//
// REBASED ONTO E1/E5/E6. The "off" context is now the full baseline: E1's
// layout short-circuit off *and* the cache off, i.e. a render every layout and
// a full traversal every record. The "on" context always has the cache on and
// takes E1's setting from LHU_EXP_SUBTREE, so:
//
//   LHU_EXP_SUBTREE=1  compares (E1 on, E2 on) against (E1 off, E2 off)
//                      -- every comparison in the file is then an E1 x E2
//                         interaction test, not just scenario 8
//   LHU_EXP_SUBTREE=0  compares (E1 off, E2 on) against (E1 off, E2 off)
//                      -- the original E2-only claim
//
// E5 (LHU_EXP_FLOATS) and E6 (LHU_EXP_STYLECACHE) are read once per process, so
// both contexts always share them; the surrounding script runs this binary once
// per combination and diffs the per-frame digests across runs.
//
// Set LHU_VERIFY_DUMP=<path> to write one digest line per recorded frame, which
// is what makes the cross-combination comparison possible.
//
// Build after ./build_macos.sh with:
//
//   L=third_party/litehtml
//   clang++ -std=c++17 -O2 -DNDEBUG -arch arm64 -mmacosx-version-min=11.0 \
//     -I$L/include -I$L/include/litehtml -I$L/src -I$L/src/gumbo/include \
//     -I$L/src/gumbo/include/gumbo -Ithird_party -Isrc -Itests \
//     tests/verify_quadcache.cpp build/macos/obj/*.o -o build/macos/bin/lhu_verify_quadcache

#include "lhu_api.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int g_checks   = 0;
int g_failures = 0;

// Per-frame digest stream, so that runs under different toggle combinations can
// be compared against each other and not merely against themselves.
std::FILE* g_dump = nullptr;

uint64_t fnv1a(const void* data, size_t len)
{
    const auto* b = static_cast<const uint8_t*>(data);
    uint64_t    h = 1469598103934665603ull;
    for(size_t i = 0; i < len; ++i)
    {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
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

// The pair of engines under test. Every call goes to both.
struct Pair
{
    LhuContext* off = nullptr;
    LhuContext* on  = nullptr;

    LhuFrame f_off {};
    LhuFrame f_on {};

    int atlas_w = 0, atlas_h = 0;
    int record_time_growths = 0;
};

void fail(const char* scenario, const char* what, const std::string& detail)
{
    std::printf("  [FAIL] %-28s %-36s %s\n", scenario, what, detail.c_str());
    ++g_failures;
}

// Records on both contexts and demands an exact match.
//
// Also notices when the atlas changed size *inside* lhu_record() -- no other
// call happened in between -- which is the case the cache is most likely to get
// wrong, because every UV already captured points into the old packing.
bool record_and_compare(Pair& p, const char* scenario, const char* what)
{
    const int before_w = p.atlas_w;
    const int before_h = p.atlas_h;

    lhu_record(p.off, &p.f_off);
    lhu_record(p.on, &p.f_on);

    ++g_checks;

    p.atlas_w = p.f_on.font_atlas_w;
    p.atlas_h = p.f_on.font_atlas_h;

    if(before_w && (before_w != p.atlas_w || before_h != p.atlas_h))
    {
        ++p.record_time_growths;
    }

    char detail[320];

    if(p.f_off.quad_count != p.f_on.quad_count)
    {
        std::snprintf(detail, sizeof(detail), "quad count off=%d on=%d", p.f_off.quad_count, p.f_on.quad_count);
        fail(scenario, what, detail);
        return false;
    }

    if(p.f_off.doc_width != p.f_on.doc_width || p.f_off.doc_height != p.f_on.doc_height)
    {
        std::snprintf(detail, sizeof(detail), "doc %.4fx%.4f vs %.4fx%.4f", p.f_off.doc_width, p.f_off.doc_height,
                      p.f_on.doc_width, p.f_on.doc_height);
        fail(scenario, what, detail);
        return false;
    }

    if(p.f_off.font_atlas_w != p.f_on.font_atlas_w || p.f_off.font_atlas_h != p.f_on.font_atlas_h)
    {
        std::snprintf(detail, sizeof(detail), "atlas %dx%d vs %dx%d", p.f_off.font_atlas_w, p.f_off.font_atlas_h,
                      p.f_on.font_atlas_w, p.f_on.font_atlas_h);
        fail(scenario, what, detail);
        return false;
    }

    if(p.f_off.grad_lut_rows != p.f_on.grad_lut_rows)
    {
        std::snprintf(detail, sizeof(detail), "grad rows %d vs %d", p.f_off.grad_lut_rows, p.f_on.grad_lut_rows);
        fail(scenario, what, detail);
        return false;
    }

    const size_t bytes = static_cast<size_t>(p.f_on.quad_count) * sizeof(LhuQuad);
    if(bytes && std::memcmp(p.f_off.quads, p.f_on.quads, bytes) != 0)
    {
        // Point at the exact quad and the exact byte, so a failure is
        // debuggable rather than just "differs".
        int first_quad = -1, first_byte = -1;
        for(int i = 0; i < p.f_on.quad_count && first_quad < 0; ++i)
        {
            const auto* a = reinterpret_cast<const uint8_t*>(&p.f_off.quads[i]);
            const auto* b = reinterpret_cast<const uint8_t*>(&p.f_on.quads[i]);
            for(size_t k = 0; k < sizeof(LhuQuad); ++k)
            {
                if(a[k] != b[k])
                {
                    first_quad = i;
                    first_byte = static_cast<int>(k);
                    break;
                }
            }
        }
        std::snprintf(detail, sizeof(detail), "%d quads match in count but quad %d differs at byte %d",
                      p.f_on.quad_count, first_quad, first_byte);
        fail(scenario, what, detail);
        return false;
    }

    if(g_dump)
    {
        // Everything the host can observe about this frame, in one line. Quad
        // bytes are hashed rather than dumped so a 16-way comparison stays
        // readable; the byte-level memcmp above is what catches a difference
        // *within* a run.
        const size_t lb = static_cast<size_t>(p.f_on.grad_lut_rows) * static_cast<size_t>(p.f_on.grad_lut_w) * 4u;
        // Both contexts are digested, not just the subject. One run therefore
        // pins down two of the sixteen toggle combinations, and eight runs over
        // (E1, E5, E6) cover all sixteen with no transitivity argument needed.
        std::fprintf(g_dump,
                     "%-28s %-38s n=%d %.4fx%.4f qON=%016llx qOFF=%016llx grad=%d/%016llx atlas=%dx%d/%d\n",
                     scenario, what, p.f_on.quad_count, p.f_on.doc_width, p.f_on.doc_height,
                     (unsigned long long)(bytes ? fnv1a(p.f_on.quads, bytes) : 0ull),
                     (unsigned long long)(bytes ? fnv1a(p.f_off.quads, bytes) : 0ull), p.f_on.grad_lut_rows,
                     (unsigned long long)(lb ? fnv1a(p.f_on.grad_lut_pixels, lb) : 0ull), p.f_on.font_atlas_w,
                     p.f_on.font_atlas_h, p.f_on.font_atlas_version);
    }

    // The gradient LUT is rebuilt from scratch every frame in draw order, so a
    // replayed run has to re-append its rows in the right place. Compare it too.
    const size_t lut_bytes =
        static_cast<size_t>(p.f_on.grad_lut_rows) * static_cast<size_t>(p.f_on.grad_lut_w) * 4u;
    if(lut_bytes && std::memcmp(p.f_off.grad_lut_pixels, p.f_on.grad_lut_pixels, lut_bytes) != 0)
    {
        std::snprintf(detail, sizeof(detail), "gradient LUT differs (%d rows)", p.f_on.grad_lut_rows);
        fail(scenario, what, detail);
        return false;
    }

    return true;
}

void ok(const char* scenario, const char* what, const Pair& p, const char* extra = "")
{
    std::printf("  [PASS] %-28s %-36s %d quads / %.4fx%.4f%s\n", scenario, what, p.f_on.quad_count, p.f_on.doc_width,
                p.f_on.doc_height, extra);
}

void step(Pair& p, const char* scenario, const char* what)
{
    if(record_and_compare(p, scenario, what))
    {
        ok(scenario, what, p);
    }
}

// --- pages -------------------------------------------------------------------

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

std::string page_list(int rows, const char* mark_label = nullptr)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div style='margin:10px'>";

    const int marked = rows / 2;
    for(int i = 0; i < rows; ++i)
    {
        const bool        is_marked = i == marked;
        const std::string label = (is_marked && mark_label) ? std::string(mark_label) : ("Esya " + std::to_string(i + 1));

        out += "<div class='row' style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span" +
               std::string(is_marked ? " id='mark'" : "") + " style='font-size:14px'>" + label +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }

    out += "</div></body>";
    return out;
}

// A page whose rows really do restyle on :hover, so the pseudo-class path is
// exercised instead of silently doing nothing.
const char* kHoverCss = ".row:hover { background: #3a1020; border-color: #ff2255; color: #ffd0d8; }"
                        ".row:active { background: #10203a; }";

// Something to scroll: a fixed-height box with far more content than fits.
std::string page_scroll(int rows)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div id='vp' style='height:300px;overflow:auto;border:2px solid #345'>";
    for(int i = 0; i < rows; ++i)
    {
        out += "<div class='row' style='padding:7px 9px;margin-bottom:5px;border-radius:6px;background:#171c2b;"
               "border:1px solid #232b45'><span style='font-size:14px'>Satir " +
               std::to_string(i + 1) + "</span></div>";
    }
    out += "</div></body>";
    return out;
}

// An <ol> whose markers are drawn but never measured outside draw: the marker
// label is produced inside draw_list_marker, so its glyphs are rasterized
// during lhu_record() rather than during parsing. That is the one way a page
// can grow the atlas *inside* a record on this engine.
std::string page_markers(int blocks, int items, int first_size)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0'>";
    for(int b = 0; b < blocks; ++b)
    {
        // One <ol> per font size: the ten digit glyphs have to be rasterized
        // separately for every size, and none of them is ever measured outside
        // draw_list_marker. A few hundred large glyph rasters cannot fit a
        // 512x512 atlas, so the repack is forced rather than hoped for.
        out += "<ol style='font-size:" + std::to_string(first_size + b * 2) + "px;list-style-type:decimal'>";
        for(int i = 0; i < items; ++i)
        {
            out += "<li>x</li>";
        }
        out += "</ol>";
    }
    out += "</body>";
    return out;
}

// A long-lived page with one very large mutable label, so that cycling code
// points through it puts real *area* into the atlas rather than a few thousand
// 14px glyphs that a 512x512 sheet swallows without repacking.
std::string page_bigtext(int rows)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div id='mark' style='font-size:72px'>Esya</div>";
    for(int i = 0; i < rows; ++i)
    {
        out += "<div class='row' style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'><span style='font-size:14px'>Esya " +
               std::to_string(i + 1) + "</span></div>";
    }
    out += "</body>";
    return out;
}

// Glyph pressure: a lot of distinct code points at a lot of distinct sizes, so
// the atlas has to repack.
std::string page_glyph_pressure(int sizes, int first_size)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#fff'>";
    for(int s = 0; s < sizes; ++s)
    {
        out += "<div style='font-size:" + std::to_string(first_size + s) + "px'>";
        for(uint32_t cp = 0x21; cp < 0x21 + 90; ++cp)
        {
            // UTF-8 encode; everything here is 1 byte.
            out += static_cast<char>(cp);
        }
        out += "</div>";
    }
    out += "</body>";
    return out;
}

// A string that pulls in code points nothing else on the page has used, so
// setting it forces new glyph rasterization on a long-lived document.
std::string exotic_label(int step_index)
{
    std::string out = "Esya ";
    // Latin-1 supplement, Greek and Cyrillic -- ranges Arial actually covers,
    // so every one of them really does get rasterized rather than falling back
    // to a pixel-less .notdef.
    static const uint32_t kRanges[][2] = {{0x00C0u, 0x0100u}, {0x0386u, 0x03CFu}, {0x0410u, 0x0450u}};
    for(int i = 0; i < 8; ++i)
    {
        const int      idx   = step_index * 8 + i;
        const uint32_t base  = kRanges[idx % 3][0];
        const uint32_t span  = kRanges[idx % 3][1] - base;
        const uint32_t cp    = base + static_cast<uint32_t>(idx / 3) % span;
        // 2-byte UTF-8.
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// --- driving both contexts ---------------------------------------------------

void both_load(Pair& p, const std::string& html, const char* user_css, float w, float h)
{
    lhu_set_viewport(p.off, w, h);
    lhu_set_viewport(p.on, w, h);
    lhu_load_html(p.off, html.c_str(), user_css);
    lhu_load_html(p.on, html.c_str(), user_css);
    lhu_layout(p.off, w);
    lhu_layout(p.on, w);
}

void both_layout(Pair& p, float w)
{
    lhu_layout(p.off, w);
    lhu_layout(p.on, w);
}

int both_set_text(Pair& p, const char* sel, const char* text)
{
    const int a = lhu_set_text(p.off, sel, text);
    const int b = lhu_set_text(p.on, sel, text);
    if(a != b)
    {
        ++g_failures;
        std::printf("  [FAIL] lhu_set_text disagreed: off=%d on=%d\n", a, b);
    }
    return b;
}

void both_mouse_move(Pair& p, float x, float y)
{
    lhu_mouse_move(p.off, x, y);
    lhu_mouse_move(p.on, x, y);
}

void both_mouse_leave(Pair& p)
{
    lhu_mouse_leave(p.off);
    lhu_mouse_leave(p.on);
}

int both_scroll(Pair& p, float dy, float x, float y)
{
    lhu_scroll(p.off, 0.f, dy, x, y);
    return lhu_scroll(p.on, 0.f, dy, x, y);
}

LhuContext* make_context(bool cache_on, bool subtree_on, const std::vector<uint8_t>& regular,
                         const std::vector<uint8_t>& bold)
{
    setenv("LHU_EXP_QUADCACHE", cache_on ? "1" : "0", 1);
    LhuContext* ctx = lhu_create(nullptr);
    // E1 is set explicitly rather than through the environment: the "off"
    // context has to be the true baseline whatever LHU_EXP_SUBTREE says, or the
    // comparison silently degrades to fast-path-vs-fast-path.
    lhu_exp_set_enabled(ctx, subtree_on ? 1 : 0);
    lhu_register_font(ctx, "sans-serif", 400, 0, regular.data(), static_cast<int32_t>(regular.size()));
    if(!bold.empty())
    {
        lhu_register_font(ctx, "sans-serif", 700, 0, bold.data(), static_cast<int32_t>(bold.size()));
    }
    lhu_set_default_font(ctx, "sans-serif", 16.f);
    return ctx;
}

// How many times this context has actually taken E1's layout short-circuit.
int32_t skipped_layouts(LhuContext* ctx)
{
    int32_t skipped = 0;
    lhu_exp_stats(ctx, nullptr, &skipped, nullptr);
    return skipped;
}

} // namespace

int main()
{
    // Order matters: the fresh-document goldens were fixed against Arial's
    // metrics, and Liberation Sans is metric-compatible with Arial by design
    // (same advances, same ascent/descent), so it reproduces them on a Linux
    // runner. DejaVu does not; it is a last resort that will fail the goldens.
    const char* font_env = std::getenv("LHU_FONT");
    const char* regular_candidates[] = {font_env, "/System/Library/Fonts/Supplemental/Arial.ttf",
                                        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        "/system/fonts/Roboto-Regular.ttf"};

    std::vector<uint8_t> regular;
    for(const char* c : regular_candidates)
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

    std::vector<uint8_t> bold;
    for(const char* c : {"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"})
    {
        bold = read_file(c);
        if(!bold.empty())
        {
            break;
        }
    }

    // The cache is always on for p.on and always off for p.off. E1 is off for
    // p.off unconditionally (that is what makes it a baseline) and comes from
    // the environment for p.on.
    const char* subtree_env = std::getenv("LHU_EXP_SUBTREE");
    const bool  subtree_on  = !(subtree_env && std::strcmp(subtree_env, "0") == 0);

    if(const char* dump_path = std::getenv("LHU_VERIFY_DUMP"))
    {
        g_dump = std::fopen(dump_path, "w");
        if(!g_dump)
        {
            std::printf("cannot open LHU_VERIFY_DUMP=%s\n", dump_path);
            return 1;
        }
    }

    Pair p;
    p.off = make_context(false, false, regular, bold);
    p.on  = make_context(true, subtree_on, regular, bold);

    if(lhu_quadcache_stat(p.on, LHU_QC_ENABLED) != 1 || lhu_quadcache_stat(p.off, LHU_QC_ENABLED) != 0)
    {
        std::printf("the two contexts are not actually in different modes -- the whole test is vacuous\n");
        return 1;
    }

    {
        int32_t on_e1 = -1, off_e1 = -1;
        lhu_exp_stats(p.on, &on_e1, nullptr, nullptr);
        lhu_exp_stats(p.off, &off_e1, nullptr, nullptr);
        if(off_e1 != 0 || on_e1 != (subtree_on ? 1 : 0))
        {
            std::printf("E1 toggles did not take: off=%d on=%d (wanted 0 / %d)\n", off_e1, on_e1, subtree_on ? 1 : 0);
            return 1;
        }
    }

    std::printf("EXPERIMENT E2 (rebased) — reference (E1 off, cache off) vs (E1 %s, cache on)\n",
                subtree_on ? "ON" : "off");
    std::printf("  E5 floats     : %s\n", (std::getenv("LHU_EXP_FLOATS") && std::strcmp(std::getenv("LHU_EXP_FLOATS"), "0") == 0) ? "off" : "on");
    std::printf("  E6 stylecache : %s\n", (std::getenv("LHU_EXP_STYLECACHE") && std::strcmp(std::getenv("LHU_EXP_STYLECACHE"), "0") == 0) ? "off" : "on");

    // --- 1. fresh documents --------------------------------------------------
    std::printf("\n=== 1. fresh document ===\n");
    struct FreshCase
    {
        const char* name;
        std::string html;
        float       w, h;
        int         expect_quads;
        float       expect_w, expect_h;
    };

    const FreshCase fresh[] = {
        {"HUD overlay", page_hud(), 420.f, 120.f, 27, 420.0000f, 69.1016f},
        {"Settings menu", page_menu(), 680.f, 460.f, 180, 680.0000f, 335.3516f},
        {"Inventory list, 40 rows", page_list(40), 480.f, 900.f, 543, 480.0000f, 1599.6250f},
        {"Inventory list, 150 rows", page_list(150), 480.f, 3000.f, 2108, 480.0000f, 5960.0938f},
    };

    for(const FreshCase& c : fresh)
    {
        both_load(p, c.html, nullptr, c.w, c.h);
        if(record_and_compare(p, c.name, "first record identical"))
        {
            char extra[160];
            const bool counts_ok = p.f_on.quad_count == c.expect_quads;
            // Document extents are checked against the numbers the experiment
            // fixed, not just against each other: two identical wrong answers
            // would still be wrong. The tolerance is the last digit the
            // experiment quotes, nothing looser.
            const bool dims_ok = std::fabs(p.f_on.doc_width - c.expect_w) < 0.0001f &&
                                 std::fabs(p.f_on.doc_height - c.expect_h) < 0.0001f;
            std::snprintf(extra, sizeof(extra), "   expect %d / %.4fx%.4f  %s", c.expect_quads, c.expect_w, c.expect_h,
                          (counts_ok && dims_ok) ? "ok" : "MISMATCH");
            if(!counts_ok || !dims_ok)
            {
                ++g_failures;
                std::printf("  [FAIL] %-28s %-36s %d quads / %.4fx%.4f%s\n", c.name, "quad count / doc extents",
                            p.f_on.quad_count, p.f_on.doc_width, p.f_on.doc_height, extra);
            }
            else
            {
                ok(c.name, "first record identical", p, extra);
            }
        }
        ++g_checks;

        // Redraw the same untouched document a few times: this is the frame the
        // cache answers without drawing at all, and the one most likely to hand
        // back a buffer nobody refreshed.
        for(int i = 0; i < 3; ++i)
        {
            both_layout(p, c.w);
            record_and_compare(p, c.name, "unchanged redraw identical");
        }
        std::printf("  [PASS] %-28s %-36s 3 redraws\n", c.name, "unchanged redraw identical");
    }

    // --- 2. text mutation ----------------------------------------------------
    std::printf("\n=== 2. after a text mutation ===\n");
    {
        const float w = 480.f, h = 3000.f;
        both_load(p, page_list(150), nullptr, w, h);
        step(p, "list150", "baseline");

        // Same word/space split, same glyph count: the case the cache is built
        // for, and the one where a stale run would be invisible to a quad-count
        // check.
        both_set_text(p, "#mark", "Esya 86");
        both_layout(p, w);
        step(p, "list150", "counter tick (same shape)");

        both_layout(p, w);
        step(p, "list150", "and the frame after it");

        // Wider text: everything after the mutation shifts, so the cached runs
        // for the rows below have to be rejected on their origin.
        both_set_text(p, "#mark", "Esya 7600000000");
        both_layout(p, w);
        step(p, "list150", "wider label");

        // Word count changes: text nodes and render items are destroyed and
        // recreated under the mutated element.
        both_set_text(p, "#mark", "Alev Kilici Artikli");
        both_layout(p, w);
        step(p, "list150", "structural (nodes recreated)");

        both_set_text(p, "#mark", "Tas");
        both_layout(p, w);
        step(p, "list150", "structural (nodes destroyed)");

        // A label long enough to wrap moves every row below it.
        both_set_text(p, "#mark",
                      "Cok uzun bir esya adi ile satirin sarmasini zorlayan bir etiket metni burada duruyor");
        both_layout(p, w);
        step(p, "list150", "wrapping label reflows the page");

        both_set_text(p, "#mark", "Esya 76");
        both_layout(p, w);
        step(p, "list150", "back to the original");
    }

    // --- 3. hover ------------------------------------------------------------
    std::printf("\n=== 3. hover enters and leaves ===\n");
    {
        const float w = 480.f, h = 900.f;
        both_load(p, page_list(40), kHoverCss, w, h);
        step(p, "hover page", "baseline, nothing hovered");

        // Straight into the middle of a row. :hover repaints its background,
        // border and text colour without moving a single pixel of geometry, so
        // the post-layout geometry diff cannot see it -- only the restyle hook
        // can.
        both_mouse_move(p, 60.f, 300.f);
        both_layout(p, w);
        step(p, "hover page", "hover enters a row");

        both_mouse_move(p, 60.f, 340.f);
        both_layout(p, w);
        step(p, "hover page", "hover moves to the next row");

        lhu_mouse_down(p.off, 60.f, 340.f);
        lhu_mouse_down(p.on, 60.f, 340.f);
        both_layout(p, w);
        step(p, "hover page", ":active while pressed");

        lhu_mouse_up(p.off, 60.f, 340.f);
        lhu_mouse_up(p.on, 60.f, 340.f);
        both_layout(p, w);
        step(p, "hover page", "released");

        both_mouse_leave(p);
        both_layout(p, w);
        step(p, "hover page", "hover leaves");

        both_layout(p, w);
        step(p, "hover page", "settled again");
    }

    // --- 4. scroll -----------------------------------------------------------
    std::printf("\n=== 4. after a scroll ===\n");
    {
        const float w = 480.f, h = 400.f;
        both_load(p, page_scroll(60), nullptr, w, h);
        step(p, "scroll page", "baseline");

        int consumed = 0;
        for(int i = 0; i < 6; ++i)
        {
            consumed += both_scroll(p, 40.f, 200.f, 150.f);
            both_layout(p, w);
            step(p, "scroll page", "scrolled down");
        }
        for(int i = 0; i < 3; ++i)
        {
            both_scroll(p, -40.f, 200.f, 150.f);
            both_layout(p, w);
            step(p, "scroll page", "scrolled back up");
        }
        std::printf("  (%d scroll events were consumed by an element)\n", consumed);
    }

    // --- 5. atlas growth -----------------------------------------------------
    std::printf("\n=== 5. the glyph atlas grows ===\n");
    {
        // 5a: growth on a long-lived document. Each set_text pulls in code
        // points the page has never used; sooner or later the atlas repacks and
        // every UV in every cached run becomes wrong at once.
        const float w = 480.f, h = 900.f;
        both_load(p, page_bigtext(40), nullptr, w, h);
        step(p, "atlas / long-lived doc", "baseline");

        int  first_w = p.f_on.font_atlas_w, first_h = p.f_on.font_atlas_h;
        int  grew_at = -1;
        for(int i = 0; i < 400; ++i)
        {
            const std::string label = exotic_label(i);
            both_set_text(p, "#mark", label.c_str());
            both_layout(p, w);
            if(!record_and_compare(p, "atlas / long-lived doc", "identical across a repack"))
            {
                break;
            }
            if(grew_at < 0 && (p.f_on.font_atlas_w != first_w || p.f_on.font_atlas_h != first_h))
            {
                grew_at = i;
            }
        }
        char extra[160];
        std::snprintf(extra, sizeof(extra), "   atlas %dx%d -> %dx%d, first repack at step %d", first_w, first_h,
                      p.f_on.font_atlas_w, p.f_on.font_atlas_h, grew_at);
        if(grew_at < 0)
        {
            std::printf("  [WARN] atlas never repacked in this run -- the scenario did not do its job\n");
        }
        ok("atlas / long-lived doc", "400 exotic labels, identical", p, extra);

        // 5b: growth *inside* lhu_record(). <ol> markers are produced by
        // draw_list_marker, so their glyphs are rasterized during the record
        // itself. Load enough glyph pressure first that the next rasterization
        // has to repack.
        int forced = 0;
        for(int attempt = 0; attempt < 3; ++attempt)
        {
            // Fill the atlas with ordinary measured text first, then hand it a
            // page whose glyphs only exist at draw time.
            both_load(p, page_glyph_pressure(10 + attempt * 4, 9), nullptr, 900.f, 900.f);
            record_and_compare(p, "atlas / mid-record", "glyph pressure page");

            const int w_before = p.f_on.font_atlas_w, h_before = p.f_on.font_atlas_h;
            both_load(p, page_markers(40, 60, 24 + attempt), nullptr, 900.f, 900.f);
            record_and_compare(p, "atlas / mid-record", "marker page");
            if(p.f_on.font_atlas_w != w_before || p.f_on.font_atlas_h != h_before)
            {
                ++forced;
            }

            // Redraw it: if the retry left stale UVs cached, this is where they
            // would surface.
            both_layout(p, 900.f);
            record_and_compare(p, "atlas / mid-record", "redraw after the repack");
            both_layout(p, 900.f);
            record_and_compare(p, "atlas / mid-record", "and once more");
            both_set_text(p, "#does-not-exist", "x");
        }
        std::snprintf(extra, sizeof(extra), "   the marker page repacked the atlas on %d of 3 loads%s", forced,
                      forced ? "" : " (never forced -- weaker evidence)");
        ok("atlas / mid-record", "identical across mid-record repack", p, extra);
    }

    // --- 6. everything, in sequence, on one document -------------------------
    std::printf("\n=== 6. one long-lived document, everything in sequence ===\n");
    {
        const float w = 480.f, h = 900.f;
        both_load(p, page_list(60), kHoverCss, w, h);
        step(p, "long-lived", "baseline");

        const char* labels[] = {"Esya 31", "Esya 41", "Esya 31 (yeni)", "E", "Esya 3100000", "Esya 31"};

        for(int round = 0; round < 40; ++round)
        {
            both_set_text(p, "#mark", labels[round % 6]);
            both_layout(p, w);
            if(!record_and_compare(p, "long-lived", "text"))
                break;

            both_mouse_move(p, 60.f, static_cast<float>(80 + (round * 37) % 500));
            both_layout(p, w);
            if(!record_and_compare(p, "long-lived", "hover"))
                break;

            both_layout(p, w);
            if(!record_and_compare(p, "long-lived", "idle redraw"))
                break;

            lhu_mouse_down(p.off, 60.f, static_cast<float>(80 + (round * 37) % 500));
            lhu_mouse_down(p.on, 60.f, static_cast<float>(80 + (round * 37) % 500));
            both_layout(p, w);
            if(!record_and_compare(p, "long-lived", "press"))
                break;

            lhu_mouse_up(p.off, 60.f, static_cast<float>(80 + (round * 37) % 500));
            lhu_mouse_up(p.on, 60.f, static_cast<float>(80 + (round * 37) % 500));
            both_mouse_leave(p);

            const std::string exotic = exotic_label(round * 3);
            both_set_text(p, "#mark", exotic.c_str());
            both_layout(p, w);
            if(!record_and_compare(p, "long-lived", "exotic glyphs"))
                break;
        }
        ok("long-lived", "40 rounds of text+hover+idle", p);

        // And the device scale, which moves every metric.
        lhu_set_device_scale(p.off, 2.f);
        lhu_set_device_scale(p.on, 2.f);
        both_layout(p, w);
        step(p, "long-lived", "device scale changed");

        lhu_set_viewport(p.off, 300.f, 500.f);
        lhu_set_viewport(p.on, 300.f, 500.f);
        both_layout(p, 300.f);
        step(p, "long-lived", "viewport changed");
    }

    // --- 7. the sequences the API contract does not require ----------------
    //
    // lhu_set_text's contract says the caller must lay out before recording
    // again. A caller that does not gets the previous frame's line boxes -- and
    // must get *exactly* that with the cache on, not something else. Same for
    // recording twice with nothing in between.
    std::printf("\n=== 7. records without an intervening layout ===\n");
    {
        const float w = 480.f, h = 900.f;
        both_load(p, page_list(40), kHoverCss, w, h);
        step(p, "no-layout", "baseline");

        record_and_compare(p, "no-layout", "record twice back to back");
        record_and_compare(p, "no-layout", "and a third time");
        std::printf("  [PASS] %-28s %-36s 2 extra records\n", "no-layout", "record twice back to back");

        both_set_text(p, "#mark", "Esya 99");
        step(p, "no-layout", "set_text, then record without layout");

        both_layout(p, w);
        step(p, "no-layout", "and now with the layout it owed");

        both_mouse_move(p, 60.f, 300.f);
        step(p, "no-layout", "hover, then record without layout");

        both_mouse_leave(p);
        step(p, "no-layout", "unhover, then record without layout");
    }

    // --- 8. E1's layout fast path composing with E2's cache ------------------
    //
    // THE failure mode of this rebase, tested rather than argued.
    //
    // lhu_set_text writes a new string whose measured width is bit-identical to
    // the old one -- "Esya 76" -> "Esya 86", tabular digits. E1 then skips
    // document::render() because provably nothing moved. E2's dirtiness
    // detection is a diff of fourteen geometry floats per render item, and
    // nothing moved, so left to itself it finds the whole page clean and
    // replays last frame's quads. The two fast paths would compose into a
    // counter that never updates again.
    //
    // This asserts three separate things, because any one of them alone can
    // pass while the bug is present:
    //   a) E1 really did take the short-circuit (skipped count went up). Without
    //      this the scenario is vacuous -- it would still pass on a build where
    //      the fast path never fires.
    //   b) the recorded frame is byte-identical to the reference context, which
    //      rendered fully and drew with no cache at all. That is what "shows the
    //      new text" means in quads.
    //   c) the frame actually *changed* from the one before the mutation. (b)
    //      alone cannot catch a cache that replays a stale frame if the
    //      reference were somehow stale too; (c) says the pixels moved.
    //
    // Nothing here hard-codes a font's metrics. It asserts that two strings
    // which this engine measured to the same width produce different glyph
    // quads -- measured behaviour, true of any font, on any platform. If some
    // font makes the digits different widths, E1 simply does not take the fast
    // path and (a) reports that honestly instead of the test lying.
    std::printf("\n=== 8. E1 layout fast path x E2 quad cache ===\n");
    if(!subtree_on)
    {
        std::printf("  (LHU_EXP_SUBTREE=0: the same script runs, but there is no fast path to compose\n"
                    "   with, so the fast-path assertions are reported rather than enforced. The frames\n"
                    "   themselves must still come out identical to the E1-on run -- that is what the\n"
                    "   cross-combination digest comparison checks.)\n");
    }
    {
        const float w = 480.f, h = 3000.f;
        both_load(p, page_list(150), nullptr, w, h);
        step(p, "E1 x E2", "baseline");

        // Digits only, same count, so the measured width does not move.
        const char* ticks[] = {"Esya 86", "Esya 96", "Esya 06", "Esya 16", "Esya 26",
                               "Esya 36", "Esya 46", "Esya 56", "Esya 66", "Esya 76"};

        int fast_path_taken = 0;
        int frame_changed   = 0;

        std::vector<uint8_t> prev(reinterpret_cast<const uint8_t*>(p.f_on.quads),
                                  reinterpret_cast<const uint8_t*>(p.f_on.quads) +
                                      static_cast<size_t>(p.f_on.quad_count) * sizeof(LhuQuad));

        for(int i = 0; i < 10; ++i)
        {
            both_set_text(p, "#mark", ticks[i]);

            const int32_t before = skipped_layouts(p.on);
            both_layout(p, w);
            const bool skipped = skipped_layouts(p.on) > before;
            fast_path_taken += skipped ? 1 : 0;

            if(!record_and_compare(p, "E1 x E2", "counter tick through both fast paths"))
            {
                break;
            }

            const size_t bytes = static_cast<size_t>(p.f_on.quad_count) * sizeof(LhuQuad);
            const bool   moved = prev.size() != bytes || std::memcmp(prev.data(), p.f_on.quads, bytes) != 0;
            frame_changed += moved ? 1 : 0;

            if(skipped && !moved)
            {
                // The exact bug this scenario exists for: layout skipped, cache
                // hit, and the buffer handed back is the previous frame.
                char detail[200];
                std::snprintf(detail, sizeof(detail),
                              "tick %d ('%s') skipped layout AND produced a byte-identical frame -- "
                              "the new text was never recorded",
                              i, ticks[i]);
                fail("E1 x E2", "stale frame after mutation", detail);
            }

            prev.assign(reinterpret_cast<const uint8_t*>(p.f_on.quads),
                        reinterpret_cast<const uint8_t*>(p.f_on.quads) + bytes);
        }

        char extra[200];
        std::snprintf(extra, sizeof(extra), "   E1 skipped layout on %d/10 ticks, frame changed on %d/10",
                      fast_path_taken, frame_changed);

        if(fast_path_taken == 0 && subtree_on)
        {
            ++g_failures;
            std::printf("  [FAIL] %-28s %-36s E1 never took the fast path -- this scenario proved nothing\n",
                        "E1 x E2", "fast path actually exercised");
        }
        else
        {
            ok("E1 x E2", "10 counter ticks, both fast paths", p, extra);
        }

        // And the structural path, where E1 must NOT skip: the render items are
        // new, so a skip would draw through items that never saw a line box.
        {
            const int32_t before = skipped_layouts(p.on);
            both_set_text(p, "#mark", "Alev Kilici Artikli");
            both_layout(p, w);
            if(subtree_on && skipped_layouts(p.on) != before)
            {
                ++g_failures;
                std::printf("  [FAIL] %-28s %-36s E1 skipped layout after a structural mutation\n", "E1 x E2",
                            "structural mutation forces a render");
            }
            step(p, "E1 x E2", "structural mutation still renders");
        }

        // A mutation whose width really does move: E1 must fall back, and the
        // cache has to reject every run below it on its recorded origin.
        {
            const int32_t before = skipped_layouts(p.on);
            both_set_text(p, "#mark", "Esya 7600000000");
            both_layout(p, w);
            if(subtree_on && skipped_layouts(p.on) != before)
            {
                ++g_failures;
                std::printf("  [FAIL] %-28s %-36s E1 skipped layout after the width moved\n", "E1 x E2",
                            "widened label forces a render");
            }
            step(p, "E1 x E2", "widened label still renders");
        }

        // Idle frames after all that: E1 stops skipping (no pending mutation)
        // and E2 should settle back onto the retained frame.
        for(int i = 0; i < 3; ++i)
        {
            both_layout(p, w);
            step(p, "E1 x E2", "idle after mutations");
        }
    }

    // --- 9. the host-driven invalidation the Unity side calls ----------------
    //
    // LHU_QC_INVALIDATE is the one invalidation the engine cannot decide for
    // itself: image UVs come from the host's get_image_uv callback and are baked
    // into cached quads, so a host that repacks its image atlas has silently
    // moved them. Nothing inside the engine can see that.
    //
    // Two things have to hold, and only the second is interesting:
    //   a) calling it never changes the frame -- it is a pure performance knob.
    //   b) calling it actually drops the retained frame, i.e. the very next
    //      record is a REBUILD and not a fast replay. Without (b) the C# hook
    //      would compile, run, and do nothing at all, which is exactly the
    //      failure this call exists to prevent.
    std::printf("\n=== 9. host-driven invalidation (LHU_QC_INVALIDATE) ===\n");
    {
        const float w = 480.f, h = 900.f;
        both_load(p, page_list(40), kHoverCss, w, h);
        step(p, "host invalidate", "baseline");

        // Settle onto the retained frame, so that "the next record rebuilt" is
        // a statement about the invalidate and not about warm-up.
        both_layout(p, w);
        step(p, "host invalidate", "settled");

        const long long fast_before    = lhu_quadcache_stat(p.on, LHU_QC_FRAMES_FAST);
        both_layout(p, w);
        step(p, "host invalidate", "idle frame is served fast");
        if(lhu_quadcache_stat(p.on, LHU_QC_FRAMES_FAST) == fast_before)
        {
            // Not fatal, but it makes the next assertion much weaker.
            std::printf("  [WARN] the idle frame was not served from the retained snapshot;\n"
                        "         the invalidation check below proves less than it should\n");
        }

        const long long rebuild_before = lhu_quadcache_stat(p.on, LHU_QC_FRAMES_REBUILD);

        // This is the call HtmlDocument.InvalidateDrawCache() makes.
        lhu_quadcache_stat(p.on, LHU_QC_INVALIDATE);

        both_layout(p, w);
        step(p, "host invalidate", "frame after invalidate is identical");

        const long long rebuilt = lhu_quadcache_stat(p.on, LHU_QC_FRAMES_REBUILD) - rebuild_before;
        if(rebuilt != 1)
        {
            ++g_failures;
            std::printf("  [FAIL] %-28s %-36s expected exactly 1 rebuild, got %lld -- the host's only\n"
                        "         lever against a repacked image atlas does not actually work\n",
                        "host invalidate", "invalidate forces a rebuild", rebuilt);
        }
        else
        {
            std::printf("  [PASS] %-28s %-36s next record was a full rebuild\n", "host invalidate",
                        "invalidate forces a rebuild");
        }

        // And it stays correct when called every frame, which a naive host will
        // do. This is the "safe but slow" contract the C# doc comment promises.
        for(int i = 0; i < 5; ++i)
        {
            lhu_quadcache_stat(p.on, LHU_QC_INVALIDATE);
            both_layout(p, w);
            record_and_compare(p, "host invalidate", "invalidated every frame");
        }
        std::printf("  [PASS] %-28s %-36s 5 frames\n", "host invalidate", "invalidated every frame");

        // A null context must not crash the P/Invoke.
        if(lhu_quadcache_stat(nullptr, LHU_QC_INVALIDATE) != -1)
        {
            ++g_failures;
            std::printf("  [FAIL] %-28s %-36s null ctx did not report -1\n", "host invalidate", "null-safe");
        }
    }

    std::printf("\n--- cache activity on the ON context ---\n");
    std::printf("  quads replayed from cache : %lld\n", (long long)lhu_quadcache_stat(p.on, LHU_QC_QUADS_REPLAYED));
    std::printf("  quads emitted by litehtml : %lld\n", (long long)lhu_quadcache_stat(p.on, LHU_QC_QUADS_EMITTED));
    std::printf("  cached subtree runs served: %lld\n", (long long)lhu_quadcache_stat(p.on, LHU_QC_RUNS_REPLAYED));
    std::printf("  frames fast/partial/rebuild: %lld / %lld / %lld\n",
                (long long)lhu_quadcache_stat(p.on, LHU_QC_FRAMES_FAST),
                (long long)lhu_quadcache_stat(p.on, LHU_QC_FRAMES_PARTIAL),
                (long long)lhu_quadcache_stat(p.on, LHU_QC_FRAMES_REBUILD));

    const long long replayed = lhu_quadcache_stat(p.on, LHU_QC_QUADS_REPLAYED);
    if(replayed == 0)
    {
        std::printf("\nThe cache never served a single quad -- the comparison above proves nothing.\n");
        ++g_failures;
    }

    std::printf("\n%d frame comparisons, %d failed\n", g_checks, g_failures);

    if(g_dump)
    {
        std::fclose(g_dump);
    }

    lhu_destroy(p.on);
    lhu_destroy(p.off);
    return g_failures == 0 ? 0 : 1;
}
