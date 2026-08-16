using System;
using System.Runtime.InteropServices;
using Unity.Collections;
using UnityEngine;
using UnityEngine.Rendering;

namespace Doctype
{
    /// <summary>
    /// Turns a recorded quad stream into a single dynamic mesh.
    /// </summary>
    /// <remarks>
    /// Every quad — fill, border edge, glyph, image, gradient — becomes four
    /// vertices in one mesh drawn with one material, so a whole page is one
    /// draw call. Per-quad parameters ride along in the eight UV channels and
    /// the shader reconstructs the shape analytically from them.
    /// </remarks>
    public sealed class HtmlMeshBuilder : IDisposable
    {
        /// <summary>
        /// Bytes per vertex. The benchmark derives its bandwidth column from
        /// this, so it must match <see cref="Vertex"/> exactly.
        /// </summary>
        public const int BytesPerVertex = 108;

        /// <summary>
        /// Four half-precision floats, written with <see cref="Mathf.FloatToHalf"/>.
        /// The GPU expands them back to float4; the shader never notices.
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        private struct Half4
        {
            public ushort X, Y, Z, W;

            public Half4(float x, float y, float z, float w)
            {
                X = Mathf.FloatToHalf(x);
                Y = Mathf.FloatToHalf(y);
                Z = Mathf.FloatToHalf(z);
                W = Mathf.FloatToHalf(w);
            }
        }

        /// <summary>
        /// Vertex layout. 108 bytes: fat, but it keeps everything in one batch
        /// and avoids needing structured buffers (which GLES3 lacks).
        /// </summary>
        /// <remarks>
        /// What is half and what is full precision is deliberate. Radii, border
        /// widths and clip corner radii are small lengths (rarely above a few
        /// hundred CSS pixels), where float16 is exact to a fraction of a pixel.
        /// Positions, rects, clip rects, atlas UVs and gradient geometry span
        /// the whole document or atlas, where float16 resolution (1 part in
        /// 2048) would visibly move SDF edges and glyph samples, so they stay
        /// float32. Position z is implied: the attribute is two floats and the
        /// GPU fills in z = 0, w = 1.
        /// </remarks>
        [StructLayout(LayoutKind.Sequential)]
        private struct Vertex
        {
            public Vector2 Position; // document space, y down
            public Color32 Color;
            public Vector4 Uv;       // u, v, type, gradient row
            public Vector4 Rect;     // centre xy, half-size xy
            public Half4 RadiusX;    // per corner: tl, tr, br, bl
            public Half4 RadiusY;
            public Half4 Border;     // left, top, right, bottom
            public Vector4 Clip;     // centre xy, half-size xy (x < 0 => unclipped)
            public Half4 ClipR;      // per corner
            public Vector4 Params;   // border edge index / gradient geometry
        }

        private static readonly VertexAttributeDescriptor[] Layout =
        {
            new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.Float32, 2),
            new VertexAttributeDescriptor(VertexAttribute.Color, VertexAttributeFormat.UNorm8, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord0, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord1, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord2, VertexAttributeFormat.Float16, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord3, VertexAttributeFormat.Float16, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord4, VertexAttributeFormat.Float16, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord5, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord6, VertexAttributeFormat.Float16, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord7, VertexAttributeFormat.Float32, 4),
        };

        // Antialiased edges are evaluated up to a pixel outside the shape, so
        // non-glyph quads get a small skirt of geometry to draw that falloff in.
        private const float AntiAliasPad = 1.5f;

        private readonly Mesh _mesh;
        private NativeArray<Vertex> _vertices;
        private NativeArray<uint> _indices;
        private int _capacityQuads;

        // Persistent-mesh state. The GPU buffers are sized to capacity and the
        // mesh is never cleared between frames, so a frame whose stable
        // prefix/suffix (see HtmlFrame) says most quads did not move only
        // rewrites the span between them. The builder filters quads (empty,
        // transparent, clipped-away border bands), so native quad indices do
        // not map 1:1 onto vertex offsets; _emittedBefore is the prefix sum
        // that translates one into the other, and it is exactly as valid as
        // the byte-identical prefix it describes, because the drop decision is
        // a pure function of the quad's bytes.
        private int[] _emittedBefore; // emitted quads before native index q
        private int _lastNativeCount;
        private int _lastEmitted;
        private bool _valid;

        /// <summary>Number of quads in the last built mesh.</summary>
        public int QuadCount { get; private set; }

