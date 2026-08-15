// Replaces a rectangle of the render target with the background colour.
//
// This exists because CommandBuffer.ClearRenderTarget and scissor rects do not
// compose portably: OpenGL scissors its clears, D3D and Metal do not, so a
// scissored ClearRenderTarget erases the whole retained frame on some devices
// and only the dirty region on others. A quad drawn with Blend One Zero writes
// colour and alpha unconditionally, which is a clear by other means, and it
// obeys geometry and scissor everywhere.
Shader "Doctype/RegionClear"
{
    Properties
    {
        _Color ("Clear colour (premultiplied, linear)", Color) = (0, 0, 0, 0)
    }

    SubShader
    {
        Tags { "RenderType" = "Opaque" }

        Cull Off
        ZWrite Off
        ZTest Always
        Blend One Zero

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 _Color;

            struct appdata
            {
                float4 vertex : POSITION;
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
            };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                return _Color;
            }
            ENDCG
        }
    }
}
