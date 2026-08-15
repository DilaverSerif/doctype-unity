using System;
using Unity.Collections;
using UnityEngine;
using UnityEngine.Rendering;

namespace Doctype
{
    /// <summary>
    /// Draws a recorded document into a <see cref="RenderTexture"/>.
    /// </summary>
    /// <remarks>
    /// Pipeline-agnostic on purpose: it issues its own CommandBuffer against an
    /// explicit render target, so it behaves identically under URP, HDRP and
    /// the built-in pipeline, and can be driven from edit mode and from tests.
    /// </remarks>
    public sealed class HtmlRenderer : IDisposable
    {
        private static readonly int FontTexId = Shader.PropertyToID("_FontTex");
        private static readonly int GradTexId = Shader.PropertyToID("_GradTex");
        private static readonly int ImageTexId = Shader.PropertyToID("_ImageTex");
        private static readonly int GradSizeId = Shader.PropertyToID("_GradSize");
        private static readonly int ClearColorId = Shader.PropertyToID("_Color");
        private static readonly int ScrollScratchId = Shader.PropertyToID("_DoctypeScrollScratch");

        private readonly HtmlMeshBuilder _meshBuilder = new HtmlMeshBuilder();
        private readonly CommandBuffer _cb = new CommandBuffer { name = "Doctype" };

        private Material _material;
        private Material _clearMaterial;
        private Mesh _clearMesh;
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

        /// <summary>How much of the target the most recent Render repainted.</summary>
        public HtmlDirtyMode LastDirtyMode { get; private set; } = HtmlDirtyMode.Full;

        /// <summary>Scissor rect of the last partial repaint, in target pixels.</summary>
        public RectInt LastDirtyPixels { get; private set; }

        public Material Material => _material;

