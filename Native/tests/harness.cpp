// Standalone test harness for the Doctype native layer.
//
// Runs without Unity and without a GPU: it drives the same C ABI that C# does,
// asserts on the recorded quad stream, and rasterizes the result to PNG through
// the reference rasterizer so the output can be eyeballed and diffed.
//
//   build/macos/bin/lhu_harness [output-dir]

#include "lhu_api.h"
#include "lhu_font.h"
#include "lhu_raster.h"

#include "lodepng.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if(ok)
    {
        std::printf("  \033[32mPASS\033[0m  %s\n", what.c_str());
    }
    else
    {
        ++g_failed;
        std::printf("  \033[31mFAIL\033[0m  %s\n", what.c_str());
    }
}

void check_near(float actual, float expected, float tol, const std::string& what)
{
    const bool ok = std::fabs(actual - expected) <= tol;
    ++g_checks;
    if(ok)
    {
        std::printf("  \033[32mPASS\033[0m  %s (%.2f)\n", what.c_str(), actual);
    }
    else
    {
        ++g_failed;
        std::printf("  \033[31mFAIL\033[0m  %s: got %.3f, expected %.3f (+/-%.3f)\n", what.c_str(), actual, expected,
                    tol);
    }
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

const char* type_name(int t)
{
    switch(t)
    {
    case LHU_QUAD_RECT: return "RECT";
    case LHU_QUAD_BORDER: return "BORDER";
    case LHU_QUAD_GLYPH: return "GLYPH";
    case LHU_QUAD_IMAGE: return "IMAGE";
    case LHU_QUAD_LINEAR_GRAD: return "LINEAR";
    case LHU_QUAD_RADIAL_GRAD: return "RADIAL";
    case LHU_QUAD_CONIC_GRAD: return "CONIC";
    default: return "?";
    }
}

int count_of(const LhuFrame& f, int type)
{
    int n = 0;
    for(int i = 0; i < f.quad_count; ++i)
    {
        if(f.quads[i].type == type)
        {
            ++n;
        }
    }
    return n;
}

const LhuQuad* first_of(const LhuFrame& f, int type)
{
    for(int i = 0; i < f.quad_count; ++i)
    {
        if(f.quads[i].type == type)
        {
            return &f.quads[i];
        }
    }
    return nullptr;
}

// Renders a frame and writes it out as a PNG.
void write_png(const std::string& path, const LhuFrame& frame, int w, int h, uint32_t background)
{
    std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 4);
    for(size_t i = 0; i < fb.size(); i += 4)
    {
        fb[i + 0] = static_cast<uint8_t>(background & 0xFF);
        fb[i + 1] = static_cast<uint8_t>((background >> 8) & 0xFF);
        fb[i + 2] = static_cast<uint8_t>((background >> 16) & 0xFF);
        fb[i + 3] = static_cast<uint8_t>((background >> 24) & 0xFF);
    }

    lhu_raster::Textures tex;
    tex.font      = frame.font_atlas_pixels;
    tex.font_w    = frame.font_atlas_w;
    tex.font_h    = frame.font_atlas_h;
    tex.grad      = frame.grad_lut_pixels;
    tex.grad_w    = frame.grad_lut_w;
    tex.grad_rows = frame.grad_lut_rows;

    for(int i = 0; i < frame.quad_count; ++i)
    {
        lhu_raster::draw_quad(fb, w, h, frame.quads[i], tex);
    }

    if(unsigned err = lodepng::encode(path, fb, static_cast<unsigned>(w), static_cast<unsigned>(h)))
    {
        std::printf("  png write failed: %s\n", lodepng_error_text(err));
    }
    else
    {
        std::printf("  -> %s\n", path.c_str());
    }
}

// Samples a pixel out of a freshly rendered frame — used to assert on colour.
void sample_pixel(const LhuFrame& frame, int w, int h, int px, int py, uint8_t out[4])
{
    std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 4, 0);

    lhu_raster::Textures tex;
    tex.font      = frame.font_atlas_pixels;
    tex.font_w    = frame.font_atlas_w;
    tex.font_h    = frame.font_atlas_h;
    tex.grad      = frame.grad_lut_pixels;
    tex.grad_w    = frame.grad_lut_w;
    tex.grad_rows = frame.grad_lut_rows;

    for(int i = 0; i < frame.quad_count; ++i)
    {
        lhu_raster::draw_quad(fb, w, h, frame.quads[i], tex);
    }

    std::memcpy(out, &fb[(static_cast<size_t>(py) * w + px) * 4], 4);
}

