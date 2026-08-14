using System.IO;
using Unity.Collections;
using UnityEditor;
using UnityEngine;

namespace LiteHtmlUnity.EditorTools
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
    ///         -executeMethod LiteHtmlUnity.EditorTools.LiteHtmlCapture.CaptureDemo -quit
    /// </remarks>
    public static class LiteHtmlCapture
    {
        private const int Width = 560;
        private const int Height = 425;

        [MenuItem("Tools/LiteHtml/Capture Demo Page")]
        public static void CaptureDemo()
        {
            string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            string htmlPath = Path.Combine(projectRoot, "Native/tests/demo.html");
            string outPath = Path.Combine(projectRoot, "Native/build/out/demo_gpu.png");

            if (!File.Exists(htmlPath))
            {
                Debug.LogError($"[LiteHtml] demo page not found at {htmlPath}");
                return;
            }

            Capture(File.ReadAllText(htmlPath), outPath, Width, Height, new Color32(18, 20, 28, 255));
        }

        /// <summary>Lays out and draws <paramref name="html"/>, writing a PNG to <paramref name="outputPath"/>.</summary>
        public static void Capture(string html, string outputPath, int width, int height, Color background)
        {
            using var document = new LiteHtmlDocument();

            int fonts = 0;
            foreach (LiteHtmlFontEntry entry in LiteHtmlSystemFonts.Discover())
            {
                byte[] data = entry.Resolve();
                if (data != null && document.RegisterFont(entry.Family, data, entry.Weight, entry.Italic))
                {
                    fonts++;
                }
            }

            if (fonts == 0)
            {
                Debug.LogError("[LiteHtml] no system fonts available to capture with");
                return;
            }

            document.SetDefaultFont("sans-serif", 16f);
            document.SetViewport(width, height);

            if (!document.LoadHtml(html))
            {
                Debug.LogError($"[LiteHtml] {document.LastError}");
                return;
            }

            document.Layout(width);

            using var renderer = new LiteHtmlRenderer();

            var target = new RenderTexture(width, height, 0, RenderTextureFormat.ARGB32,
                                           RenderTextureReadWrite.sRGB);
            target.Create();

            NativeArray<LiteHtmlQuad> quads = document.Record(out LiteHtmlFrame frame);
            renderer.Render(quads, frame, target, background, new Vector2(width, height));

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = target;

            var readback = new Texture2D(width, height, TextureFormat.RGBA32, false, false);
            readback.ReadPixels(new Rect(0, 0, width, height), 0, 0);
            readback.Apply(false, false);

            RenderTexture.active = previous;

            Directory.CreateDirectory(Path.GetDirectoryName(outputPath) ?? ".");
            File.WriteAllBytes(outputPath, readback.EncodeToPNG());

            Debug.Log($"[LiteHtml] captured {frame.QuadCount} quads " +
                      $"({renderer.LastQuadCount} drawn, {frame.DocWidth}x{frame.DocHeight} document) -> {outputPath}");

            Object.DestroyImmediate(readback);
            target.Release();
            Object.DestroyImmediate(target);
        }
    }
}
