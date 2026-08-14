using System.IO;
using Unity.Collections;
using UnityEditor;
using UnityEngine;

namespace Doctype.EditorTools
{
    /// <summary>
    /// Renders a page through the real GPU path and writes it to a PNG.
    /// </summary>
    /// <remarks>
    /// Its counterpart is Native/tests/harness.cpp, which renders the same file
    /// with the reference CPU rasterizer. Diffing the two images is how the
    /// shader is verified against its specification — pixel assertions in the
    /// test suite catch regressions, but only the image shows you *what* broke.
    ///
    /// Headless:
    ///   Unity -batchmode -projectPath . \
    ///         -executeMethod Doctype.EditorTools.HtmlCapture.CaptureDemo -quit
    /// </remarks>
    public static class HtmlCapture
    {
        private const int Width = 560;
        private const int Height = 425;

        [MenuItem("Tools/Doctype/Capture Demo Page")]
        public static void CaptureDemo()
        {
            string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            string htmlPath = Path.Combine(projectRoot, "Native/tests/demo.html");
            string outPath = Path.Combine(projectRoot, "Native/build/out/demo_gpu.png");

            if (!File.Exists(htmlPath))
            {
                Debug.LogError($"[Doctype] demo page not found at {htmlPath}");
                return;
            }

            Capture(File.ReadAllText(htmlPath), outPath, Width, Height, new Color32(18, 20, 28, 255));
        }

        /// <summary>Lays out and draws <paramref name="html"/>, writing a PNG to <paramref name="outputPath"/>.</summary>
        public static void Capture(string html, string outputPath, int width, int height, Color background)
        {
            using var document = new HtmlDocument();

            int fonts = 0;
            foreach (HtmlFontEntry entry in HtmlSystemFonts.Discover())
            {
                byte[] data = entry.Resolve();
                if (data != null && document.RegisterFont(entry.Family, data, entry.Weight, entry.Italic))
                {
                    fonts++;
                }
            }

            if (fonts == 0)
            {
                Debug.LogError("[Doctype] no system fonts available to capture with");
                return;
            }

            document.SetDefaultFont("sans-serif", 16f);
            document.SetViewport(width, height);

            if (!document.LoadHtml(html))
            {
                Debug.LogError($"[Doctype] {document.LastError}");
                return;
            }

            document.Layout(width);

            using var renderer = new HtmlRenderer();

            var target = new RenderTexture(width, height, 0, RenderTextureFormat.ARGB32,
                                           RenderTextureReadWrite.sRGB);
            target.Create();

            NativeArray<HtmlQuad> quads = document.Record(out HtmlFrame frame);
            renderer.Render(quads, frame, target, background, new Vector2(width, height));

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = target;

            var readback = new Texture2D(width, height, TextureFormat.RGBA32, false, false);
            readback.ReadPixels(new Rect(0, 0, width, height), 0, 0);
            readback.Apply(false, false);

            RenderTexture.active = previous;

            Directory.CreateDirectory(Path.GetDirectoryName(outputPath) ?? ".");
            File.WriteAllBytes(outputPath, readback.EncodeToPNG());

            Debug.Log($"[Doctype] captured {frame.QuadCount} quads " +
                      $"({renderer.LastQuadCount} drawn, {frame.DocWidth}x{frame.DocHeight} document) -> {outputPath}");

            Object.DestroyImmediate(readback);
            target.Release();
            Object.DestroyImmediate(target);
        }
    }
}