std::string g_out_dir = ".";
std::string g_root;

struct Fixture
{
    LhuContext* ctx = nullptr;

    explicit Fixture(bool use_ahem = true)
    {
        ctx = lhu_create(nullptr);

        const auto ahem = read_file(g_root + "/third_party/litehtml/containers/test/fonts/ahem.ttf");
        if(!ahem.empty())
        {
            lhu_register_font(ctx, "ahem", 400, 0, ahem.data(), static_cast<int32_t>(ahem.size()));
        }

        const auto arial = read_file("/System/Library/Fonts/Supplemental/Arial.ttf");
        if(!arial.empty())
        {
            lhu_register_font(ctx, "arial", 400, 0, arial.data(), static_cast<int32_t>(arial.size()));
            lhu_register_font(ctx, "sans-serif", 400, 0, arial.data(), static_cast<int32_t>(arial.size()));
        }

        const auto arial_bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
        if(!arial_bold.empty())
        {
            lhu_register_font(ctx, "arial", 700, 0, arial_bold.data(), static_cast<int32_t>(arial_bold.size()));
            lhu_register_font(ctx, "sans-serif", 700, 0, arial_bold.data(), static_cast<int32_t>(arial_bold.size()));
        }

        lhu_set_default_font(ctx, use_ahem ? "ahem" : "sans-serif", 16.f);
    }

    ~Fixture() { lhu_destroy(ctx); }

    LhuFrame render(const char* html, float width, float height = 600.f)
    {
        lhu_set_viewport(ctx, width, height);

        if(!lhu_load_html(ctx, html, nullptr))
        {
            std::printf("  load_html failed: %s\n", lhu_last_error(ctx));
        }

        lhu_layout(ctx, width);

        LhuFrame f {};
        lhu_record(ctx, &f);
        return f;
    }

    LhuFrame rerecord()
    {
        LhuFrame f {};
        lhu_record(ctx, &f);
        return f;
    }
};

//
// Tests
//

void test_abi()
{
    std::printf("\n[abi]\n");
    std::printf("  version: %s, sizeof(LhuQuad) = %d\n", lhu_version(), lhu_quad_size());

    check(lhu_quad_size() == static_cast<int32_t>(sizeof(LhuQuad)), "quad size is self-consistent");
    check(lhu_quad_size() % 16 == 0, "quad size is 16-byte aligned");

    LhuContext* ctx = lhu_create(nullptr);
    check(ctx != nullptr, "context creation");
    check(lhu_load_html(ctx, "<p>x</p>", nullptr) == 0, "load without fonts is rejected, not a crash");
    lhu_destroy(ctx);
}

void test_solid_rect()
{
    std::printf("\n[solid rect]\n");

    Fixture fx;
    auto    frame = fx.render(
        "<body style='margin:0'><div style='width:100px;height:50px;background:#ff0000'></div></body>", 400);

    const LhuQuad* r = first_of(frame, LHU_QUAD_RECT);
    check(r != nullptr, "a RECT quad was produced");
    if(!r)
    {
        return;
    }

    check_near(r->x, 0.f, 0.01f, "rect x");
    check_near(r->y, 0.f, 0.01f, "rect y");
    check_near(r->w, 100.f, 0.01f, "rect width");
    check_near(r->h, 50.f, 0.01f, "rect height");
    check(r->color == 0xFF0000FFu, "rect colour is opaque red (RGBA packed)");

    uint8_t px[4];
    sample_pixel(frame, 400, 200, 50, 25, px);
    check(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255, "centre pixel rasterizes to red");

    sample_pixel(frame, 400, 200, 200, 25, px);
    check(px[3] == 0, "pixel outside the div stays empty");
}

