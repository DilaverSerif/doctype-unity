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

        /// <summary>Number of quads in the last built mesh.</summary>
        public int QuadCount { get; private set; }

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
            _capacityQuads = capacity;
        }

        /// <summary>
        /// Rebuilds the mesh from a recorded frame.
        /// </summary>
        public void Build(NativeArray<HtmlQuad> quads, int count, Vector2 documentSize)
        {
            QuadCount = 0;

            if (count <= 0 || !quads.IsCreated)
            {
                _mesh.Clear();
                return;
            }

            EnsureCapacity(count);

            int v = 0;
            int i = 0;

            for (int q = 0; q < count; q++)
            {
                HtmlQuad quad = quads[q];

                if (quad.W <= 0f || quad.H <= 0f)
                {
                    continue;
                }

                // A fully transparent quad still costs fill rate; drop it here.
                if ((quad.Color >> 24) == 0 && quad.Type != HtmlQuadType.Image &&
                    quad.Type < HtmlQuadType.LinearGradient)
                {
                    continue;
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
                        continue;
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

                _indices[i + 0] = (uint)(v + 0);
                _indices[i + 1] = (uint)(v + 1);
                _indices[i + 2] = (uint)(v + 2);
                _indices[i + 3] = (uint)(v + 0);
                _indices[i + 4] = (uint)(v + 2);
                _indices[i + 5] = (uint)(v + 3);

                v += 4;
                i += 6;
                QuadCount++;
            }

            _mesh.Clear(true);

            if (v == 0)
            {
                return;
            }

            _mesh.SetVertexBufferParams(v, Layout);
            _mesh.SetVertexBufferData(_vertices, 0, 0, v, 0, MeshUpdateFlags.DontValidateIndices);

            _mesh.SetIndexBufferParams(i, IndexFormat.UInt32);
            _mesh.SetIndexBufferData(_indices, 0, 0, i, MeshUpdateFlags.DontValidateIndices);

            _mesh.subMeshCount = 1;
            _mesh.SetSubMesh(0, new SubMeshDescriptor(0, i, MeshTopology.Triangles),
                             MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontValidateIndices);

            _mesh.bounds = new Bounds(new Vector3(documentSize.x * 0.5f, documentSize.y * 0.5f, 0f),
                                      new Vector3(documentSize.x, documentSize.y, 1f));
        }
    }
}
