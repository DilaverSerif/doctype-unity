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



// The frame diff behind partial redraw. Wrong-side errors here are not equal:
// an under-reported rect leaves stale pixels on screen, an over-reported one
// merely wastes fill -- so the tests pin containment (the changed element is
// inside the rect) and boundedness (the rect is nowhere near the whole page).
void test_dirty_region()
{
    std::printf("\n[dirty region]\n");

    const char* html = R"HTML(
<html><head><style>
  body { margin:0; background:#101418; }
  .row { height:40px; background:#222833; margin:8px; }
</style></head><body>
  <div class="row" id="r0"><span id="t0">alpha</span></div>
  <div class="row" id="r1"><span id="t1">bravo</span></div>
  <div class="row" id="r2"><span id="t2">charlie</span></div>
  <div class="row" id="r3"><span id="t3">delta</span></div>
  <div class="row" id="r4"><span id="t4">echo</span></div>
</body></html>)HTML";

    Fixture fx(false);
    lhu_set_viewport(fx.ctx, 400.f, 400.f);
    lhu_load_html(fx.ctx, html, nullptr);
    lhu_layout(fx.ctx, 400.f);

    LhuFrame f {};
    lhu_record(fx.ctx, &f);
    check(f.dirty_mode == LHU_DIRTY_MODE_FULL, "the first frame repaints everything");

    lhu_record(fx.ctx, &f);
    check(f.dirty_mode == LHU_DIRTY_MODE_NONE, "an unchanged frame reports nothing to do");

    // One text in the middle of the page changes, same glyph count.
    lhu_set_text(fx.ctx, "#t2", "quebec!");
    lhu_layout(fx.ctx, 400.f);
    lhu_record(fx.ctx, &f);

    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;
    lhu_element_rect(fx.ctx, "#t2", &rx, &ry, &rw, &rh);

    check(f.dirty_mode == LHU_DIRTY_MODE_RECT, "a text change reports a rect");
    check(f.dirty_x <= rx && f.dirty_y <= ry && f.dirty_x + f.dirty_w >= rx + rw &&
              f.dirty_y + f.dirty_h >= ry + rh,
          "the rect contains the changed element");
    check(f.dirty_h < 200.f, "and stays far from the whole page");
    check(f.dirty_y > 60.f, "rows above the change are outside it");

    // A text that changes glyph COUNT inserts quads. Pairwise diffing would
    // smear the dirt over everything after the insertion point; the
    // prefix/suffix diff must keep it local.
    lhu_set_text(fx.ctx, "#t1", "a much longer line than before");
    lhu_layout(fx.ctx, 400.f);
    lhu_record(fx.ctx, &f);

    lhu_element_rect(fx.ctx, "#t1", &rx, &ry, &rw, &rh);
    check(f.dirty_mode == LHU_DIRTY_MODE_RECT, "a glyph-count change still reports a rect");
    check(f.dirty_y <= ry && f.dirty_y + f.dirty_h >= ry + rh, "which contains the reflowed text");
    check(f.dirty_h < 200.f, "without smearing over the rows after it");

    // A hover recolour goes through the cache's styles_changed path.
    lhu_mouse_move(fx.ctx, 200.f, 30.f);
    lhu_layout(fx.ctx, 400.f);
    lhu_record(fx.ctx, &f);
    check(f.dirty_mode != LHU_DIRTY_MODE_FULL, "hover does not repaint the page");

    // A viewport change moves everything and must not pretend otherwise.
    lhu_set_viewport(fx.ctx, 380.f, 400.f);
    lhu_layout(fx.ctx, 380.f);
    lhu_record(fx.ctx, &f);
    check(f.dirty_mode == LHU_DIRTY_MODE_FULL, "a resize repaints everything");
}

// The scroll fast path: a frame that is the previous one translated must say
// so, and the host recipe -- copy the region, repaint the strip -- must land on
// exactly the pixels a full repaint would have produced. The pixel comparison
// is the load-bearing check; everything else pins when the path may NOT fire.
void test_scroll_partial_redraw()
{
    std::printf("\n[scroll partial redraw]\n");

    const auto raster = [](const LhuFrame& f, int w, int h, uint32_t bg) {
        std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 4);
        for(size_t i = 0; i < fb.size(); i += 4)
        {
            fb[i + 0] = static_cast<uint8_t>(bg & 0xFF);
            fb[i + 1] = static_cast<uint8_t>((bg >> 8) & 0xFF);
            fb[i + 2] = static_cast<uint8_t>((bg >> 16) & 0xFF);
            fb[i + 3] = static_cast<uint8_t>((bg >> 24) & 0xFF);
        }
        lhu_raster::Textures tex;
        tex.font      = f.font_atlas_pixels;
        tex.font_w    = f.font_atlas_w;
        tex.font_h    = f.font_atlas_h;
        tex.grad      = f.grad_lut_pixels;
        tex.grad_w    = f.grad_lut_w;
        tex.grad_rows = f.grad_lut_rows;
        for(int i = 0; i < f.quad_count; ++i)
        {
            lhu_raster::draw_quad(fb, w, h, f.quads[i], tex);
        }
        return fb;
    };

    // What the Unity renderer does with a SCROLL frame, on the CPU: fill the
    // destination rect from the retained image translated, then take both
    // dirty rects' pixels from a full repaint (identical to a scissored
    // clear+draw).
    const auto apply_scroll = [](const std::vector<uint8_t>& prev, const std::vector<uint8_t>& repaint, int w, int h,
                                 const LhuFrame& f) {
        std::vector<uint8_t> fb = prev;

        const int dx0 = std::max(0, static_cast<int>(f.scroll_x));
        const int dy0 = std::max(0, static_cast<int>(f.scroll_y));
        const int dx1 = std::min(w, static_cast<int>(f.scroll_x + f.scroll_w));
        const int dy1 = std::min(h, static_cast<int>(f.scroll_y + f.scroll_h));
        const int tx  = static_cast<int>(f.scroll_dx);
        const int ty  = static_cast<int>(f.scroll_dy);

        for(int y = dy0; y < dy1; ++y)
        {
            for(int x = dx0; x < dx1; ++x)
            {
                const size_t dst = (static_cast<size_t>(y) * w + x) * 4;
                const size_t src = (static_cast<size_t>(y - ty) * w + (x - tx)) * 4;
                std::memcpy(&fb[dst], &prev[src], 4);
            }
        }

        const auto repaint_rect = [&](float fx, float fy, float fw, float fh) {
            const int rx0 = std::max(0, static_cast<int>(fx));
            const int ry0 = std::max(0, static_cast<int>(fy));
            const int rx1 = std::min(w, static_cast<int>(std::ceil(fx + fw)));
            const int ry1 = std::min(h, static_cast<int>(std::ceil(fy + fh)));
            for(int y = ry0; y < ry1; ++y)
            {
                std::memcpy(&fb[(static_cast<size_t>(y) * w + rx0) * 4],
                            &repaint[(static_cast<size_t>(y) * w + rx0) * 4], static_cast<size_t>(rx1 - rx0) * 4);
            }
        };

        repaint_rect(f.dirty_x, f.dirty_y, f.dirty_w, f.dirty_h);
        if(f.dirty2_w > 0.f && f.dirty2_h > 0.f)
        {
            repaint_rect(f.dirty2_x, f.dirty2_y, f.dirty2_w, f.dirty2_h);
        }

        return fb;
    };

    std::string rows;
    for(int i = 0; i < 30; ++i)
    {
        rows += "<div style='height:30px'>row " + std::to_string(i) + "</div>";
    }

    const std::string html = "<body style='margin:0;background:#101418'>"
                             "<div style='height:40px;background:#222833'>header</div>"
                             "<div id='list' style='height:200px;overflow:auto;background:#181d26'>" +
                             rows +
                             "</div>"
                             "<div style='height:40px;background:#222833'>footer</div></body>";

    const int W = 300, H = 400;
    const uint32_t BG = 0xFF000000u;

    Fixture fx;
    LhuFrame f = fx.render(html.c_str(), static_cast<float>(W), static_cast<float>(H));
    std::vector<uint8_t> prev = raster(f, W, H, BG);

    // A scroll, recorded the way the host records it: no layout in between.
    check(lhu_scroll(fx.ctx, 0.f, 40.f, 150.f, 140.f) > 0, "the list took the scroll");
    f = fx.rerecord();

    check(f.dirty_mode == LHU_DIRTY_MODE_SCROLL, "a pure scroll reports a translation");
    check_near(f.scroll_dy, -40.f, 0.01f, "content moved up by the scrolled amount");
    check_near(f.scroll_dx, 0.f, 0.01f, "and did not move sideways");
    // The copy destination is the scrolled window minus the strip that
    // scrolled in and the whole-pixel margins at both edges.
    check(f.scroll_h > 120.f && f.scroll_h <= 200.5f, "the copy spans most of the scrolled window");
    check(f.dirty_h < 60.f, "the strip is the entered band plus padding, not the viewport");
    check(f.dirty_y > f.scroll_y + f.scroll_h - 60.f, "and sits at the bottom edge, where content entered");

    std::vector<uint8_t> full = raster(f, W, H, BG);
    std::vector<uint8_t> sim  = apply_scroll(prev, full, W, H, f);
    check(sim == full, "copy + strip repaint reproduces the full repaint byte for byte");
    prev = std::move(sim);

    // Scrolling back moves the strip to the top.
    check(lhu_scroll(fx.ctx, 0.f, -40.f, 150.f, 140.f) > 0, "the list scrolled back");
    f = fx.rerecord();
    check(f.dirty_mode == LHU_DIRTY_MODE_SCROLL, "the way back is a translation too");
    check_near(f.scroll_dy, 40.f, 0.01f, "content moved down");
    check(f.dirty_y < f.scroll_y + 60.f, "the strip is at the top edge now");

    full = raster(f, W, H, BG);
    sim  = apply_scroll(prev, full, W, H, f);
    check(sim == full, "the upward copy is exact too");

    // Two scrolls that cancel are byte-identical frames.
    lhu_scroll(fx.ctx, 0.f, 40.f, 150.f, 140.f);
    lhu_scroll(fx.ctx, 0.f, -40.f, 150.f, 140.f);
    f = fx.rerecord();
    check(f.dirty_mode == LHU_DIRTY_MODE_NONE, "a scroll that nets to zero repaints nothing");

    // A hover restyle in the same frame is not a translation; the diff must
    // notice and fall back rather than copy a recoloured row around.
    {
        std::string hover_rows;
        for(int i = 0; i < 30; ++i)
        {
            hover_rows += "<div class='r' style='height:30px'>row " + std::to_string(i) + "</div>";
        }
        const std::string hover_html = "<body style='margin:0;background:#101418'>"
                                       "<style>.r:hover{background:#553311}</style>"
                                       "<div id='list' style='height:200px;overflow:auto'>" +
                                       hover_rows + "</div></body>";

        Fixture hv;
        hv.render(hover_html.c_str(), static_cast<float>(W), static_cast<float>(H));
        lhu_mouse_move(hv.ctx, 150.f, 100.f);
        hv.rerecord();

        lhu_mouse_move(hv.ctx, 150.f, 130.f); // hover moves a row...
        lhu_scroll(hv.ctx, 0.f, 40.f, 150.f, 100.f); // ...and the list scrolls
        f = hv.rerecord();
        check(f.dirty_mode != LHU_DIRTY_MODE_SCROLL, "hover + scroll in one frame is not sold as a translation");
    }

    // Content scrolling across a gradient background cannot be copied: the
    // pixels under the content change with position.
    {
        const std::string grad_html = "<body style='margin:0'>"
                                      "<div id='list' style='height:200px;overflow:auto;"
                                      "background:linear-gradient(#204060,#101418)'>" +
                                      rows + "</div></body>";
        Fixture gr;
        gr.render(grad_html.c_str(), static_cast<float>(W), static_cast<float>(H));
        lhu_scroll(gr.ctx, 0.f, 40.f, 150.f, 100.f);
        f = gr.rerecord();
        check(f.dirty_mode != LHU_DIRTY_MODE_SCROLL, "a gradient under the content disarms the copy");
    }

    // A list that fills the whole surface, scrolled without a layout call in
    // between -- exactly the page and flow the Unity play-mode test drives.
    {
        std::string prows;
        for(int i = 0; i < 30; ++i)
        {
            prows += std::string("<div style='height:30px;background:") + (i % 2 == 0 ? "#ff0000" : "#0000ff") +
                     "'></div>";
        }
        const std::string ph = "<body style='margin:0;background:#101418'>"
                               "<div id='list' style='height:300px;overflow:auto'>" +
                               prows + "</div></body>";
        Fixture pv;
        pv.render(ph.c_str(), 300.f, 300.f);
        check(lhu_scroll(pv.ctx, 0.f, 30.f, 150.f, 150.f) > 0, "the surface-sized list took the scroll");
        f = pv.rerecord();
        check(f.dirty_mode == LHU_DIRTY_MODE_SCROLL, "a surface-sized list is still a translation");
    }

    // Horizontal scrolling exercises the other axis of every interval above.
    {
        std::string cells;
        for(int i = 0; i < 30; ++i)
        {
            cells += "<div style='display:inline-block;width:60px;height:100px'>c" + std::to_string(i) + "</div>";
        }
        const std::string h_html = "<body style='margin:0;background:#101418'>"
                                   "<div id='strip' style='height:120px;overflow:auto;white-space:nowrap;"
                                   "background:#181d26'>" +
                                   cells + "</div></body>";
        Fixture hz;
        LhuFrame hf = hz.render(h_html.c_str(), static_cast<float>(W), static_cast<float>(H));
        std::vector<uint8_t> hprev = raster(hf, W, H, BG);

        check(lhu_scroll(hz.ctx, 40.f, 0.f, 150.f, 60.f) > 0, "the strip took a horizontal scroll");
        hf = hz.rerecord();
        check(hf.dirty_mode == LHU_DIRTY_MODE_SCROLL, "a horizontal scroll is a translation too");
        check_near(hf.scroll_dx, -40.f, 0.01f, "content moved left");
        check(hf.dirty_w < 60.f, "the strip is the entered column plus padding");

        std::vector<uint8_t> hfull = raster(hf, W, H, BG);
        std::vector<uint8_t> hsim  = apply_scroll(hprev, hfull, W, H, hf);
        check(hsim == hfull, "the horizontal copy is exact");
    }
}