void test_ahem_metrics()
{
    std::printf("\n[text metrics: ahem]\n");

    // Every Ahem glyph is exactly 1em wide and fills the box from -0.8em to
    // +0.2em around the baseline, which makes layout assertions exact.
    Fixture fx;
    auto    frame = fx.render("<body style='margin:0'><div style=\"font:100px ahem;line-height:1\">AA</div></body>",
                              400);

    check_near(static_cast<float>(count_of(frame, LHU_QUAD_GLYPH)), 2.f, 0.f, "two glyph quads");

    const LhuQuad* g0 = nullptr;
    const LhuQuad* g1 = nullptr;
    for(int i = 0; i < frame.quad_count; ++i)
    {
        if(frame.quads[i].type != LHU_QUAD_GLYPH)
        {
            continue;
        }
        if(!g0)
        {
            g0 = &frame.quads[i];
        }
        else if(!g1)
        {
            g1 = &frame.quads[i];
        }
    }

    if(g0 && g1)
    {
        check_near(g0->x, 0.f, 1.f, "first glyph starts at x=0");
        check_near(g1->x - g0->x, 100.f, 1.f, "advance is exactly 1em (100px)");
        check_near(g0->w, 100.f, 1.f, "glyph box is 1em wide");
        check_near(g0->h, 100.f, 1.f, "glyph box is 1em tall");
    }
}

void test_borders()
{
    std::printf("\n[borders]\n");

    Fixture fx;
    auto    frame = fx.render("<body style='margin:0'><div style='width:100px;height:60px;"
                              "border-top:4px solid #ff0000;border-right:8px solid #00ff00;"
                              "border-bottom:12px solid #0000ff;border-left:16px solid #ffff00;"
                              "border-radius:10px'></div></body>",
                              400);

    check(count_of(frame, LHU_QUAD_BORDER) == 4, "one quad per visible edge");

    bool seen[4] = {false, false, false, false};
    for(int i = 0; i < frame.quad_count; ++i)
    {
        const LhuQuad& q = frame.quads[i];
        if(q.type != LHU_QUAD_BORDER)
        {
            continue;
        }

        const int e = static_cast<int>(q.params[0]);
        if(e >= 0 && e < 4)
        {
            seen[e] = true;
        }

        if(e == LHU_EDGE_TOP)
        {
            check(q.color == 0xFF0000FFu, "top edge is red");
            check_near(q.border[1], 4.f, 0.01f, "top width");
            check_near(q.border[0], 16.f, 0.01f, "left width recorded on every edge quad");
            check_near(q.rx[0], 10.f, 0.01f, "corner radius reaches the quad");
        }
    }

    check(seen[0] && seen[1] && seen[2] && seen[3], "all four edges present");

    // The miter split must assign each corner region to exactly one edge.
    uint8_t px[4];
    sample_pixel(frame, 200, 120, 50, 1, px);
    check(px[0] > 200 && px[1] < 60, "top strip rasterizes red");

    sample_pixel(frame, 200, 120, 2, 30, px);
    check(px[0] > 200 && px[1] > 200 && px[2] < 60, "left strip rasterizes yellow");

    // box-sizing is content-box, so the border box is 124 x 76 and the bottom
    // 12px band runs from y=64 to y=76.
    sample_pixel(frame, 200, 120, 50, 70, px);
    check(px[2] > 200 && px[0] < 60, "bottom strip rasterizes blue");

    sample_pixel(frame, 200, 120, 120, 40, px);
    check(px[1] > 200 && px[0] < 60 && px[2] < 60, "right strip rasterizes green");
}

void test_hover()
{
    std::printf("\n[hover / :hover CSS]\n");

    Fixture fx;
    auto    frame = fx.render("<body style='margin:0'><style>"
                              "#b{width:100px;height:100px;background:#ff0000}"
                              "#b:hover{background:#0000ff}"
                              "</style><div id='b'></div></body>",
                              400);

    const LhuQuad* r = first_of(frame, LHU_QUAD_RECT);
    check(r && r->color == 0xFF0000FFu, "starts red");

    const int changed = lhu_mouse_move(fx.ctx, 50.f, 50.f);
    check(changed == 1, "mouse move over the element reports a change");

    lhu_layout(fx.ctx, 400.f);
    auto           hovered = fx.rerecord();
    const LhuQuad* r2      = first_of(hovered, LHU_QUAD_RECT);
    // #0000ff packs as R=0, G=0, B=255, A=255.
    check(r2 && r2->color == 0xFFFF0000u, "turns blue while hovered");

    lhu_mouse_move(fx.ctx, 300.f, 300.f);
    lhu_layout(fx.ctx, 400.f);
    auto           left = fx.rerecord();
    const LhuQuad* r3   = first_of(left, LHU_QUAD_RECT);
    check(r3 && r3->color == 0xFF0000FFu, "returns to red after leaving");
}

