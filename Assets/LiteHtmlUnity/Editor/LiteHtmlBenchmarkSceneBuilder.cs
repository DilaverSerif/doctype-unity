using System.IO;
using LiteHtmlUnity.Samples;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace LiteHtmlUnity.EditorTools
{
    /// <summary>
    /// Builds the on-device benchmark scene: a spinning-cube load with four HUD
    /// panels tiled over it.
    /// </summary>
    /// <remarks>
    /// Four quarter-screen panels rather than one: the scenarios step from one
    /// panel to four, and tiling them means the four-panel case covers exactly
    /// the screen. That makes it an upper bound anyone can reason about — "a HUD
    /// over the whole display" — instead of an arbitrary amount of surface.
    ///
    /// Nothing here is interactive. The benchmark drives every scenario itself
    /// and writes a CSV; a person only has to launch it and read the result.
    ///
    /// Headless:
    ///   Unity -batchmode -projectPath . \
    ///         -executeMethod LiteHtmlUnity.EditorTools.LiteHtmlBenchmarkSceneBuilder.Build -quit
    /// </remarks>
    public static class LiteHtmlBenchmarkSceneBuilder
    {
        public const string ScenePath = "Assets/LiteHtmlUnity/Samples/LiteHtmlBenchmark.unity";

        [MenuItem("Tools/LiteHtml/Build Benchmark Scene")]
        public static void Build()
        {
            Scene scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            CreateLoad();
            CreateUi();

            Directory.CreateDirectory(Path.GetDirectoryName(ScenePath) ?? ".");
            EditorSceneManager.MarkSceneDirty(scene);

            if (!EditorSceneManager.SaveScene(scene, ScenePath))
            {
                Debug.LogError($"[LiteHtml] could not save {ScenePath}");
                return;
            }

            AssetDatabase.Refresh();
            Debug.Log($"[LiteHtml] benchmark scene written to {ScenePath}");
        }

        static void CreateLoad()
        {
            var root = new GameObject("Load", typeof(LiteHtmlBenchmarkLoad));

            var cameraGo = new GameObject("Main Camera", typeof(Camera));
            cameraGo.tag = "MainCamera";
            cameraGo.transform.SetParent(root.transform, false);

            Camera camera = cameraGo.GetComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color32(14, 18, 28, 255);
            camera.farClipPlane = 200f;

            var lightGo = new GameObject("Light", typeof(Light));
            lightGo.transform.SetParent(root.transform, false);
            lightGo.transform.rotation = Quaternion.Euler(35f, -25f, 0f);

            Light light = lightGo.GetComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.1f;
        }

        static void CreateUi()
        {
            var canvasGo = new GameObject("Canvas", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));

            Canvas canvas = canvasGo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;

            CanvasScaler scaler = canvasGo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1080f, 1920f);
            scaler.screenMatchMode = CanvasScaler.ScreenMatchMode.MatchWidthOrHeight;
            scaler.matchWidthOrHeight = 0f;

            var hudGo = new GameObject("Benchmark", typeof(RectTransform));
            hudGo.transform.SetParent(canvasGo.transform, false);

            // Stretched, and not for looks: the panels below anchor to quadrants
            // of *this* rect. Left at its default zero size they become four
            // slivers around the canvas centre, and every number the benchmark
            // reports is then about a HUD nobody would ship.
            var hudRect = (RectTransform)hudGo.transform;
            hudRect.anchorMin = Vector2.zero;
            hudRect.anchorMax = Vector2.one;
            hudRect.offsetMin = Vector2.zero;
            hudRect.offsetMax = Vector2.zero;

            // Quadrants, in the order the scenarios switch them on.
            CreatePanel(hudGo, "Panel0", new Vector2(0f, 0.5f), new Vector2(0.5f, 1f));
            CreatePanel(hudGo, "Panel1", new Vector2(0.5f, 0.5f), new Vector2(1f, 1f));
            CreatePanel(hudGo, "Panel2", new Vector2(0f, 0f), new Vector2(0.5f, 0.5f));
            CreatePanel(hudGo, "Panel3", new Vector2(0.5f, 0f), new Vector2(1f, 0.5f));

            hudGo.AddComponent<LiteHtmlBenchmark>();
        }

        static void CreatePanel(GameObject parent, string name, Vector2 anchorMin, Vector2 anchorMax)
        {
            var go = new GameObject(name, typeof(RectTransform), typeof(RawImage),
                                    typeof(LiteHtmlView), typeof(LiteHtmlRawImage));
            go.transform.SetParent(parent.transform, false);

            var rect = (RectTransform)go.transform;
            rect.anchorMin = anchorMin;
            rect.anchorMax = anchorMax;

            // A small inset so the quadrants read as four panels rather than one
            // sheet, and so each has a transparent border to composite.
            rect.offsetMin = new Vector2(12f, 12f);
            rect.offsetMax = new Vector2(-12f, -12f);

            go.GetComponent<RawImage>().color = Color.white;
        }
    }
}
