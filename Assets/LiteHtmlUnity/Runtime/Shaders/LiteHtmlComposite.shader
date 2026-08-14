// Composites a LiteHtmlView's surface onto a uGUI canvas.
//
// The quad shader blends RGB with SrcAlpha, so colour reaches the surface
// already multiplied by its own alpha. uGUI's default material multiplies by
// alpha a second time, which renders a half-transparent menu at a quarter of the
// opacity it was authored with. This composites with One OneMinusSrcAlpha
// instead, which is the matching half of that convention.
//
// Everything else follows UI-Default so masks, nesting and tinting behave the
// way any other uGUI graphic does.
Shader "LiteHtml/Composite"
{
    Properties
    {
        [PerRendererData] _MainTex ("Surface", 2D) = "white" {}
        _Color ("Tint", Color) = (1,1,1,1)

        _StencilComp ("Stencil Comparison", Float) = 8
        _Stencil ("Stencil ID", Float) = 0
        _StencilOp ("Stencil Operation", Float) = 0
        _StencilWriteMask ("Stencil Write Mask", Float) = 255
        _StencilReadMask ("Stencil Read Mask", Float) = 255
        _ColorMask ("Color Mask", Float) = 15
    }

    SubShader
    {
        Tags
        {
            "Queue" = "Transparent"
            "IgnoreProjector" = "True"
            "RenderType" = "Transparent"
            "PreviewType" = "Plane"
            "CanUseSpriteAtlas" = "True"
        }

        Stencil
        {
            Ref [_Stencil]
            Comp [_StencilComp]
            Pass [_StencilOp]
            ReadMask [_StencilReadMask]
            WriteMask [_StencilWriteMask]
        }

        Cull Off
        Lighting Off
        ZWrite Off
        ZTest [unity_GUIZTestMode]
        Blend One OneMinusSrcAlpha
        ColorMask [_ColorMask]

        Pass
        {
            Name "Default"
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 2.0
            #include "UnityCG.cginc"
            #include "UnityUI.cginc"
            #pragma multi_compile_local _ UNITY_UI_CLIP_RECT

            struct appdata_t
            {
                float4 vertex : POSITION;
                float4 color  : COLOR;
                float2 uv     : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f
            {
                float4 vertex : SV_POSITION;
                fixed4 color  : COLOR;
                float2 uv     : TEXCOORD0;
                float4 world  : TEXCOORD1;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            sampler2D _MainTex;
            fixed4 _Color;
            float4 _ClipRect;
            float4 _MainTex_ST;

            v2f vert(appdata_t v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.world = v.vertex;
                o.vertex = UnityObjectToClipPos(v.vertex);
                o.uv = TRANSFORM_TEX(v.uv, _MainTex);
                o.color = v.color * _Color;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                fixed4 surface = tex2D(_MainTex, i.uv);

                // The surface is premultiplied, so a tint has to scale colour by
                // the tint's own alpha as well, or fading the graphic out would
                // leave the colour at full strength.
                fixed4 col;
                col.rgb = surface.rgb * i.color.rgb * i.color.a;
                col.a   = surface.a * i.color.a;

                #ifdef UNITY_UI_CLIP_RECT
                col *= UnityGet2DClipping(i.world.xy, _ClipRect);
                #endif

                return col;
            }
            ENDCG
        }
    }
}