void test_clip()
{
    std::printf("\n[overflow clipping]\n");

    Fixture fx;
    auto    frame = fx.render("<body style='margin:0'>"
                              "<div style='width:60px;height:60px;overflow:hidden'>"
                              "<div style='width:200px;height:200px;background:#00ff00'></div>"
                              "</div></body>",
                              400);

    bool any_clipped = false;
    for(int i = 0; i < frame.quad_count; ++i)
    {
        if(frame.quads[i].type == LHU_QUAD_RECT && frame.quads[i].clip_w >= 0.f)
        {
            any_clipped = true;
            check_near(frame.quads[i].clip_w, 60.f, 0.01f, "clip width follows the overflow box");
        }
    }
    check(any_clipped, "the inner fill carries a clip rect");

    uint8_t px[4];
    sample_pixel(frame, 300, 300, 30, 30, px);
    check(px[1] > 200, "inside the clip is painted");

    sample_pixel(frame, 300, 300, 100, 100, px);
    check(px[3] == 0, "outside the clip is not painted");
}

void test_gradient()
{
    std::printf("\n[gradients]\n");

    Fixture fx;
    auto    frame = fx.render("<body style='margin:0'><div style='width:200px;height:100px;"
                              "background:linear-gradient(to right,#ff0000,#0000ff)'></div></body>",
                              400);

    check(count_of(frame, LHU_QUAD_LINEAR_GRAD) == 1, "one linear-gradient quad");
    check(frame.grad_lut_rows >= 1, "a LUT row was baked");

    const LhuQuad* g = first_of(frame, LHU_QUAD_LINEAR_GRAD);
    if(g)
    {
        check(g->grad_row == 0, "quad points at LUT row 0");
    }

    uint8_t px[4];
    sample_pixel(frame, 300, 200, 3, 50, px);
    check(px[0] > 230 && px[2] < 30, "left end is red");

    sample_pixel(frame, 300, 200, 196, 50, px);
    check(px[2] > 230 && px[0] < 30, "right end is blue");
}

// Radial and conic gradients were only ever exercised by eye; this pins their
// actual geometry down.
void test_radial_conic()
{
    std::printf("\n[radial / conic gradients]\n");

    const char* forms[] = {
        "radial-gradient(#ff0000,#0000ff)",
        "radial-gradient(circle,#ff0000,#0000ff)",
        "radial-gradient(circle 40px at 50% 50%,#ff0000,#0000ff)",
        "radial-gradient(circle closest-side at 50% 50%,#ff0000,#0000ff)",
    };

    for(const char* form : forms)
    {
        Fixture     fx;
        std::string html = std::string("<body style='margin:0'><div style='width:100px;height:100px;background:") +
                           form + "'></div></body>";

        auto frame = fx.render(html.c_str(), 200);

        const LhuQuad* g = first_of(frame, LHU_QUAD_RADIAL_GRAD);
        std::printf("  %-52s quads=%d radial=%s", form, frame.quad_count, g ? "yes" : "NO");

        if(g)
        {
            std::printf(" centre=(%.0f,%.0f) radius=(%.0f,%.0f)", g->params[0], g->params[1], g->params[2],
                        g->params[3]);
        }
        std::printf("\n");

        check(g != nullptr, std::string("recorded as a radial gradient: ") + form);

        if(g)
        {
            check(g->params[2] > 1.f && g->params[3] > 1.f,
                  std::string("radius is usable (not zero): ") + form);

            uint8_t px[4];
            sample_pixel(frame, 200, 200, 50, 50, px);
            check(px[0] > 200 && px[2] < 60, std::string("centre is red: ") + form);
        }
    }

    {
        Fixture fx;
        auto    frame = fx.render("<body style='margin:0'><div style='width:100px;height:100px;"
                                  "background:conic-gradient(from 0deg,#ff0000,#00ff00,#0000ff,#ff0000)'></div></body>",
                                  200);

        const LhuQuad* c = first_of(frame, LHU_QUAD_CONIC_GRAD);
        check(c != nullptr, "conic gradient recorded");

        if(c)
        {
            std::printf("  conic centre=(%.0f,%.0f) angle=%.0f radius=%.0f\n", c->params[0], c->params[1],
                        c->params[2], c->params[3]);

            uint8_t px[4];
            sample_pixel(frame, 200, 200, 50, 10, px);
            check(px[0] > 150, "12 o'clock is at the start of the ramp (red)");
        }
    }
}

