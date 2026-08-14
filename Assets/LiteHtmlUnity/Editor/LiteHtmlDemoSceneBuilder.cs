using System.IO;
using LiteHtmlUnity.Samples;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

#if ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem.UI;
#endif

namespace LiteHtmlUnity.EditorTools
{
    /// <summary>
    /// Builds the demo scene from scratch.
    /// </summary>
    /// <remarks>
    /// The scene is generated rather than hand-authored so it stays diffable and
    /// can be rebuilt after any API change — a .unity file full of serialized
    /// GUIDs is not something you can review.
    ///
    /// Headless:
    ///   Unity -batchmode -projectPath . \
    ///         -executeMethod LiteHtmlUnity.EditorTools.LiteHtmlDemoSceneBuilder.Build -quit
    /// </remarks>
    public static class LiteHtmlDemoSceneBuilder
    {
        public const string ScenePath = "Assets/LiteHtmlUnity/Samples/LiteHtmlDemo.unity";

        [MenuItem("Tools/LiteHtml/Build Demo Scene")]
        public static void Build()
        {
            Scene scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            CreateCamera();
            CreateEventSystem();
            GameObject view = CreateUi();

            Directory.CreateDirectory(Path.GetDirectoryName(ScenePath) ?? ".");
            EditorSceneManager.MarkSceneDirty(scene);

            if (!EditorSceneManager.SaveScene(scene, ScenePath))
            {
                Debug.LogError($"[LiteHtml] could not save {ScenePath}");
                return;
            }

            AssetDatabase.Refresh();
            Debug.Log($"[LiteHtml] demo scene written to {ScenePath} (view: {view.name})");
        }

        private static void CreateCamera()
        {
            var go = new GameObject("Main Camera", typeof(Camera));
            go.tag = "MainCamera";

            Camera camera = go.GetComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color32(9, 10, 14, 255);
            camera.orthographic = true;

            go.transform.position = new Vector3(0f, 0f, -10f);
        }

        private static void CreateEventSystem()
        {
            var go = new GameObject("EventSystem", typeof(EventSystem));

            // The project is set to the new Input System, where the legacy
            // StandaloneInputModule throws on enable.
#if ENABLE_INPUT_SYSTEM
            go.AddComponent<InputSystemUIInputModule>();
#else
            go.AddComponent<StandaloneInputModule>();
#endif
        }

        private static GameObject CreateUi()
        {
            var canvasGo = new GameObject("Canvas", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));

            Canvas canvas = canvasGo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;

            CanvasScaler scaler = canvasGo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1280f, 720f);

            var panelGo = new GameObject("LiteHtmlPanel", typeof(RectTransform), typeof(RawImage));
            panelGo.transform.SetParent(canvasGo.transform, false);

            var rect = (RectTransform)panelGo.transform;
            rect.anchorMin = new Vector2(0.5f, 0.5f);
            rect.anchorMax = new Vector2(0.5f, 0.5f);
            rect.pivot = new Vector2(0.5f, 0.5f);
            rect.sizeDelta = new Vector2(680f, 460f);
            rect.anchoredPosition = Vector2.zero;

            RawImage raw = panelGo.GetComponent<RawImage>();
            // The view's own background paints the page; the RawImage is only a
            // surface, so it must not tint what lands on it.
            raw.color = Color.white;

            panelGo.AddComponent<LiteHtmlView>();
            panelGo.AddComponent<LiteHtmlRawImage>();

            // The limiter lives on the same object so the demo's Performance
            // page can find and drive it.
            panelGo.AddComponent<FrameRateLimiter>();
            panelGo.AddComponent<LiteHtmlDemoController>();

            return panelGo;
        }
    }
}