        /// <summary>Vertices actually uploaded by the last build — the measured
        /// cost a ranged rewrite avoids paying in full.</summary>
        public int LastUploadedVertices { get; private set; }

        /// <summary>Kilobytes of vertex data uploaded over this builder's lifetime.</summary>
        public double UploadedKbTotal { get; private set; }

        public Mesh Mesh => _mesh;

        public HtmlMeshBuilder()
        {
            Debug.Assert(Unity.Collections.LowLevel.Unsafe.UnsafeUtility.SizeOf<Vertex>() == BytesPerVertex,
                         "Vertex struct size drifted from BytesPerVertex");
            _mesh = new Mesh { name = "Doctype", hideFlags = HideFlags.HideAndDontSave };
            _mesh.MarkDynamic();
        }

        public void Dispose()
        {
            if (_vertices.IsCreated)
            {
                _vertices.Dispose();
            }

            if (_indices.IsCreated)
            {
                _indices.Dispose();
            }

            if (_mesh != null)
            {
#if UNITY_EDITOR
                if (Application.isPlaying)
                {
                    UnityEngine.Object.Destroy(_mesh);
                }
                else
                {
                    UnityEngine.Object.DestroyImmediate(_mesh);
                }
#else
                UnityEngine.Object.Destroy(_mesh);
#endif
            }
        }

        private void EnsureCapacity(int quads)
        {
            if (_capacityQuads >= quads && _vertices.IsCreated)
            {
                return;
            }

            // Grow generously; relayout churn otherwise reallocates every frame.
            int capacity = Mathf.NextPowerOfTwo(Mathf.Max(quads, 256));

            if (_vertices.IsCreated)
            {
                _vertices.Dispose();
            }

            if (_indices.IsCreated)
            {
                _indices.Dispose();
            }

            _vertices = new NativeArray<Vertex>(capacity * 4, Allocator.Persistent,
                                                NativeArrayOptions.UninitializedMemory);
            _indices = new NativeArray<uint>(capacity * 6, Allocator.Persistent,
                                             NativeArrayOptions.UninitializedMemory);
            _emittedBefore = new int[capacity + 1];
            _capacityQuads = capacity;

            // The index pattern never depends on content — quad k is always
            // {4k, 4k+1, 4k+2, 4k, 4k+2, 4k+3} — so it is written once per
            // capacity and never touched again; per frame only the submesh's
            // index COUNT moves.
            for (int k = 0; k < capacity; k++)
            {
                uint b = (uint)(k * 4);
                int j = k * 6;
                _indices[j + 0] = b;
                _indices[j + 1] = b + 1;
                _indices[j + 2] = b + 2;
                _indices[j + 3] = b;
                _indices[j + 4] = b + 2;
                _indices[j + 5] = b + 3;
            }

            // GPU buffers live at capacity, so a ranged frame can rewrite a
            // span without the mesh being cleared or its params re-sent.
            _mesh.Clear(true);
            _mesh.SetVertexBufferParams(capacity * 4, Layout);
            _mesh.SetIndexBufferParams(capacity * 6, IndexFormat.UInt32);
            _mesh.SetIndexBufferData(_indices, 0, 0, capacity * 6, MeshUpdateFlags.DontValidateIndices);
            _mesh.subMeshCount = 1;
            _mesh.SetSubMesh(0, new SubMeshDescriptor(0, 0, MeshTopology.Triangles),
                             MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontValidateIndices);

            // Whatever the old buffers held is gone; the next build is full.
            _valid = false;
        }

        /// <summary>
        /// Rebuilds the whole mesh from a recorded frame.
        /// </summary>
        public void Build(NativeArray<HtmlQuad> quads, int count, Vector2 documentSize)
        {
            Build(quads, count, documentSize, 0, 0);
        }

