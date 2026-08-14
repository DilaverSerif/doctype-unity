// Analytic renderer for the litehtml quad stream.
//
// Every shape — rounded fills, border rings, glyphs, images, gradients — is
// reconstructed here from per-vertex parameters, so a whole page draws in one
// batch. The maths mirrors Native/tests/lhu_raster.h line for line; the golden
// tests compare the two, so keep them in sync.

Shader "Doctype/Quad"
{
    Properties
    {
        _FontTex  ("Font atlas (R8)", 2D) = "black" {}
        _GradTex  ("Gradient LUT", 2D) = "white" {}
        _ImageTex ("Image atlas", 2D) = "white" {}
        _GradSize ("Gradient LUT size", Vector) = (256, 1, 0, 0)
    }

    SubShader
    {
        Tags
        {
            "RenderType" = "Transparent"
            "Queue" = "Transparent"
            "IgnoreProjector" = "True"
            "PreviewType" = "Plane"
        }

        Cull Off
        ZWrite Off
        ZTest Always
        Lighting Off
        // Straight source-over. The separate alpha term keeps the destination
        // alpha correct so the result composites properly downstream.
        Blend SrcAlpha OneMinusSrcAlpha, One OneMinusSrcAlpha

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0

            #include "UnityCG.cginc"

            #define LHU_RECT   0
            #define LHU_BORDER 1
            #define LHU_GLYPH  2
            #define LHU_IMAGE  3
            #define LHU_LINEAR 4
            #define LHU_RADIAL 5
            #define LHU_CONIC  6

            sampler2D _FontTex;
            sampler2D _GradTex;
            sampler2D _ImageTex;
            float4 _GradSize;

            struct appdata
            {
                float4 vertex   : POSITION;
                float4 color    : COLOR;
                float4 uv       : TEXCOORD0; // u, v, type, gradient row
                float4 rect     : TEXCOORD1; // centre xy, half-size xy
                float4 radiusX  : TEXCOORD2;
                float4 radiusY  : TEXCOORD3;
                float4 border   : TEXCOORD4; // left, top, right, bottom
                float4 clipRect : TEXCOORD5; // centre xy, half-size xy
                float4 clipR    : TEXCOORD6;
                float4 params   : TEXCOORD7;
            };

            struct v2f
            {
                float4 pos      : SV_POSITION;
                float4 uvDoc    : TEXCOORD0; // uv.xy, document-space position
                float4 color    : TEXCOORD1;
                float4 rect     : TEXCOORD2;
                float4 radiusX  : TEXCOORD3;
                float4 radiusY  : TEXCOORD4;
                float4 border   : TEXCOORD5;
                float4 clipRect : TEXCOORD6;
                float4 clipR    : TEXCOORD7;
                float4 params   : TEXCOORD8;
                float2 misc     : TEXCOORD9; // type, gradient row
            };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos      = UnityObjectToClipPos(v.vertex);
                o.uvDoc    = float4(v.uv.xy, v.vertex.xy);
                o.color    = v.color;
                o.rect     = v.rect;
                o.radiusX  = v.radiusX;
                o.radiusY  = v.radiusY;
                o.border   = v.border;
                o.clipRect = v.clipRect;
                o.clipR    = v.clipR;
                o.params   = v.params;
                o.misc     = float2(v.uv.z, v.uv.w);
                return o;
            }

            // Signed distance to a rounded box. Exact for circular corners; for
            // elliptical ones the radial term is scaled by the smaller radius,
            // which stays continuous and is visually indistinguishable.
            float sdRoundBox(float2 p, float2 b, float rx, float ry)
            {
                rx = max(rx, 0.0);
                ry = max(ry, 0.0);

                float2 q = abs(p) - b + float2(rx, ry);
                float2 m = max(q, 0.0);

                float outside;
                if (rx > 0.0 && ry > 0.0)
                {
                    outside = length(m / float2(rx, ry)) * min(rx, ry);
                }
                else
                {
                    outside = length(m);
                }

                return min(max(q.x, q.y), 0.0) + outside - min(rx, ry);
            }

            // CSS corner order is top-left, top-right, bottom-right, bottom-left.
            float2 pickRadius(float4 rx, float4 ry, float2 p)
            {
                float2 top = p.x > 0.0 ? float2(rx.y, ry.y) : float2(rx.x, ry.x);
                float2 bot = p.x > 0.0 ? float2(rx.z, ry.z) : float2(rx.w, ry.w);
                return p.y < 0.0 ? top : bot;
            }

            float coverage(float d)
            {
                // fwidth gives the shape one pixel of falloff regardless of the
                // scale the document is being rendered at.
                float fw = max(fwidth(d), 1e-6);
                return saturate(0.5 - d / fw);
            }

            // Which of the four border edges owns this point?
            //
            // The smallest normalized penetration depth wins, which is exactly
            // the miter split along the corner-to-corner diagonals but needs no
            // line equations.
            int owningEdge(float2 local, float2 size, float4 border)
            {
                const float INF = 1e30;

                float tL = border.x > 0.0 ? local.x / border.x : INF;
                float tT = border.y > 0.0 ? local.y / border.y : INF;
                float tR = border.z > 0.0 ? (size.x - local.x) / border.z : INF;
                float tB = border.w > 0.0 ? (size.y - local.y) / border.w : INF;

                int best = 0; // top
                float bt = tT;

                if (tR < bt) { best = 1; bt = tR; }
                if (tB < bt) { best = 2; bt = tB; }
                if (tL < bt) { best = 3; }

                return best;
            }

            // The exact sRGB transfer function, not Unity's GammaToLinearSpace
            // approximation. The approximation is off by several 8-bit steps in
            // dark colours, which is exactly where UI backgrounds live, so a
            // CSS colour would not survive the round trip through an sRGB target.
            float3 SRGBToLinearExact(float3 c)
            {
                float3 lo = c / 12.92;
                float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
                return lerp(lo, hi, step(0.04045, c));
            }

            float4 sampleGradient(float row, float t)
            {
                float rows = max(_GradSize.y, 1.0);
                return tex2D(_GradTex, float2(saturate(t), (row + 0.5) / rows));
            }

            fixed4 frag(v2f i) : SV_Target
            {
                int type = (int)(i.misc.x + 0.5);

                float2 p   = i.uvDoc.zw;
                float2 rel = p - i.rect.xy;
                float2 hs  = i.rect.zw;

                float4 col = i.color;
                #ifndef UNITY_COLORSPACE_GAMMA
                    // Vertex colours arrive as raw sRGB bytes; the render target
                    // expects linear.
                    col.rgb = SRGBToLinearExact(col.rgb);
                #endif

                float alpha = 1.0;

                if (type == LHU_GLYPH)
                {
                    // Glyph coverage is already antialiased in the atlas.
                    alpha = tex2D(_FontTex, i.uvDoc.xy).r;
                }
                else
                {
                    float2 r = pickRadius(i.radiusX, i.radiusY, rel);
                    float d = sdRoundBox(rel, hs, r.x, r.y);
                    alpha = coverage(d);

                    if (type == LHU_BORDER)
                    {
                        // Inner rounded rect = outer shrunk by the per-side widths.
                        float bl = i.border.x, bt = i.border.y;
                        float br = i.border.z, bb = i.border.w;

                        float2 outerMin = i.rect.xy - hs;
                        float2 size     = hs * 2.0;

                        float2 inHalf   = max(float2(size.x - bl - br, size.y - bt - bb) * 0.5, 0.0);
                        float2 inCentre = outerMin + float2(bl, bt) + inHalf;

                        float4 inRx = max(float4(i.radiusX.x - bl, i.radiusX.y - br,
                                                 i.radiusX.z - br, i.radiusX.w - bl), 0.0);
                        float4 inRy = max(float4(i.radiusY.x - bt, i.radiusY.y - bt,
                                                 i.radiusY.z - bb, i.radiusY.w - bb), 0.0);

                        float2 inRel = p - inCentre;
                        float2 ir = pickRadius(inRx, inRy, inRel);
                        float dIn = sdRoundBox(inRel, inHalf, ir.x, ir.y);

                        alpha *= coverage(-dIn);

                        int want = (int)round(i.params.x);
                        if (want >= 0 && owningEdge(p - outerMin, size, i.border) != want)
                        {
                            alpha = 0.0;
                        }
                    }
                    else if (type == LHU_IMAGE)
                    {
                        col = tex2D(_ImageTex, i.uvDoc.xy);
                    }
                    else if (type >= LHU_LINEAR)
                    {
                        float t = 0.0;

                        if (type == LHU_LINEAR)
                        {
                            float2 ab = i.params.zw - i.params.xy;
                            float len2 = dot(ab, ab);
                            t = len2 > 0.0 ? dot(p - i.params.xy, ab) / len2 : 0.0;
                        }
                        else if (type == LHU_RADIAL)
                        {
                            float2 dr = (p - i.params.xy) / max(i.params.zw, 1e-6);
                            t = length(dr);
                        }
                        else // conic
                        {
                            float2 dc = p - i.params.xy;
                            // CSS conic gradients start at 12 o'clock and run
                            // clockwise; document y grows downward.
                            float deg = degrees(atan2(dc.x, -dc.y)) - i.params.z;
                            t = frac(deg / 360.0 + 1.0);
                        }

                        col = sampleGradient(i.misc.y, t);
                    }
                }

                alpha *= col.a;
                clip(alpha - 0.0005);

                // Rounded-rect clipping. A negative half-width marks "unclipped".
                if (i.clipRect.z >= 0.0)
                {
                    float2 cRel = p - i.clipRect.xy;
                    float cr = cRel.x > 0.0 ? (cRel.y < 0.0 ? i.clipR.y : i.clipR.z)
                                            : (cRel.y < 0.0 ? i.clipR.x : i.clipR.w);
                    alpha *= coverage(sdRoundBox(cRel, i.clipRect.zw, cr, cr));
                }

                return fixed4(col.rgb, alpha);
            }
            ENDCG
        }
    }

    Fallback Off
}
