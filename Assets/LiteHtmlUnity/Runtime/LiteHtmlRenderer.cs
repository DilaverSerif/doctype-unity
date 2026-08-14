using System;
using Unity.Collections;
using UnityEngine;
using UnityEngine.Rendering;

namespace LiteHtmlUnity
{
    /// <summary>
    /// Draws a recorded document into a <see cref="RenderTexture"/>.
    /// </summary>
    /// <remarks>
    /// Pipeline-agnostic on purpose: it issues its own CommandBuffer against an
    /// explicit render target, so it behaves identically under URP, HDRP and
    /// the built-in pipeline, and can be driven from edit mode and from tests.
    /// </remarks>
    public sealed class LiteHtmlRenderer : IDisposable
    {
        private static readonly int FontTexId = Shader.PropertyToID("_FontTex");
        private static readonly int GradTexId = Shader.PropertyToID("_GradTex");
        private static readonly int ImageTexId = Shader.PropertyToID("_ImageTex");
        private static readonly int GradSizeId = Shader.PropertyToID("_GradSize");

        private readonly LiteHtmlMeshBuilder _meshBuilder = new LiteHtmlMeshBuilder();
        private readonly CommandBuffer _cb = new CommandBuffer { name = "LiteHtml" };

        private Material _material;
        private Texture2D _fontAtlas;
        private Texture2D _gradLut;
        private Texture2D _white;

        private int _fontAtlasVersion = -1;
        private int _fontAtlasWidth;
        private int _fontAtlasHeight;
        private int _gradVersion = -1;
        private int _gradRows;

        /// <summary>Quads drawn in the most recent <see cref="Render"/>.</summary>
        public int LastQuadCount => _meshBuilder.QuadCount;

        public Material Material => _material;

        public LiteHtmlRenderer(Shader shader = null)
        {
            shader = shader != null ? shader : Shader.Find("LiteHtmlUnity/Quad");

            if (shader == null)
            {
                throw new InvalidOperationException(
                    "Shader 'LiteHtmlUnity/Quad' not found. If this is a built player, add it to " +
                    "Project Settings > Graphics > Always Included Shaders.");
            }

            _material = new Material(shader) { hideFlags = HideFlags.HideAndDontSave };

            _white = new Texture2D(1, 1, TextureFormat.RGBA32, false, false)
            {
                hideFlags = HideFlags.HideAndDontSave,
            };
            _white.SetPixel(0, 0, Color.white);
            _white.Apply();

            _material.SetTexture(ImageTexId, _white);
            _material.SetTexture(GradTexId, _white);
        }

        public void Dispose()
        {
            _meshBuilder.Dispose();
            _cb.Dispose();

            DestroySafely(_material);
            DestroySafely(_fontAtlas);
            DestroySafely(_gradLut);
            DestroySafely(_white);

            _material = null;
            _fontAtlas = null;
            _gradLut = null;
            _white = null;
        }

        private static void DestroySafely(UnityEngine.Object obj)
        {
            if (obj == null)
            {
                return;
            }

#if UNITY_EDITOR
            if (Application.isPlaying)
            {
                UnityEngine.Object.Destroy(obj);
            }
            else
            {
                UnityEngine.Object.DestroyImmediate(obj);
            }
#else
            UnityEngine.Object.Destroy(obj);
#endif
        }