        /// <summary>
        /// Rebuilds the mesh, rewriting only the span the frame's stable
        /// prefix/suffix leaves open when the previous build is still resident.
        /// Pass zeros to force a full rewrite.
        /// </summary>
        public void Build(NativeArray<HtmlQuad> quads, int count, Vector2 documentSize,
                          int stablePrefix, int stableSuffix)
        {
            if (count <= 0 || !quads.IsCreated)
            {
                QuadCount = 0;
                LastUploadedVertices = 0;
                _lastNativeCount = 0;
                _lastEmitted = 0;

                if (_capacityQuads > 0)
                {
                    // Keep the buffers; an empty frame is a submesh of zero
                    // indices, not a destroyed mesh.
                    _mesh.SetSubMesh(0, new SubMeshDescriptor(0, 0, MeshTopology.Triangles),
                                     MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontValidateIndices);
                    _emittedBefore[0] = 0;
                    _valid = true;
                }
                else
                {
                    _mesh.Clear();
                    _valid = false;
                }

                return;
            }

            EnsureCapacity(count); // resets _valid when it reallocates

            // The claim is only usable against the exact buffer the last build
            // left behind. Anything that broke that continuity — a fresh
            // capacity, a caller that skipped a frame — falls back to full.
            // The suffix additionally requires equal counts, which native
            // already guarantees; distrust it anyway, the check is one compare.
            int prefix = stablePrefix;
            int suffix = stableSuffix;

            if (!_valid || prefix < 0 || suffix < 0)
            {
                prefix = 0;
                suffix = 0;
            }
            else
            {
                if (suffix > 0 && count != _lastNativeCount)
                {
                    suffix = 0;
                }

                int maxCommon = Mathf.Min(count, _lastNativeCount);
                prefix = Mathf.Min(prefix, maxCommon);
                suffix = Mathf.Min(suffix, count - prefix);
            }

            // Vertex offsets translate through the emitted-quad prefix sum: the
            // prefix quads are byte-identical, so their drop decisions and
            // therefore their emitted count are exactly last frame's.
            int emitted = prefix > 0 ? _emittedBefore[prefix] : 0;
            int v = emitted * 4;
            int uploadStart = v;

            int middleEnd = count - suffix;
            int oldSuffixEmitStart = suffix > 0 ? _emittedBefore[middleEnd] : _lastEmitted;
            int oldMiddleEmitted = oldSuffixEmitStart - emitted;

            for (int q = prefix; q < middleEnd; q++)
            {
                if (EmitQuad(quads[q], ref v))
                {
                    emitted++;
                }

                _emittedBefore[q + 1] = emitted;
            }

            int totalEmitted;

            if (suffix > 0 && emitted - _emittedBefore[prefix] == oldMiddleEmitted)
            {
                // The middle emitted exactly as many quads as it replaced, so
                // the suffix vertices already sit at their final offsets and
                // _emittedBefore's suffix entries still tell the truth.
                totalEmitted = _lastEmitted;
            }
            else
            {
                // The tail slid (or the suffix claim was refused): re-emit it
                // at its new offsets. Its content is unchanged — emission is a
                // pure function of the quad — but its position is not.
                for (int q = middleEnd; q < count; q++)
                {
                    if (EmitQuad(quads[q], ref v))
                    {
                        emitted++;
                    }

                    _emittedBefore[q + 1] = emitted;
                }

                totalEmitted = emitted;
            }

            int uploadVerts = v - uploadStart;

            if (uploadVerts > 0)
            {
                _mesh.SetVertexBufferData(_vertices, uploadStart, uploadStart, uploadVerts, 0,
                                          MeshUpdateFlags.DontValidateIndices);
            }

            _mesh.SetSubMesh(0, new SubMeshDescriptor(0, totalEmitted * 6, MeshTopology.Triangles),
                             MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontValidateIndices);

            _mesh.bounds = new Bounds(new Vector3(documentSize.x * 0.5f, documentSize.y * 0.5f, 0f),
                                      new Vector3(documentSize.x, documentSize.y, 1f));

            QuadCount = totalEmitted;
            LastUploadedVertices = uploadVerts;
            UploadedKbTotal += uploadVerts * (double)BytesPerVertex / 1024d;
            _lastNativeCount = count;
            _lastEmitted = totalEmitted;
            _valid = true;
        }

