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
    /// Builds the drag-and-drop inventory scene: a full-screen portrait page
    /// authored against 1080x1920.
    /// </summary>
    /// <remarks>
    /// A separate scene rather than another tab in the main demo, because that
    /// one is authored for a 1280x720 landscape panel and an inventory built for
    /// a portrait phone cannot share it without one of them being wrong.
    ///
    /// The panel stretches to fill the canvas here, which is what makes the
    /// layout resolution-independent: LiteHtmlRawImage sizes the surface at the
    /// canvas scale factor and feeds the same factor to DeviceScale, so one CSS
    /// pixel is one unit of the 1080x1920 reference on every screen.
    ///
    /// Headless:
    ///   Unity -batchmode -projectPath . \
    ///         -executeMethod LiteHtmlUnity.EditorTools.LiteHtmlInventorySceneBuilder.Build -quit
    /// </remarks>
    public static class LiteHtmlInventorySceneBuilder
    {
        public const string ScenePath = "Assets/LiteHtmlUnity/Samples/LiteHtmlInventoryDemo.unity";

        [MenuItem("Tools/LiteHtml/Build Inventory Demo Scene")]
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
            Debug.Log($"[LiteHtml] inventory scene written to {ScenePath} (view: {view.name})");
        }

        static void CreateCamera()
        {
            var go = new GameObject("Main Camera", typeof(Camera));
            go.tag = "MainCamera";

            Camera camera = go.GetComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color32(11, 15, 26, 255);
            camera.orthographic = true;

            go.transform.position = new Vector3(0f, 0f, -10f);
        }

        static void CreateEventSystem()
        {
            var go = new GameObject("EventSystem", typeof(EventSystem));

#if ENABLE_INPUT_SYSTEM
            go.AddComponent<InputSystemUIInputModule>();
#else
            go.AddComponent<StandaloneInputModule>();
#endif
        }

        static GameObject CreateUi()
        {
            var canvasGo = new GameObject("Canvas", typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));

            Canvas canvas = canvasGo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;

            CanvasScaler scaler = canvasGo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1080f, 1920f);

            // Match width: a taller phone gets more page, a shorter one less,
            // rather than the columns changing width under the layout.
            scaler.screenMatchMode = CanvasScaler.ScreenMatchMode.MatchWidthOrHeight;
            scaler.matchWidthOrHeight = 0f;

            var panelGo = new GameObject("LiteHtmlPanel", typeof(RectTransform), typeof(RawImage));
            panelGo.transform.SetParent(canvasGo.transform, false);

            // Stretched, not sized: the page is the screen.
            var rect = (RectTransform)panelGo.transform;
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.pivot = new Vector2(0.5f, 0.5f);
            rect.offsetMin = Vector2.zero;
            rect.offsetMax = Vector2.zero;

            RawImage raw = panelGo.GetComponent<RawImage>();
            raw.color = Color.white;

            panelGo.AddComponent<LiteHtmlView>();
            panelGo.AddComponent<LiteHtmlRawImage>();
            panelGo.AddComponent<LiteHtmlInventoryDemo>();

            return panelGo;
        }
    }
}
