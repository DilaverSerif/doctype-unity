using System.Collections;
using System.IO;
using LiteHtmlUnity.Samples;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using UnityEngine.UI;

namespace LiteHtmlUnity.Tests
{
    /// <summary>
    /// Captures what the demo actually looks like while the game is running.
    /// </summary>
    /// <remarks>
    /// Lives in the test assembly because entering Play mode headlessly is
    /// something only the test runner does reliably in batch mode. It asserts
    /// too, so a broken capture fails rather than silently writing a blank PNG.
    /// </remarks>
    public class LiteHtmlRuntimeCapture
    {
        [UnityTest]
        public IEnumerator CaptureRunningDemo()
        {
            var camera = new GameObject("Camera", typeof(Camera));
            Camera cam = camera.GetComponent<Camera>();
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color32(9, 10, 14, 255);

            var canvasGo = new GameObject("Canvas", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            canvasGo.GetComponent<Canvas>().renderMode = RenderMode.ScreenSpaceOverlay;

            var panel = new GameObject("Panel", typeof(RectTransform), typeof(RawImage));
            panel.transform.SetParent(canvasGo.transform, false);
            ((RectTransform)panel.transform).sizeDelta = new Vector2(640f, 430f);

            var view = panel.AddComponent<LiteHtmlView>();
            panel.AddComponent<LiteHtmlRawImage>();
            panel.AddComponent<FrameRateLimiter>();
            var controller = panel.AddComponent<LiteHtmlDemoController>();

            // Let the live-stats page settle so the numbers on it are real.
            float deadline = Time.unscaledTime + 4f;
            while (Time.unscaledTime < deadline && controller.Rebuilds < 8)
            {
                yield return null;
            }

            Assert.IsNotNull(view.Texture, "no surface to capture");
            Assert.Greater(view.QuadCount, 20, "the page looks empty; capturing it would prove nothing");

            string dir = Path.GetFullPath(Path.Combine(Application.dataPath, "../Native/build/out"));
            Directory.CreateDirectory(dir);

            foreach (LiteHtmlDemoController.Page page in
                     System.Enum.GetValues(typeof(LiteHtmlDemoController.Page)))
            {
                controller.GoTo(page);

                // Past the 0.22s enter transition so the page is fully settled.
                float until = Time.unscaledTime + 0.5f;
                while (Time.unscaledTime < until)
                {
                    yield return null;
                }

                string path = Path.Combine(dir, $"demo_{page.ToString().ToLowerInvariant()}.png");
                Capture(view, path);

                Debug.Log($"[LiteHtml] captured {page}: {view.QuadCount} quads, " +
                          $"{view.TotalMs:0.00} ms rebuild -> {path}");
            }

            // A second frame of the animated page, to show it actually moves.
            controller.GoTo(LiteHtmlDemoController.Page.Animation);
            float wait = Time.unscaledTime + 0.45f;
            while (Time.unscaledTime < wait)
            {
                yield return null;
            }

            Capture(view, Path.Combine(dir, "demo_animation_b.png"));

            // Keep the original name pointing at the entry page.
            Capture(view, Path.Combine(dir, "demo_runtime.png"), LiteHtmlDemoController.Page.Overview, controller);
            yield return null;
            Object.DestroyImmediate(canvasGo);
            Object.DestroyImmediate(camera);
        }

        private static void Capture(LiteHtmlView view, string path,
                                    LiteHtmlDemoController.Page? goTo = null,
                                    LiteHtmlDemoController controller = null)
        {
            if (goTo.HasValue && controller != null)
            {
                controller.GoTo(goTo.Value);
            }

            RenderTexture target = view.Texture;
            var readback = new Texture2D(target.width, target.height, TextureFormat.RGBA32, false, false);

            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = target;
            readback.ReadPixels(new Rect(0, 0, target.width, target.height), 0, 0);
            readback.Apply(false, false);
            RenderTexture.active = previous;

            File.WriteAllBytes(path, readback.EncodeToPNG());
            Object.DestroyImmediate(readback);
        }
    }
}