        public HtmlRenderer(Shader shader = null)
        {
            shader = shader != null ? shader : Shader.Find("Doctype/Quad");

            if (shader == null)
            {
                throw new InvalidOperationException(
                    "Shader 'Doctype/Quad' not found. If this is a built player, add it to " +
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

            // The partial path clears by drawing, because ClearRenderTarget and
            // scissor rects do not compose the same way on every API. Missing
            // shader degrades to full repaints, not to a broken image.
            Shader clearShader = Shader.Find("Doctype/RegionClear");
            if (clearShader != null)
            {
                _clearMaterial = new Material(clearShader) { hideFlags = HideFlags.HideAndDontSave };
            }
            else
            {
                Debug.LogWarning("[Doctype] Doctype/RegionClear not found; partial redraw disabled. " +
                                 "Add the shader to Always Included Shaders for player builds.");
            }

            _clearMesh = new Mesh { name = "Doctype Region", hideFlags = HideFlags.HideAndDontSave };
            _clearMesh.vertices = new[]
            {
                new Vector3(0f, 0f, 0f), new Vector3(1f, 0f, 0f),
                new Vector3(1f, 1f, 0f), new Vector3(0f, 1f, 0f),
            };
            _clearMesh.triangles = new[] { 0, 1, 2, 0, 2, 3 };
            _clearMesh.UploadMeshData(true);
        }

        public void Dispose()
        {
            _meshBuilder.Dispose();
            _cb.Dispose();

            DestroySafely(_material);
            DestroySafely(_clearMaterial);
            DestroySafely(_clearMesh);
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
        public void Render(NativeArray<HtmlQuad> quads, in HtmlFrame frame, RenderTexture target,
                           Color clearColor, Vector2 documentSize, Texture imageAtlas = null,
                           bool forceFullRedraw = false)
        {
            if (target == null)
            {
                return;
            }

            if (documentSize.x <= 0f || documentSize.y <= 0f)
            {
                documentSize = new Vector2(target.width, target.height);
            }

            // The target retains the previous frame between renders, which is
            // what makes the dirty modes meaningful: None keeps it untouched,
            // Rect repaints a hole in it. Full is the only mode that does not
            // rely on that -- and therefore the only safe answer whenever the
            // caller knows the retained pixels are gone or remapped (a fresh
            // target, a background change) or the plugin predates the field.
            HtmlDirtyMode mode = frame.DirtyMode;
            if (forceFullRedraw || _clearMaterial == null)
            {
                mode = HtmlDirtyMode.Full;
            }

            LastDirtyMode = mode;

            if (mode == HtmlDirtyMode.None)
            {
                // The target already shows exactly this frame.
                return;
            }

            SyncFontAtlas(frame);
            SyncGradientLut(frame);

            _material.SetTexture(ImageTexId, imageAtlas != null ? imageAtlas : _white);

            _meshBuilder.Build(quads, frame.QuadCount, documentSize);

            // Background colours are authored the same way CSS colours are: as
            // sRGB. Both clears write the value straight into an sRGB target,
            // so it has to be linearized first or it comes out washed out in a
            // linear-colour-space project.
            Color clear = QualitySettings.activeColorSpace == ColorSpace.Linear
                              ? clearColor.linear
                              : clearColor;

            _cb.Clear();

            // The copy has to be queued before the repaint. A translation the
            // texel copy cannot express (fractional pixels at this scale, a
            // platform without RT copies) degrades to one full repaint.
            if (mode == HtmlDirtyMode.Scroll && !EmitScrollCopy(frame, target, documentSize))
            {
                mode = HtmlDirtyMode.Full;
                LastDirtyMode = mode;
            }

            _cb.SetRenderTarget(target);

            // Document space: origin top-left, y down, one unit = one CSS pixel.
            _cb.SetViewProjectionMatrices(Matrix4x4.identity,
                                          Matrix4x4.Ortho(0f, documentSize.x, documentSize.y, 0f, -1f, 1f));

            if (mode == HtmlDirtyMode.Rect || mode == HtmlDirtyMode.Scroll)
            {
                RepaintRegion(frame.DirtyX, frame.DirtyY, frame.DirtyW, frame.DirtyH,
                              target, documentSize, clear, updateLastDirty: true);

                // A scroll can carry a second rect: the sliver on the scrolled
                // window's opposite edge that the whole-pixel copy cannot move.
                if (mode == HtmlDirtyMode.Scroll && frame.Dirty2W > 0f && frame.Dirty2H > 0f)
                {
                    RepaintRegion(frame.Dirty2X, frame.Dirty2Y, frame.Dirty2W, frame.Dirty2H,
                                  target, documentSize, clear, updateLastDirty: false);
                }
            }
            else
            {
                LastDirtyPixels = new RectInt(0, 0, target.width, target.height);

                _cb.ClearRenderTarget(true, true, clear);

                if (_meshBuilder.QuadCount > 0)
                {
                    _cb.DrawMesh(_meshBuilder.Mesh, Matrix4x4.identity, _material, 0, 0);
                }
            }

            Graphics.ExecuteCommandBuffer(_cb);
        }

        /// <summary>
        /// Queues one scissored clear + draw over a dirty rect (document
        /// space). The scissor is what keeps every other pixel of the retained
        /// target untouched.
        /// </summary>
        private void RepaintRegion(float dirtyX, float dirtyY, float dirtyW, float dirtyH,
                                   RenderTexture target, Vector2 documentSize, Color clear, bool updateLastDirty)
        {
            // Document units to target pixels, padded a pixel outward so
            // rounding can never shave the repaint short of the dirt.
            float sx = target.width / documentSize.x;
            float sy = target.height / documentSize.y;

            int px0 = Mathf.Clamp(Mathf.FloorToInt(dirtyX * sx) - 1, 0, target.width);
            int py0 = Mathf.Clamp(Mathf.FloorToInt(dirtyY * sy) - 1, 0, target.height);
            int px1 = Mathf.Clamp(Mathf.CeilToInt((dirtyX + dirtyW) * sx) + 1, 0, target.width);
            int py1 = Mathf.Clamp(Mathf.CeilToInt((dirtyY + dirtyH) * sy) + 1, 0, target.height);

            if (px1 <= px0 || py1 <= py0)
            {
                return;
            }

            // The scissor is given in target pixels with a bottom-left
            // origin, the document counts y downward from the top; flip.
            var scissor = new Rect(px0, target.height - py1, px1 - px0, py1 - py0);
            if (updateLastDirty)
            {
                LastDirtyPixels = new RectInt(px0, py0, px1 - px0, py1 - py0);
            }

            _cb.EnableScissorRect(scissor);

            // Clear by drawing (see RegionClear): a scissored quad in
            // document space over the dirty rect, Blend One Zero.
            _clearMaterial.SetColor(ClearColorId, clear);
            var region = Matrix4x4.TRS(new Vector3(dirtyX, dirtyY, 0f),
                                       Quaternion.identity,
                                       new Vector3(dirtyW, dirtyH, 1f));
            _cb.DrawMesh(_clearMesh, region, _clearMaterial, 0, 0);

            // The whole mesh, scissored: quads outside the rect cost their
            // vertices (microseconds) and no fragments, which beats working
            // out on the CPU which of them overlap the region.
            if (_meshBuilder.QuadCount > 0)
            {
                _cb.DrawMesh(_meshBuilder.Mesh, Matrix4x4.identity, _material, 0, 0);
            }

            _cb.DisableScissorRect();
        }

        /// <summary>
        /// Queues the pixel move of a <see cref="HtmlDirtyMode.Scroll"/> frame:
        /// the still-valid part of the scrolled region rides along as two texel
        /// copies (into a scratch target and back shifted, because a texture
        /// cannot copy onto itself), and only the strip that scrolled in is
        /// left for the repaint. Returns false when the move cannot be
        /// expressed exactly, in which case the caller must repaint fully.
        /// </summary>
        private bool EmitScrollCopy(in HtmlFrame frame, RenderTexture target, Vector2 documentSize)
        {
            if ((SystemInfo.copyTextureSupport & CopyTextureSupport.RTToTexture) == 0)
            {
                return false;
            }

            float sx = target.width / documentSize.x;
            float sy = target.height / documentSize.y;

            // A texel copy cannot resample: the translation and the region must
            // land on whole pixels, or every scroll frame would blur a little.
            // Native guarantees whole document pixels; a fractional scale
            // between document and target re-breaks that, and falls back.
            if (!ToPixel(frame.ScrollDx * sx, out int tx) || !ToPixel(frame.ScrollDy * sy, out int ty) ||
                !ToPixel(frame.ScrollX * sx, out int dx0) || !ToPixel(frame.ScrollY * sy, out int dy0) ||
                !ToPixel((frame.ScrollX + frame.ScrollW) * sx, out int dx1) ||
                !ToPixel((frame.ScrollY + frame.ScrollH) * sy, out int dy1))
            {
                return false;
            }

            if (tx == 0 && ty == 0)
            {
                return false;
            }

            // The frame carries the copy's destination directly; the source is
            // the destination un-translated. Both must sit inside the target.
            int w = dx1 - dx0;
            int h = dy1 - dy0;
            int sx0 = dx0 - tx;
            int sy0 = dy0 - ty;
            if (w <= 0 || h <= 0 ||
                dx0 < 0 || dy0 < 0 || dx1 > target.width || dy1 > target.height ||
                sx0 < 0 || sy0 < 0 || sx0 + w > target.width || sy0 + h > target.height)
            {
                return false;
            }

            // Document y counts down from the top, CopyTexture from the bottom.
            int srcY = target.height - (sy0 + h);
            int dstY = target.height - (dy0 + h);

            RenderTextureDescriptor desc = target.descriptor;
            desc.depthBufferBits = 0;

            _cb.GetTemporaryRT(ScrollScratchId, desc);
            _cb.CopyTexture(target, 0, 0, sx0, srcY, w, h, ScrollScratchId, 0, 0, 0, 0);
            _cb.CopyTexture(ScrollScratchId, 0, 0, 0, 0, w, h, target, 0, 0, dx0, dstY);
            _cb.ReleaseTemporaryRT(ScrollScratchId);
            return true;
        }

        private static bool ToPixel(float value, out int pixel)
        {
            pixel = Mathf.RoundToInt(value);
            return Mathf.Abs(value - pixel) < 0.01f;
        }

        private void SyncFontAtlas(in HtmlFrame frame)
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
                    name = "Doctype Font Atlas",
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

        private void SyncGradientLut(in HtmlFrame frame)
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
                    name = "Doctype Gradient LUT",
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