// EXPERIMENT E8: subtree relayout. The proof style is A/B: two contexts fed
// the same call sequence, one answering mutations with subtree renders and
// one always rendering the document, must produce byte-identical quad
// streams at every step. Byte equality is the whole claim -- the fast path
// may only ever change how the answer is computed, never the answer.
void test_subtree_relayout()
{
    std::printf("\n[subtree relayout]\n");

    std::string rows;
    for(int i = 0; i < 12; ++i)
    {
        rows += "<div id='r" + std::to_string(i) + "' style='height:30px'><span id='t" + std::to_string(i) +
                "'>row " + std::to_string(i) + "</span></div>";
    }

    const std::string html = "<body style='margin:0;background:#101418'>"
                             "<div style='height:40px;background:#222833'>header</div>"
                             "<div id='list' style='height:200px;overflow:auto'>" +
                             rows +
                             "</div>"
                             "<div id='g'><span id='gt'>short</span></div>"
                             "<div style='height:40px;background:#222833'>footer</div></body>";

    Fixture on, off;
    lhu_exp_sublayout_set_enabled(off.ctx, 0);

    on.render(html.c_str(), 300.f, 400.f);
    off.render(html.c_str(), 300.f, 400.f);

    const auto step = [&](const char* what, const std::function<void(LhuContext*)>& mutate) {
        mutate(on.ctx);
        mutate(off.ctx);
        lhu_layout(on.ctx, 300.f);
        lhu_layout(off.ctx, 300.f);
        LhuFrame fa {}, fb {};
        lhu_record(on.ctx, &fa);
        lhu_record(off.ctx, &fb);
        const bool same = fa.quad_count == fb.quad_count &&
                          std::memcmp(fa.quads, fb.quads, sizeof(LhuQuad) * fa.quad_count) == 0 &&
                          fa.doc_height == fb.doc_height;
        check(same, what);
    };

    // A glyph-count change inside a fixed-height row: the case E1 cannot skip
    // and E8 exists for.
    step("a text of a different length lays out identically through the subtree",
         [](LhuContext* c) { lhu_set_text(c, "#t3", "different words here"); });

    step("a digit gaining a digit does too",
         [](LhuContext* c) { lhu_set_text(c, "#t5", "12345"); });

    // Same width in ahem: E1's skip answers on both contexts.
    step("a same-width change stays the skip it always was",
         [](LhuContext* c) { lhu_set_text(c, "#t5", "54321"); });

    // Text that wraps to a second line inside a FIXED-height row: content
    // overflows, the footprint does not, the subtree answer must still match.
    step("a wrap inside a fixed-height row matches",
         [](LhuContext* c) { lhu_set_text(c, "#t7", "words that will surely wrap onto another line"); });

    // An inline style edit, same confinement.
    step("an inline style recolour matches",
         [](LhuContext* c) { lhu_set_style(c, "#t4", "color:#ff4444"); });

    // A row that GROWS (auto height): the footprint moves, the subtree answer
    // is refused, and the fallback must still be byte-identical.
    step("a growing block falls back to the full render and matches",
         [](LhuContext* c) { lhu_set_text(c, "#gt", "now this text is long enough to wrap and grow the box"); });

    // A scroll in the same frame as a mutation: the scroll's invalidation
    // must win (it dirties more than one subtree).
    step("scroll plus mutation in one frame matches", [](LhuContext* c) {
        lhu_scroll(c, 0.f, 40.f, 150.f, 140.f);
        lhu_set_text(c, "#t2", "changed mid scroll");
    });

    int32_t subs = 0, falls = 0;
    lhu_exp_sublayout_stats(on.ctx, nullptr, &subs, &falls);
    check(subs >= 4, "the confined mutations were answered by subtree renders");
    check(falls >= 1, "and the grower fell back");

    int32_t subs_off = 0;
    lhu_exp_sublayout_stats(off.ctx, nullptr, &subs_off, nullptr);
    check(subs_off == 0, "the control context never took the fast path");

    // The dangerous shapes: ancestors that read intrinsic widths from below.
    // A flex row re-distributes when an item's natural width changes even if
    // the item's assigned box does not, so the guard must refuse it -- and
    // the A/B equality is what proves the guard is load-bearing.
    {
        const char* flex_html = "<body style='margin:0'>"
                                "<div style='display:flex'>"
                                "<div id='fa'>aa</div><div id='fb'>bb</div>"
                                "</div></body>";
        Fixture fon, foff;
        lhu_exp_sublayout_set_enabled(foff.ctx, 0);
        fon.render(flex_html, 300.f, 200.f);
        foff.render(flex_html, 300.f, 200.f);

        lhu_set_text(fon.ctx, "#fa", "aaaaaaaa");
        lhu_set_text(foff.ctx, "#fa", "aaaaaaaa");
        lhu_layout(fon.ctx, 300.f);
        lhu_layout(foff.ctx, 300.f);

        LhuFrame fa {}, fb {};
        lhu_record(fon.ctx, &fa);
        lhu_record(foff.ctx, &fb);
        check(fa.quad_count == fb.quad_count &&
                  std::memcmp(fa.quads, fb.quads, sizeof(LhuQuad) * fa.quad_count) == 0,
              "a flex ancestor is refused and the page still matches");

        int32_t fsubs = 0;
        lhu_exp_sublayout_stats(fon.ctx, nullptr, &fsubs, nullptr);
        check(fsubs == 0, "no subtree render fired under a flex ancestor");
    }

    // Floats leak across formatting contexts; a document with any float is
    // out wholesale.
    {
        const char* float_html = "<body style='margin:0'>"
                                 "<div style='float:left;width:80px;height:40px;background:#333'></div>"
                                 "<div id='fl'>beside the float</div></body>";
        Fixture fon;
        fon.render(float_html, 300.f, 200.f);
        lhu_set_text(fon.ctx, "#fl", "still beside, but longer now");
        lhu_layout(fon.ctx, 300.f);

        int32_t fsubs = 0;
        lhu_exp_sublayout_stats(fon.ctx, nullptr, &fsubs, nullptr);
        check(fsubs == 0, "a document with a float never takes the fast path");
    }

    // Scrolling still works after a subtree render: the list's scroll range
    // must reflect the tree the fast path left behind.
    {
        Fixture sv;
        sv.render(html.c_str(), 300.f, 400.f);
        lhu_set_text(sv.ctx, "#t1", "mutated before scrolling");
        lhu_layout(sv.ctx, 300.f);

        int32_t subs2 = 0;
        lhu_exp_sublayout_stats(sv.ctx, nullptr, &subs2, nullptr);
        check(subs2 == 1, "the mutation was answered by a subtree render");
        check(lhu_scroll(sv.ctx, 0.f, 60.f, 150.f, 140.f) > 0, "and the list still scrolls after it");

        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        lhu_element_rect(sv.ctx, "#r5", &x, &y, &w, &h);
        check_near(y, 40.f + 5 * 30.f - 60.f, 0.5f, "by the full sixty pixels");
    }

    // The host re-sends the device scale before every layout. An identical
    // value must change nothing -- the unguarded version invalidated the
    // layout and the whole quad cache once per frame, which quietly disarmed
    // every fast path on the device while the desktop probes stayed green.
    {
        Fixture ds;
        ds.render(html.c_str(), 300.f, 400.f);
        lhu_set_text(ds.ctx, "#t1", "mutated once");
        lhu_set_device_scale(ds.ctx, 1.f); // identical to the default
        lhu_layout(ds.ctx, 300.f);

        int32_t subs3 = 0;
        lhu_exp_sublayout_stats(ds.ctx, nullptr, &subs3, nullptr);
        check(subs3 == 1, "a re-sent identical device scale does not disarm the subtree render");
    }
}