// Reproduces the reported failure: text vanishes after the page has been
// rebuilt for a while.
void test_glyph_cache_churn()
{
    std::printf("\n[glyph cache under rebuild churn]\n");

    Fixture fx(false);

    const char* html = "<body style='margin:0;font-family:sans-serif'>"
                       "<h1>Baslik</h1><p>Bir miktar govde metni, birkac kelime.</p>"
                       "<p style='font-size:12px'>Kucuk yazi</p></body>";

    int  first_glyphs = 0;
    int  first_atlas  = 0;
    bool text_lost    = false;
    int  lost_at      = -1;

    for(int i = 0; i < 400; ++i)
    {
        auto frame = fx.render(html, 400);

        const int glyphs = count_of(frame, LHU_QUAD_GLYPH);

        if(i == 0)
        {
            first_glyphs = glyphs;
            first_atlas  = frame.font_atlas_w * frame.font_atlas_h;
        }

        if(glyphs < first_glyphs && !text_lost)
        {
            text_lost = true;
            lost_at   = i;
            std::printf("  rebuild %d: glyph count fell %d -> %d, atlas %dx%d\n", i, first_glyphs, glyphs,
                        frame.font_atlas_w, frame.font_atlas_h);
        }

        if(i == 399)
        {
            std::printf("  after 400 rebuilds: %d glyphs, atlas %dx%d (started %dx%d)\n", glyphs,
                        frame.font_atlas_w, frame.font_atlas_h, 512, 512);

            check(!text_lost, "text still renders after 400 rebuilds");
            check(glyphs == first_glyphs, "glyph count is stable across rebuilds");
            check(frame.font_atlas_w * frame.font_atlas_h <= first_atlas * 4,
                  "atlas does not grow without bound when fonts are recreated");
        }
    }

    if(text_lost)
    {
        std::printf("  (text was first lost at rebuild %d)\n", lost_at);
    }
}