        /// <summary>
        /// Writes one quad's four vertices at <paramref name="v"/>, or drops it
        /// (empty, fully transparent, or a border band clipped to nothing).
        /// A pure function of the quad's bytes — the ranged build depends on
        /// that: identical bytes must always emit identical vertices, or none.
        /// </summary>
        private bool EmitQuad(HtmlQuad quad, ref int v)
        {
            {
                if (quad.W <= 0f || quad.H <= 0f)
                {
                    return false;
                }

                // A fully transparent quad still costs fill rate; drop it here.
                if ((quad.Color >> 24) == 0 && quad.Type != HtmlQuadType.Image &&
                    quad.Type < HtmlQuadType.LinearGradient)
                {
                    return false;
                }

                float pad = quad.Type == HtmlQuadType.Glyph ? 0f : AntiAliasPad;

                float x0 = quad.X - pad;
                float y0 = quad.Y - pad;
                float x1 = quad.X + quad.W + pad;
                float y1 = quad.Y + quad.H + pad;

                // A border edge quad describes the whole element box (the
                // shader reconstructs the ring from the Rect field, which
                // stays the full box below), but it can only ever PAINT a band
                // along its own edge: as deep as the edge is thick or its two
                // corner arcs reach, plus the antialiasing falloff. Shrinking
                // the geometry to that band costs nothing and stops a bordered
                // element from paying four element-sized quads of fill for a
                // hairline ring — which measured as most of the overdraw on a
                // full redraw.
                if (quad.Type == HtmlQuadType.Border)
                {
                    const float aa = 2f;
                    switch ((int)quad.P0)
                    {
                        case 0: // top: corners tl, tr
                            y1 = Mathf.Min(y1, quad.Y + Mathf.Max(quad.BorderT, Mathf.Max(quad.Ry0, quad.Ry1)) + aa);
                            break;
                        case 1: // right: corners tr, br
                            x0 = Mathf.Max(x0, quad.X + quad.W
                                               - Mathf.Max(quad.BorderR, Mathf.Max(quad.Rx1, quad.Rx2)) - aa);
                            break;
                        case 2: // bottom: corners br, bl
                            y0 = Mathf.Max(y0, quad.Y + quad.H
                                               - Mathf.Max(quad.BorderB, Mathf.Max(quad.Ry2, quad.Ry3)) - aa);
                            break;
                        case 3: // left: corners tl, bl
                            x1 = Mathf.Min(x1, quad.X + Mathf.Max(quad.BorderL, Mathf.Max(quad.Rx0, quad.Rx3)) + aa);
                            break;
                    }

                    if (x1 <= x0 || y1 <= y0)
                    {
                        return false;
                    }
                }

                // UVs are mapped against the unpadded rect so glyphs land on
                // exactly their atlas cell.
                float du = (quad.U1 - quad.U0) / quad.W;
                float dv = (quad.V1 - quad.V0) / quad.H;

                float u0 = quad.U0 - pad * du;
                float v0 = quad.V0 - pad * dv;
                float u1 = quad.U1 + pad * du;
                float v1 = quad.V1 + pad * dv;

                var color = new Color32((byte)(quad.Color & 0xFF),
                                        (byte)((quad.Color >> 8) & 0xFF),
                                        (byte)((quad.Color >> 16) & 0xFF),
                                        (byte)((quad.Color >> 24) & 0xFF));

                var rect = new Vector4(quad.X + quad.W * 0.5f, quad.Y + quad.H * 0.5f,
                                       quad.W * 0.5f, quad.H * 0.5f);

                var radiusX = new Half4(quad.Rx0, quad.Rx1, quad.Rx2, quad.Rx3);
                var radiusY = new Half4(quad.Ry0, quad.Ry1, quad.Ry2, quad.Ry3);
                var border = new Half4(quad.BorderL, quad.BorderT, quad.BorderR, quad.BorderB);

                Vector4 clip = quad.IsClipped
                    ? new Vector4(quad.ClipX + quad.ClipW * 0.5f, quad.ClipY + quad.ClipH * 0.5f,
                                  quad.ClipW * 0.5f, quad.ClipH * 0.5f)
                    : new Vector4(0f, 0f, -1f, -1f);

                var clipR = new Half4(quad.ClipR0, quad.ClipR1, quad.ClipR2, quad.ClipR3);
                var parameters = new Vector4(quad.P0, quad.P1, quad.P2, quad.P3);

                var template = new Vertex
                {
                    Color = color,
                    Rect = rect,
                    RadiusX = radiusX,
                    RadiusY = radiusY,
                    Border = border,
                    Clip = clip,
                    ClipR = clipR,
                    Params = parameters,
                };

                float typeF = (float)quad.TypeRaw;
                float gradF = quad.GradRow;

                template.Position = new Vector2(x0, y0);
                template.Uv = new Vector4(u0, v0, typeF, gradF);
                _vertices[v + 0] = template;

                template.Position = new Vector2(x1, y0);
                template.Uv = new Vector4(u1, v0, typeF, gradF);
                _vertices[v + 1] = template;

                template.Position = new Vector2(x1, y1);
                template.Uv = new Vector4(u1, v1, typeF, gradF);
                _vertices[v + 2] = template;

                template.Position = new Vector2(x0, y1);
                template.Uv = new Vector4(u0, v1, typeF, gradF);
                _vertices[v + 3] = template;

                v += 4;
                return true;
            }
        }
    }
}