// Input events must say WHAT they dirtied, not just that they did. A :hover
// that recolours reports paint; a :hover that resizes reports layout too. The
// host draws stale geometry if layout-dirty is under-reported, and pays a full
// layout per pointer twitch if it is over-reported -- so both directions are
// pinned, on the same page.
void test_input_dirty_flags()
{
    std::printf("\n[input dirty flags]\n");

    const char* html = R"HTML(
<html><head><style>
  body { margin:0; }
  div  { height:40px; }
  #paint { width:100px; background:#333; }
  #paint:hover { background:#f00; }
  #grow { width:100px; background:#333; }
  #grow:hover { width:200px; }
  #parent { width:100px; background:#333; }
  #child { width:50px; height:10px; }
  #parent:hover #child { height:30px; }
</style></head><body>
  <div id="paint"></div>
  <div id="grow"></div>
  <div id="parent"><div id="child"></div></div>
</body></html>)HTML";

    Fixture fx(false);
    fx.render(html, 400.f, 400.f);

    const auto width_of = [&fx](const char* sel) {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        lhu_element_rect(fx.ctx, sel, &x, &y, &w, &h);
        return w;
    };

    // Paint-only hover: over #paint (rows are 40px tall, stacked).
    int32_t flags = lhu_mouse_move(fx.ctx, 50.f, 20.f);
    check(flags == LHU_DIRTY_PAINT, "recolouring hover reports paint only");

    // Off to bare page (bottom of the viewport, below all rows).
    lhu_mouse_move(fx.ctx, 300.f, 390.f);

    // Geometry hover: over #grow.
    flags = lhu_mouse_move(fx.ctx, 50.f, 60.f);
    check((flags & LHU_DIRTY_LAYOUT) != 0, "resizing hover reports layout");

    lhu_layout(fx.ctx, 400.f);
    check_near(width_of("#grow"), 200.f, 0.5f, "and the new width is real after layout");

    // Leaving it undoes the resize, which is itself a layout change.
    flags = lhu_mouse_leave(fx.ctx);
    check((flags & LHU_DIRTY_LAYOUT) != 0, "leaving a resizing hover reports layout");

    lhu_layout(fx.ctx, 400.f);
    check_near(width_of("#grow"), 100.f, 0.5f, "and the width snaps back");

    // The descendant case: the rule fires on #parent's hover but moves #child.
    // compute_styles is recursive, so this is the case a naive per-element
    // check misses.
    flags = lhu_mouse_move(fx.ctx, 50.f, 100.f);
    check((flags & LHU_DIRTY_LAYOUT) != 0, "hover that resizes a descendant reports layout");

    lhu_layout(fx.ctx, 400.f);
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    lhu_element_rect(fx.ctx, "#child", &x, &y, &w, &h);
    check_near(h, 30.f, 0.5f, "and the descendant grew");
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