// The kerning cache (FontManager::kern_px) memoizes the single most expensive
// step in text measurement. It is keyed on typeface + code-point pair and
// stores the raw font-unit advance, so one entry must serve every pixel size of
// that face without the sizes contaminating each other. Getting that wrong
// would not crash -- it would silently shift text by a fraction of a pixel and
// drift the whole document's geometry, which is exactly the class of bug this
// checks for.
void test_kern_cache()
{
    std::printf("\n[kerning cache]\n");

    lhu::FontManager fonts;

    const auto regular = read_file(g_root + "/tests/fonts/Arial.ttf");
    std::vector<uint8_t> data = regular;
    if(data.empty())
    {
        data = read_file("/System/Library/Fonts/Supplemental/Arial.ttf");
    }
    if(data.empty())
    {
        std::printf("  SKIP (no Arial available)\n");
        return;
    }

    check(fonts.register_font("sans-serif", 400, false, data.data(), data.size()), "test font registered");
    fonts.set_default_family("sans-serif");

    // An uncached reference walk, built only out of glyph() and the free kern()
    // function, so it shares no code with the cache it is checking.
    auto reference_width = [&](lhu::Font* f, const char* utf8) {
        float    pen  = 0.f;
        uint32_t prev = 0;
        for(const char* p = utf8; *p;)
        {
            uint32_t cp = 0;
            p += lhu::utf8_decode(p, &cp);
            if(prev)
            {
                pen += lhu::kern(f, prev, cp);
            }
            pen += fonts.glyph(f, cp).advance;
            prev = cp;
        }
        return pen;
    };

    // Strings chosen for pairs Arial actually kerns: AV, VA, To, Ya, We.
    const char* samples[] = {"AV", "VA", "To", "Yardim", "Weapon", "AVATAR", "Esya 1", "x40",
                             "Cozunurluk", "1920x1080", "HP 84 / 100", "W"};
    const float sizes[]   = {11.f, 13.f, 14.f, 21.f, 22.f, 26.f, 48.f};

    // --- 1. cached == uncached, for every string at every size ---------------
    int mismatches = 0;
    for(float size : sizes)
    {
        lhu::Font* f = fonts.create_font("sans-serif", size, 400, false, 0, 0, 0, 0.f);
        for(const char* sample : samples)
        {
            const float cached = fonts.text_width(f, sample);
            const float ref    = reference_width(f, sample);
            if(cached != ref)
            {
                ++mismatches;
                std::printf("  '%s' @%.0fpx: cached %.6f vs uncached %.6f\n", sample, size, cached, ref);
            }
        }
        fonts.destroy_font(f);
    }
    check(mismatches == 0, "cached width is bit-identical to the uncached reference (84 cases)");

    // --- 2. the same string at two sizes must not collide --------------------
    lhu::Font* small = fonts.create_font("sans-serif", 13.f, 400, false, 0, 0, 0, 0.f);
    lhu::Font* big   = fonts.create_font("sans-serif", 26.f, 400, false, 0, 0, 0, 0.f);

    const char* kerned = "AVATAR";

    // Measure small first, so the cache is populated by the 13 px font and the
    // 26 px measurement is served entirely from entries another size created.
    const float w_small  = fonts.text_width(small, kerned);
    const float w_big    = fonts.text_width(big, kerned);
    const float w_small2 = fonts.text_width(small, kerned); // back again, all hits

    check(w_small != w_big, "same string at 13px and 26px does not return the same width");
    check(w_small == w_small2, "13px width is reproduced exactly after the 26px measurement");
    check_near(w_big / w_small, 2.f, 0.001f, "26px is exactly twice 13px (scale applied, not cached)");
    check(w_big == reference_width(big, kerned), "26px width matches the uncached reference");
    check(w_small == reference_width(small, kerned), "13px width matches the uncached reference");

    // Interleaving in the other order must be just as stable.
    const float b1 = fonts.text_width(big, "To Ya We");
    const float s1 = fonts.text_width(small, "To Ya We");
    const float b2 = fonts.text_width(big, "To Ya We");
    check(b1 == b2 && b1 != s1, "interleaved 26px/13px measurements stay independent");

    // --- 3. the pair actually is kerned, or the test proves nothing ----------
    const float kerned_w   = fonts.text_width(small, "AV");
    const float unkerned_w = fonts.glyph(small, 'A').advance + fonts.glyph(small, 'V').advance;
    check(kerned_w != unkerned_w, "'AV' is genuinely kerned in this font (test is not vacuous)");

    // --- 4. survives cache overflow -----------------------------------------
    const float before = fonts.text_width(small, "AVATAR");

    // Push far more distinct pairs through than the cache can hold, forcing at
    // least one clear, then re-measure.
    for(uint32_t left = 0x4E00; left < 0x4E00 + 200; ++left)
    {
        for(uint32_t right = 0x4E00; right < 0x4E00 + 200; ++right)
        {
            fonts.kern_px(small, left, right);
        }
    }

    const float after = fonts.text_width(small, "AVATAR");
    check(before == after, "width unchanged after the kern cache overflowed and cleared");
    check(fonts.kern_cache_size() <= 16384, "kern cache stays within its cap");
    std::printf("  kern cache holds %zu entries after 40000 distinct pairs\n", fonts.kern_cache_size());

    // --- 5. registering another font does not corrupt existing entries -------
    const auto bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
    if(!bold.empty())
    {
        const float pre = fonts.text_width(small, "AVATAR");
        fonts.register_font("sans-serif", 700, false, bold.data(), bold.size());
        const float post = fonts.text_width(small, "AVATAR");
        check(pre == post, "re-registering fonts does not change an existing measurement");

        lhu::Font* boldf = fonts.create_font("sans-serif", 13.f, 700, false, 0, 0, 0, 0.f);
        check(fonts.text_width(boldf, "AVATAR") == reference_width(boldf, "AVATAR"),
              "a second typeface gets its own kerning, not the first one's");
        fonts.destroy_font(boldf);
    }

    fonts.destroy_font(small);
    fonts.destroy_font(big);
}