        /// <summary>
        /// Uploads changed atlases, rebuilds the mesh and draws it into
        /// <paramref name="target"/>.
        /// </summary>
        /// <param name="documentSize">
        /// Extent of the document in CSS pixels. It is mapped onto the whole
        /// target, so passing a smaller size than the target is how device
        /// scaling (a browser's devicePixelRatio) is applied — for free, by the
        /// rasterizer, with antialiasing computed at target resolution.
        /// </param>
        public void Render(NativeArray<LiteHtmlQuad> quads, in LiteHtmlFrame frame, RenderTexture target,
                           Color clearColor, Vector2 documentSize, Texture imageAtlas = null)
        {
            if (target == null)
            {
                return;
            }

            if (documentSize.x <= 0f || documentSize.y <= 0f)
            {
                documentSize = new Vector2(target.width, target.height);
            }

            SyncFontAtlas(frame);
            SyncGradientLut(frame);

            _material.SetTexture(ImageTexId, imageAtlas != null ? imageAtlas : _white);

            _meshBuilder.Build(quads, frame.QuadCount, documentSize);

            _cb.Clear();
            _cb.SetRenderTarget(target);

            // Background colours are authored the same way CSS colours are —
            // as sRGB. ClearRenderTarget writes the value straight into an sRGB
            // target, so it has to be linearized first or it comes out washed
            // out in a linear-colour-space project.
            _cb.ClearRenderTarget(true, true,
                                  QualitySettings.activeColorSpace == ColorSpace.Linear
                                      ? clearColor.linear
                                      : clearColor);

            // Document space: origin top-left, y down, one unit = one CSS pixel.
            _cb.SetViewProjectionMatrices(Matrix4x4.identity,
                                          Matrix4x4.Ortho(0f, documentSize.x, documentSize.y, 0f, -1f, 1f));

            if (_meshBuilder.QuadCount > 0)
            {
                _cb.DrawMesh(_meshBuilder.Mesh, Matrix4x4.identity, _material, 0, 0);
            }

            Graphics.ExecuteCommandBuffer(_cb);
        }

        private void SyncFontAtlas(in LiteHtmlFrame frame)
        {
            if (frame.FontAtlasPixels == IntPtr.Zero || frame.FontAtlasWidth <= 0)
            {
                return;
            }

            bool resized = _fontAtlas == null ||
                           _fontAtlasWidth != frame.FontAtlasWidth ||
                           _fontAtlasHeight != frame.FontAtlasHeight;

            if (resized)
            {
                DestroySafely(_fontAtlas);

                // Single-channel coverage, so it must not be treated as sRGB.
                _fontAtlas = new Texture2D(frame.FontAtlasWidth, frame.FontAtlasHeight, TextureFormat.R8, false, true)
                {
                    name = "LiteHtml Font Atlas",
                    hideFlags = HideFlags.HideAndDontSave,
                    filterMode = FilterMode.Bilinear,
                    wrapMode = TextureWrapMode.Clamp,
                };

                _fontAtlasWidth = frame.FontAtlasWidth;
                _fontAtlasHeight = frame.FontAtlasHeight;
                _fontAtlasVersion = -1;
            }

            if (_fontAtlasVersion != frame.FontAtlasVersion)
            {
                _fontAtlas.LoadRawTextureData(frame.FontAtlasPixels, frame.FontAtlasWidth * frame.FontAtlasHeight);
                _fontAtlas.Apply(false, false);
                _fontAtlasVersion = frame.FontAtlasVersion;
            }

            _material.SetTexture(FontTexId, _fontAtlas);
        }

        private void SyncGradientLut(in LiteHtmlFrame frame)
        {
            if (frame.GradLutPixels == IntPtr.Zero || frame.GradLutRows <= 0)
            {
                _material.SetTexture(GradTexId, _white);
                _material.SetVector(GradSizeId, new Vector4(1f, 1f, 0f, 0f));
                return;
            }

            if (_gradLut == null || _gradRows != frame.GradLutRows || _gradLut.width != frame.GradLutWidth)
            {
                DestroySafely(_gradLut);

                // The LUT holds sRGB colours, so let the GPU linearize on sample.
                _gradLut = new Texture2D(frame.GradLutWidth, frame.GradLutRows, TextureFormat.RGBA32, false, false)
                {
                    name = "LiteHtml Gradient LUT",
                    hideFlags = HideFlags.HideAndDontSave,
                    filterMode = FilterMode.Bilinear,
                    wrapMode = TextureWrapMode.Clamp,
                };

                _gradRows = frame.GradLutRows;
                _gradVersion = -1;
            }

            if (_gradVersion != frame.GradLutVersion)
            {
                _gradLut.LoadRawTextureData(frame.GradLutPixels, frame.GradLutWidth * frame.GradLutRows * 4);
                _gradLut.Apply(false, false);
                _gradVersion = frame.GradLutVersion;
            }

            _material.SetTexture(GradTexId, _gradLut);
            _material.SetVector(GradSizeId, new Vector4(frame.GradLutWidth, frame.GradLutRows, 0f, 0f));
        }
    }
}