// No C++ exception may cross the C ABI: the real caller is a P/Invoke frame,
// and an escaped exception is undefined behaviour there. The harness cannot
// inject a throw into an arbitrary export, so this drives the entry points
// with the most throw-prone inputs the API accepts -- selector strings go
// straight into litehtml's CSS parser -- and asserts the calls come back as
// failures instead of not coming back at all. If none of these inputs happens
// to throw in a future litehtml, the checks still hold; the boundary macros
// in lhu_api.cpp are what actually guarantee containment.
void test_abi_exception_boundary()
{
    std::printf("\n[abi exception boundary]\n");

    Fixture fx(false);
    fx.render("<body><div id='a'>text</div></body>", 300.f, 300.f);

    const char* hostile[] = {"[", ":nth-child(", "a[href=", "::", "*::::*", ")("};

    bool survived = true;
    for(const char* sel : hostile)
    {
        if(lhu_set_text(fx.ctx, sel, "x") != 0 || lhu_set_style(fx.ctx, sel, "color:red") != 0)
        {
            survived = false;
        }
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if(lhu_element_rect(fx.ctx, sel, &x, &y, &w, &h) != 0)
        {
            survived = false;
        }
    }
    check(survived, "hostile selectors fail as return codes, not as crashes");

    // The context is still coherent afterwards: a real mutation works and a
    // record produces a frame.
    check(lhu_set_text(fx.ctx, "#a", "changed") == 1, "the context still accepts a real mutation");
    lhu_layout(fx.ctx, 300.f);
    const LhuFrame f = fx.rerecord();
    check(f.quad_count > 0, "and still records a frame");
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
    test_dirty_region();
    test_scroll_partial_redraw();
    test_subtree_relayout();
    test_input_dirty_flags();
    test_scroll_survives_a_resent_viewport();
    test_abi_exception_boundary();
    test_demo_page();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
