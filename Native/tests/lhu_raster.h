// Reference CPU rasterizer for the LhuQuad stream.
//
// This is NOT part of the runtime. It exists so that:
//   * the native test harness can produce a golden PNG without a GPU, and
//   * the shader has an executable specification to be checked against.
//
// Every function here is written to mirror LiteHtmlQuad.shader one-for-one. If
// you change the maths in one, change it in the other — LiteHtmlGoldenTests
// compares their outputs pixel by pixel.

#ifndef LHU_RASTER_H
#define LHU_RASTER_H

#include "lhu_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lhu_raster
{

struct Vec2
{
    float x = 0, y = 0;
};

struct RGBA
{
    float r = 0, g = 0, b = 0, a = 0;
};

inline RGBA unpack(uint32_t c)
{
    return RGBA {static_cast<float>(c & 0xFFu) / 255.f, static_cast<float>((c >> 8) & 0xFFu) / 255.f,
                 static_cast<float>((c >> 16) & 0xFFu) / 255.f, static_cast<float>((c >> 24) & 0xFFu) / 255.f};
}

// Signed distance to a rounded box, in pixels.
//
// `p` is relative to the box centre, `b` is the half-size, `r` the elliptical
// radius of the corner `p` falls into. Exact for circular corners; for
// elliptical ones the radial term is scaled by the smaller radius, which is a
// close approximation and keeps the function cheap.
inline float sd_round_box(Vec2 p, Vec2 b, float rx, float ry)
{
    rx = std::max(rx, 0.f);
    ry = std::max(ry, 0.f);

    const Vec2 q {std::fabs(p.x) - b.x + rx, std::fabs(p.y) - b.y + ry};

    const float mx = std::max(q.x, 0.f);
    const float my = std::max(q.y, 0.f);

    float outside;
    if(rx > 0.f && ry > 0.f)
    {
        const float ex = mx / rx;
        const float ey = my / ry;
        outside        = std::sqrt(ex * ex + ey * ey) * std::min(rx, ry);
    }
    else
    {
        outside = std::sqrt(mx * mx + my * my);
    }

    return std::min(std::max(q.x, q.y), 0.f) + outside - std::min(rx, ry);
}

// Picks the corner radius for the quadrant `p` lies in.
// Corner order is CSS order: top-left, top-right, bottom-right, bottom-left.
inline void pick_radius(const float rx[4], const float ry[4], Vec2 p, float* out_rx, float* out_ry)
{
    const int i = p.x > 0.f ? (p.y < 0.f ? 1 : 2) : (p.y < 0.f ? 0 : 3);
    *out_rx     = rx[i];
    *out_ry     = ry[i];
}

// Distance -> coverage. `fw` is the screen-space width of one pixel, which is
// 1.0 in the harness and fwidth(d) in the shader.
inline float coverage(float d, float fw = 1.f)
{
    return std::min(1.f, std::max(0.f, 0.5f - d / std::max(fw, 1e-6f)));
}

// Which border edge owns this point?
//
// Normalized penetration depth from each side; the smallest wins. This is
// exactly equivalent to splitting the ring along the corner-to-corner miter
// diagonals, but needs no line equations.
inline int owning_edge(Vec2 local, float w, float h, const float border[4])
{
    const float inf = 1e30f;

    const float t_left   = border[0] > 0.f ? local.x / border[0] : inf;
    const float t_top    = border[1] > 0.f ? local.y / border[1] : inf;
    const float t_right  = border[2] > 0.f ? (w - local.x) / border[2] : inf;
    const float t_bottom = border[3] > 0.f ? (h - local.y) / border[3] : inf;

    int   best   = LHU_EDGE_TOP;
    float best_t = t_top;

    if(t_right < best_t)
    {
        best   = LHU_EDGE_RIGHT;
        best_t = t_right;
    }
    if(t_bottom < best_t)
    {
        best   = LHU_EDGE_BOTTOM;
        best_t = t_bottom;
    }
    if(t_left < best_t)
    {
        best = LHU_EDGE_LEFT;
    }

    return best;
}

struct Textures
{
    const uint8_t* font   = nullptr; // R8
    int            font_w = 0, font_h = 0;

    const uint8_t* grad      = nullptr; // RGBA8
    int            grad_w    = 0, grad_rows = 0;

    const uint8_t* image   = nullptr; // RGBA8
    int            image_w = 0, image_h = 0;
};

inline float sample_r8(const uint8_t* px, int w, int h, float u, float v)
{
    if(!px || w <= 0 || h <= 0)
    {
        return 0.f;
    }

    // Bilinear, matching the GPU's default filtering.
    const float fx = u * w - 0.5f;
    const float fy = v * h - 0.5f;

    const int   x0 = static_cast<int>(std::floor(fx));
    const int   y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;

    auto at = [&](int x, int y) -> float {
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return px[static_cast<size_t>(y) * w + x] / 255.f;
    };

    const float a = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
    const float b = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
    return a * (1 - ty) + b * ty;
}

inline RGBA sample_grad(const Textures& tex, int row, float t)
{
    if(!tex.grad || row < 0 || row >= tex.grad_rows)
    {
        return RGBA {1, 1, 1, 1};
    }

    t             = std::min(1.f, std::max(0.f, t));
    const int   i = std::min(tex.grad_w - 1, static_cast<int>(t * (tex.grad_w - 1) + 0.5f));
    const uint8_t* p = tex.grad + (static_cast<size_t>(row) * tex.grad_w + i) * 4;

    return RGBA {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f};
}

// Rasterizes one quad into an RGBA8 buffer using straight source-over
// compositing in sRGB space, which is what a browser does.
inline void draw_quad(std::vector<uint8_t>& fb, int fb_w, int fb_h, const LhuQuad& q, const Textures& tex)
{
    // Bounding box, expanded by one pixel for antialiasing.
    int x0 = static_cast<int>(std::floor(q.x)) - 1;
    int y0 = static_cast<int>(std::floor(q.y)) - 1;
    int x1 = static_cast<int>(std::ceil(q.x + q.w)) + 1;
    int y1 = static_cast<int>(std::ceil(q.y + q.h)) + 1;

    if(q.clip_w >= 0.f)
    {
        x0 = std::max(x0, static_cast<int>(std::floor(q.clip_x)) - 1);
        y0 = std::max(y0, static_cast<int>(std::floor(q.clip_y)) - 1);
        x1 = std::min(x1, static_cast<int>(std::ceil(q.clip_x + q.clip_w)) + 1);
        y1 = std::min(y1, static_cast<int>(std::ceil(q.clip_y + q.clip_h)) + 1);
    }

    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, fb_w);
    y1 = std::min(y1, fb_h);

    const Vec2 centre {q.x + q.w * 0.5f, q.y + q.h * 0.5f};
    const Vec2 half {q.w * 0.5f, q.h * 0.5f};
    const RGBA tint = unpack(q.color);

    for(int py = y0; py < y1; ++py)
    {
        for(int px = x0; px < x1; ++px)
        {
            const Vec2 p {px + 0.5f, py + 0.5f};
            const Vec2 rel {p.x - centre.x, p.y - centre.y};

            float alpha = 1.f;
            RGBA  col   = tint;

            switch(q.type)
            {
            case LHU_QUAD_RECT:
            {
                float rx = 0, ry = 0;
                pick_radius(q.rx, q.ry, rel, &rx, &ry);
                alpha = coverage(sd_round_box(rel, half, rx, ry));
                break;
            }

            case LHU_QUAD_BORDER:
            {
                float rx = 0, ry = 0;
                pick_radius(q.rx, q.ry, rel, &rx, &ry);
                const float d_out = sd_round_box(rel, half, rx, ry);

                // Inner rounded rect = outer shrunk by the per-side widths.
                const float bl = q.border[0], bt = q.border[1], br = q.border[2], bb = q.border[3];

                const Vec2 in_centre {q.x + bl + (q.w - bl - br) * 0.5f, q.y + bt + (q.h - bt - bb) * 0.5f};
                const Vec2 in_half {std::max(0.f, (q.w - bl - br) * 0.5f), std::max(0.f, (q.h - bt - bb) * 0.5f)};

                const float in_rx[4] = {std::max(0.f, q.rx[0] - bl), std::max(0.f, q.rx[1] - br),
                                        std::max(0.f, q.rx[2] - br), std::max(0.f, q.rx[3] - bl)};
                const float in_ry[4] = {std::max(0.f, q.ry[0] - bt), std::max(0.f, q.ry[1] - bt),
                                        std::max(0.f, q.ry[2] - bb), std::max(0.f, q.ry[3] - bb)};

                const Vec2 in_rel {p.x - in_centre.x, p.y - in_centre.y};
                float      irx = 0, iry = 0;
                pick_radius(in_rx, in_ry, in_rel, &irx, &iry);
                const float d_in = sd_round_box(in_rel, in_half, irx, iry);

                // Inside the outer shape and outside the inner one.
                alpha = coverage(d_out) * coverage(-d_in);

                const int want = static_cast<int>(q.params[0]);
                if(want >= 0)
                {
                    const Vec2 local {p.x - q.x, p.y - q.y};
                    if(owning_edge(local, q.w, q.h, q.border) != want)
                    {
                        alpha = 0.f;
                    }
                }
                break;
            }

            case LHU_QUAD_GLYPH:
            {
                const float u = q.u0 + (p.x - q.x) / q.w * (q.u1 - q.u0);
                const float v = q.v0 + (p.y - q.y) / q.h * (q.v1 - q.v0);

                if(p.x < q.x || p.x > q.x + q.w || p.y < q.y || p.y > q.y + q.h)
                {
                    alpha = 0.f;
                }
                else
                {
                    alpha = sample_r8(tex.font, tex.font_w, tex.font_h, u, v);
                }
                break;
            }

            case LHU_QUAD_IMAGE:
            {
                float rx = 0, ry = 0;
                pick_radius(q.rx, q.ry, rel, &rx, &ry);
                alpha = coverage(sd_round_box(rel, half, rx, ry));

                if(tex.image)
                {
                    const float u = q.u0 + (p.x - q.x) / q.w * (q.u1 - q.u0);
                    const float v = q.v0 + (p.y - q.y) / q.h * (q.v1 - q.v0);

                    auto at = [&](int c) {
                        const int ix = std::min(std::max(static_cast<int>(u * tex.image_w), 0), tex.image_w - 1);
                        const int iy = std::min(std::max(static_cast<int>(v * tex.image_h), 0), tex.image_h - 1);
                        return tex.image[(static_cast<size_t>(iy) * tex.image_w + ix) * 4 + c] / 255.f;
                    };

                    col = RGBA {at(0), at(1), at(2), at(3)};
                }
                break;
            }

            case LHU_QUAD_LINEAR_GRAD:
            case LHU_QUAD_RADIAL_GRAD:
            case LHU_QUAD_CONIC_GRAD:
            {
                float rx = 0, ry = 0;
                pick_radius(q.rx, q.ry, rel, &rx, &ry);
                alpha = coverage(sd_round_box(rel, half, rx, ry));

                float t = 0.f;

                if(q.type == LHU_QUAD_LINEAR_GRAD)
                {
                    const Vec2  a {q.params[0], q.params[1]};
                    const Vec2  b {q.params[2], q.params[3]};
                    const Vec2  ab {b.x - a.x, b.y - a.y};
                    const float len2 = ab.x * ab.x + ab.y * ab.y;
                    t = len2 > 0.f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2 : 0.f;
                }
                else if(q.type == LHU_QUAD_RADIAL_GRAD)
                {
                    const float dx = (p.x - q.params[0]) / std::max(q.params[2], 1e-6f);
                    const float dy = (p.y - q.params[1]) / std::max(q.params[3], 1e-6f);
                    t              = std::sqrt(dx * dx + dy * dy);
                }
                else
                {
                    const float dx = p.x - q.params[0];
                    const float dy = p.y - q.params[1];
                    // CSS conic gradients start at 12 o'clock and run clockwise;
                    // y grows downward here, hence atan2(dx, -dy).
                    float deg = std::atan2(dx, -dy) * 57.2957795f - q.params[2];
                    deg       = std::fmod(std::fmod(deg, 360.f) + 360.f, 360.f);
                    t         = deg / 360.f;
                }

                col = sample_grad(tex, q.grad_row, t);
                break;
            }

            default: alpha = 0.f; break;
            }

            // Rounded-rect clipping.
            if(alpha > 0.f && q.clip_w >= 0.f)
            {
                const Vec2 c_centre {q.clip_x + q.clip_w * 0.5f, q.clip_y + q.clip_h * 0.5f};
                const Vec2 c_half {q.clip_w * 0.5f, q.clip_h * 0.5f};
                const Vec2 c_rel {p.x - c_centre.x, p.y - c_centre.y};

                const int   ci = c_rel.x > 0.f ? (c_rel.y < 0.f ? 1 : 2) : (c_rel.y < 0.f ? 0 : 3);
                const float cr = q.clip_r[ci];

                alpha *= coverage(sd_round_box(c_rel, c_half, cr, cr));
            }

            alpha *= col.a;
            if(alpha <= 0.f)
            {
                continue;
            }

            uint8_t* dst = &fb[(static_cast<size_t>(py) * fb_w + px) * 4];

            for(int c = 0; c < 3; ++c)
            {
                const float src = (c == 0 ? col.r : c == 1 ? col.g : col.b);
                dst[c] = static_cast<uint8_t>(std::lround((src * alpha + dst[c] / 255.f * (1.f - alpha)) * 255.f));
            }
            dst[3] = static_cast<uint8_t>(std::lround((alpha + dst[3] / 255.f * (1.f - alpha)) * 255.f));
        }
    }
}

} // namespace lhu_raster

#endif // LHU_RASTER_H