void test_demo_page()
{
    std::printf("\n[demo page]\n");

    const char* html = R"HTML(
<html><head><style>
  body { margin:0; background:#12141c; font-family:arial; color:#e6e9f0; }
  .card { margin:24px; padding:20px 24px; border-radius:14px;
          background:linear-gradient(135deg,#1e2333,#2a3350);
          border:1px solid #3d4870; }
  h1 { font-size:28px; margin:0 0 6px 0; color:#ffffff; }
  .sub { font-size:14px; color:#8e97b3; margin:0 0 18px 0; }
  .row { margin-bottom:10px; }
  .pill { display:inline-block; padding:5px 14px; border-radius:999px;
          background:#3b82f6; color:#fff; font-size:13px; }
  .pill.warn { background:#f59e0b; }
  .pill.bad  { background:#ef4444; }
  .bar { height:10px; border-radius:5px; background:#232a3d; margin-top:6px; }
  .bar > i { display:block; height:10px; border-radius:5px;
             background:linear-gradient(to right,#22d3ee,#3b82f6); }
  table { width:100%; border-collapse:collapse; margin-top:16px; font-size:13px; }
  th,td { text-align:left; padding:7px 8px; border-bottom:1px solid #2c3450; }
  th { color:#8e97b3; font-weight:normal; }
  a { color:#7dd3fc; }
</style></head><body>
  <div class="card">
    <h1>litehtml &rarr; Unity GPU</h1>
    <p class="sub">CPU'da layout, GPU'da SDF ile cizim. Tek mesh, tek draw call.</p>

    <div class="row">
      <span class="pill">layout</span>
      <span class="pill warn">gradient</span>
      <span class="pill bad">border</span>
    </div>

    <div class="bar"><i style="width:72%"></i></div>

    <table>
      <tr><th>Ozellik</th><th>Durum</th></tr>
      <tr><td>Yuvarlak kose + kenarlik</td><td>SDF</td></tr>
      <tr><td>Metin</td><td>stb_truetype atlas</td></tr>
      <tr><td>Gradient</td><td>LUT dokusu</td></tr>
      <tr><td>Kirpma</td><td>Yuvarlak dikdortgen</td></tr>
    </table>

    <p class="sub" style="margin-top:16px;margin-bottom:0">
      <a href="https://github.com/litehtml/litehtml">litehtml</a> BSD-3-Clause
    </p>
  </div>
</body></html>)HTML";

    // The same file drives the Unity capture, so the two renderers are
    // compared on identical input.
    const auto shared = read_file(g_root + "/tests/demo.html");
    const std::string source(shared.begin(), shared.end());

    Fixture fx(false);
    auto    frame = fx.render(source.empty() ? html : source.c_str(), 560.f, 620.f);

    std::printf("  document: %.0f x %.0f, %d quads\n", frame.doc_width, frame.doc_height, frame.quad_count);
    std::printf("  atlas: %dx%d (v%d), gradient rows: %d\n", frame.font_atlas_w, frame.font_atlas_h,
                frame.font_atlas_version, frame.grad_lut_rows);

    int counts[8] = {0};
    for(int i = 0; i < frame.quad_count; ++i)
    {
        if(frame.quads[i].type >= 0 && frame.quads[i].type < 8)
        {
            ++counts[frame.quads[i].type];
        }
    }
    for(int t = 0; t < 7; ++t)
    {
        if(counts[t])
        {
            std::printf("    %-7s %d\n", type_name(t), counts[t]);
        }
    }

    check(frame.quad_count > 100, "the page produced a substantial quad stream");
    check(counts[LHU_QUAD_GLYPH] > 100, "text was laid out");
    check(counts[LHU_QUAD_LINEAR_GRAD] >= 2, "both gradients recorded");
    check(counts[LHU_QUAD_BORDER] > 0, "borders recorded");

    write_png(g_out_dir + "/demo.png", frame, static_cast<int>(frame.doc_width),
              static_cast<int>(frame.doc_height) + 24, 0xFF1C1412u);

    // The font atlas itself, useful when debugging glyph packing.
    if(frame.font_atlas_pixels)
    {
        std::vector<uint8_t> atlas(static_cast<size_t>(frame.font_atlas_w) * frame.font_atlas_h * 4);
        for(size_t i = 0; i < static_cast<size_t>(frame.font_atlas_w) * frame.font_atlas_h; ++i)
        {
            const uint8_t v  = frame.font_atlas_pixels[i];
            atlas[i * 4 + 0] = v;
            atlas[i * 4 + 1] = v;
            atlas[i * 4 + 2] = v;
            atlas[i * 4 + 3] = 255;
        }
        lodepng::encode(g_out_dir + "/atlas.png", atlas, static_cast<unsigned>(frame.font_atlas_w),
                        static_cast<unsigned>(frame.font_atlas_h));
        std::printf("  -> %s/atlas.png\n", g_out_dir.c_str());
    }
}

// A host re-sends the viewport on every layout; only a real change may restyle.
//
// Regression: making lhu_set_viewport mark styles stale unconditionally turned
// every layout into a full restyle plus a render-tree rebuild, which quietly
// threw away every scroll offset in the document. It reproduces only when a
// scroll is followed by a layout, which is what a host does on the very next
// frame -- so the page scrolled and snapped straight back.
void test_scroll_survives_a_resent_viewport()
{
    std::printf("\n[scroll vs viewport]\n");

    std::string rows;
    for(int i = 0; i < 20; ++i)
    {
        rows += "<div id='r" + std::to_string(i) + "' style='height:30px'>row " + std::to_string(i) + "</div>";
    }

    const std::string html =
        "<body style='margin:0'><div style='height:150px;overflow:auto'>" + rows + "</div></body>";

    // Loaded first, sized second -- the order a host actually uses, since a
    // surface only learns its size after the page is already parsed. It matters:
    // it is the one order in which the first layout sees the viewport change,
    // and a restyle that rebuilt the render tree there left every scroll_view
    // reporting nothing to scroll.
    Fixture fx(false);
    fx.render(html.c_str(), 300.f, 600.f);

    lhu_set_viewport(fx.ctx, 300.f, 150.f);
    lhu_layout(fx.ctx, 300.f);

    const auto row_y = [&fx]() {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        return lhu_element_rect(fx.ctx, "#r5", &x, &y, &w, &h) ? y : -1.f;
    };

    check_near(row_y(), 150.f, 0.5f, "row 5 starts one viewport down");

    check(lhu_scroll(fx.ctx, 0.f, 40.f, 150.f, 75.f) > 0, "the list took the scroll");
    check_near(row_y(), 110.f, 0.5f, "and moved up by it");

    // Four frames of exactly what a host does: same viewport, then a layout.
    for(int frame = 0; frame < 4; ++frame)
    {
        lhu_set_viewport(fx.ctx, 300.f, 150.f);
        lhu_layout(fx.ctx, 300.f);
    }

    check_near(row_y(), 110.f, 0.5f, "and stayed there across four re-sent viewports");

    // A viewport that really did change still recomputes vw, which is the whole
    // reason the restyle exists.
    Fixture vw(false);
    vw.render("<body style='margin:0'><div id='box' style='width:50vw;height:10px'></div></body>", 800.f, 600.f);

    lhu_set_viewport(vw.ctx, 400.f, 600.f);
    lhu_layout(vw.ctx, 400.f);

    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    lhu_element_rect(vw.ctx, "#box", &x, &y, &w, &h);
    check_near(w, 200.f, 0.5f, "50vw follows a viewport that actually changed");
}

} // namespace

int main(int argc, char** argv)
{
    if(argc > 1)
    {
        g_out_dir = argv[1];
    }

    // The harness is launched from the build directory; walk back to Native/.
    const char* env_root = std::getenv("LHU_ROOT");
    g_root               = env_root ? env_root : ".";

    std::printf("Doctype native harness\n");
    std::printf("root: %s\n", g_root.c_str());

    test_abi();
    test_solid_rect();
    test_ahem_metrics();
    test_borders();
    test_hover();
    test_clip();
    test_gradient();
    test_radial_conic();
    test_glyph_cache_churn();
    test_kern_cache();
    test_scroll_survives_a_resent_viewport();
    test_demo_page();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
