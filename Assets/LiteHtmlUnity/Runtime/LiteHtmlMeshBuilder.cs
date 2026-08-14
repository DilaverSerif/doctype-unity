using System;
using System.Runtime.InteropServices;
using Unity.Collections;
using UnityEngine;
using UnityEngine.Rendering;

namespace LiteHtmlUnity
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
    public sealed class LiteHtmlMeshBuilder : IDisposable
    {
        /// <summary>
        /// Vertex layout. 144 bytes: fat, but it keeps everything in one batch
        /// and avoids needing structured buffers (which GLES3 lacks).
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        private struct Vertex
        {
            public Vector3 Position; // document space, y down
            public Color32 Color;
            public Vector4 Uv;       // u, v, type, gradient row
            public Vector4 Rect;     // centre xy, half-size xy
            public Vector4 RadiusX;  // per corner: tl, tr, br, bl
            public Vector4 RadiusY;
            public Vector4 Border;   // left, top, right, bottom
            public Vector4 Clip;     // centre xy, half-size xy (x < 0 => unclipped)
            public Vector4 ClipR;    // per corner
            public Vector4 Params;   // border edge index / gradient geometry
        }

        private static readonly VertexAttributeDescriptor[] Layout =
        {
            new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.Float32, 3),
            new VertexAttributeDescriptor(VertexAttribute.Color, VertexAttributeFormat.UNorm8, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord0, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord1, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord2, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord3, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord4, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord5, VertexAttributeFormat.Float32, 4),
            new VertexAttributeDescriptor(VertexAttribute.TexCoord6, VertexAttributeFormat.Float32, 4),
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

        public LiteHtmlMeshBuilder()
        {
            _mesh = new Mesh { name = "LiteHtml", hideFlags = HideFlags.HideAndDontSave };
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
        public void Build(NativeArray<LiteHtmlQuad> quads, int count, Vector2 documentSize)
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
                LiteHtmlQuad quad = quads[q];

                if (quad.W <= 0f || quad.H <= 0f)
                {
                    continue;
                }

                // A fully transparent quad still costs fill rate; drop it here.
                if ((quad.Color >> 24) == 0 && quad.Type != LiteHtmlQuadType.Image &&
                    quad.Type < LiteHtmlQuadType.LinearGradient)
                {
                    continue;
                }

                float pad = quad.Type == LiteHtmlQuadType.Glyph ? 0f : AntiAliasPad;

                float x0 = quad.X - pad;
                float y0 = quad.Y - pad;
                float x1 = quad.X + quad.W + pad;
                float y1 = quad.Y + quad.H + pad;

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

                var radiusX = new Vector4(quad.Rx0, quad.Rx1, quad.Rx2, quad.Rx3);
                var radiusY = new Vector4(quad.Ry0, quad.Ry1, quad.Ry2, quad.Ry3);
                var border = new Vector4(quad.BorderL, quad.BorderT, quad.BorderR, quad.BorderB);

                Vector4 clip = quad.IsClipped
                    ? new Vector4(quad.ClipX + quad.ClipW * 0.5f, quad.ClipY + quad.ClipH * 0.5f,
                                  quad.ClipW * 0.5f, quad.ClipH * 0.5f)
                    : new Vector4(0f, 0f, -1f, -1f);

                var clipR = new Vector4(quad.ClipR0, quad.ClipR1, quad.ClipR2, quad.ClipR3);
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

                template.Position = new Vector3(x0, y0, 0f);
                template.Uv = new Vector4(u0, v0, typeF, gradF);
                _vertices[v + 0] = template;

                template.Position = new Vector3(x1, y0, 0f);
                template.Uv = new Vector4(u1, v0, typeF, gradF);
                _vertices[v + 1] = template;

                template.Position = new Vector3(x1, y1, 0f);
                template.Uv = new Vector4(u1, v1, typeF, gradF);
                _vertices[v + 2] = template;

                template.Position = new Vector3(x0, y1, 0f);
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
