using System.IO;
using Doctype.Samples;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

#if ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem.UI;
#endif

namespace Doctype.EditorTools
{
    /// <summary>
    /// Builds the drag-and-drop inventory scene: two portrait panels, each its
    /// own surface, drawn over a stand-in game.
    /// </summary>
    /// <remarks>
    /// A separate scene rather than another tab in the main demo, because that
    /// one is authored for a 1280x720 landscape panel and an inventory built for
    /// a portrait phone cannot share it without one of them being wrong.
    ///
    /// The bag is pinned to the top and the hotbar to the bottom, both stretched
    /// horizontally and both only as tall as their own page — the sample drives
    /// the height from each view's laid-out document. Nothing at all covers the
    /// band between them, which is what makes it empty rather than merely
    /// transparent: there is no surface there to swallow a touch and no texture
    /// there to composite.
    ///
    /// Dragging an item from one panel to the other therefore crosses a surface
    /// boundary, which is the point of the sample.
    ///
    /// Headless:
    ///   Unity -batchmode -projectPath . \
    ///         -executeMethod Doctype.EditorTools.HtmlInventorySceneBuilder.Build -quit
    /// </remarks>
    public static class HtmlInventorySceneBuilder
    {
        public const string ScenePath = "Assets/Doctype/Samples/HtmlInventoryDemo.unity";

        [MenuItem("Tools/Doctype/Build Inventory Demo Scene")]
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
                Debug.LogError($"[Doctype] could not save {ScenePath}");
                return;
            }

            AssetDatabase.Refresh();
            Debug.Log($"[Doctype] inventory scene written to {ScenePath} (view: {view.name})");
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

            // Match width: a taller phone gets more room between the panels,
            // rather than the columns changing width under the layout.
            scaler.screenMatchMode = CanvasScaler.ScreenMatchMode.MatchWidthOrHeight;
            scaler.matchWidthOrHeight = 0f;

            // The heights here are only a first guess. The sample replaces both
            // with whatever its pages lay out to, on the first frame and after
            // every reload, so a panel is never taller or shorter than its page.
            GameObject bag = CreatePanel(canvasGo, "BagPanel", top: true, inset: 56f, guessHeight: 820f);
            GameObject hotbar = CreatePanel(canvasGo, "HotbarPanel", top: false, inset: 48f, guessHeight: 280f);

            var hudGo = new GameObject("InventoryHud", typeof(RectTransform));
            hudGo.transform.SetParent(canvasGo.transform, false);

            var demo = hudGo.AddComponent<HtmlInventoryDemo>();
            demo.BagView = bag.GetComponent<HtmlView>();
            demo.HotbarView = hotbar.GetComponent<HtmlView>();

            return hudGo;
        }

        /// <summary>
        /// One panel: stretched across the screen with a margin, pinned to the
        /// top or the bottom, and sized to its own content from there.
        /// </summary>
        static GameObject CreatePanel(GameObject canvasGo, string name, bool top, float inset, float guessHeight)
        {
            const float SideMargin = 32f;

            var go = new GameObject(name, typeof(RectTransform), typeof(RawImage));
            go.transform.SetParent(canvasGo.transform, false);

            var rect = (RectTransform)go.transform;

            // Stretched horizontally, pinned vertically: the pivot sits on the
            // edge it is pinned to, so the height can grow away from that edge
            // without the panel drifting.
            rect.anchorMin = new Vector2(0f, top ? 1f : 0f);
            rect.anchorMax = new Vector2(1f, top ? 1f : 0f);
            rect.pivot = new Vector2(0.5f, top ? 1f : 0f);

            rect.sizeDelta = new Vector2(-SideMargin * 2f, guessHeight);
            rect.anchoredPosition = new Vector2(0f, top ? -inset : inset);

            go.GetComponent<RawImage>().color = Color.white;

            go.AddComponent<HtmlView>();

            // Neither panel fills its own rect right to the corners -- the page
            // is rounded -- and a HUD that swallows touches through its corners
            // is the kind of thing nobody notices until a device is in hand.
            go.AddComponent<HtmlRawImage>().PassThroughEmptyAreas = true;

            return go;
        }
    }
}
