using UnityEditor;
using UnityEngine;

namespace LiteHtmlUnity.EditorTools
{
    /// <summary>
    /// Keeps the native plugins' import settings correct.
    /// </summary>
    /// <remarks>
    /// A freshly written .so arrives with an empty .meta: compatible with
    /// nothing, so Unity silently leaves it out of the player. The build then
    /// succeeds, installs, and dies on the first P/Invoke with a
    /// DllNotFoundException — a failure that only shows up on a device.
    /// <para>
    /// Applied on import so a rebuild of the native code cannot reintroduce it,
    /// and callable directly so a build can assert it rather than assume it.
    /// </para>
    /// </remarks>
    public sealed class LiteHtmlPluginSettings : AssetPostprocessor
    {
        const string PluginRoot = "Assets/LiteHtmlUnity/Plugins/";

        void OnPreprocessAsset()
        {
            if (!assetPath.StartsWith(PluginRoot) || assetImporter is not PluginImporter importer)
            {
                return;
            }

            Apply(importer, assetPath);
        }

        /// <summary>Re-applies the settings to every native plugin in the package.</summary>
        [MenuItem("Tools/LiteHtml/Fix native plugin import settings")]
        public static void ConfigureAll()
        {
            int fixedUp = 0;

            foreach (string guid in AssetDatabase.FindAssets("", new[] { "Assets/LiteHtmlUnity/Plugins" }))
            {
                string path = AssetDatabase.GUIDToAssetPath(guid);
                if (AssetImporter.GetAtPath(path) is not PluginImporter importer)
                {
                    continue;
                }

                Apply(importer, path);
                importer.SaveAndReimport();
                fixedUp++;
            }

            Debug.Log($"[LiteHtml] configured {fixedUp} native plugin(s)");
        }

        /// <summary>
        /// Names of the shaders resolved with Shader.Find at runtime.
        /// </summary>
        /// <remarks>
        /// Nothing in the project references these through a material asset, so
        /// a player build strips them and the page renders as a blank surface —
        /// an editor that works and a device that does not. Adding them to
        /// Always Included Shaders is what keeps them in the build.
        /// </remarks>
        static readonly string[] RuntimeShaders =
        {
            "LiteHtmlUnity/Quad",
            "LiteHtml/Composite",
        };

        /// <summary>Adds the runtime shaders to Graphics Settings if missing.</summary>
        [MenuItem("Tools/LiteHtml/Ensure shaders ship with the player")]
        public static void EnsureShadersIncluded()
        {
            var settings = new SerializedObject(
                AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/GraphicsSettings.asset")[0]);
            SerializedProperty included = settings.FindProperty("m_AlwaysIncludedShaders");

            foreach (string name in RuntimeShaders)
            {
                Shader shader = Shader.Find(name);
                if (shader == null)
                {
                    Debug.LogError($"[LiteHtml] shader '{name}' is missing from the project entirely.");
                    continue;
                }

                bool present = false;
                for (int i = 0; i < included.arraySize; i++)
                {
                    if (included.GetArrayElementAtIndex(i).objectReferenceValue == shader)
                    {
                        present = true;
                        break;
                    }
                }

                if (present)
                {
                    continue;
                }

                included.InsertArrayElementAtIndex(included.arraySize);
                included.GetArrayElementAtIndex(included.arraySize - 1).objectReferenceValue = shader;
                Debug.Log($"[LiteHtml] added '{name}' to Always Included Shaders");
            }

            settings.ApplyModifiedProperties();
            AssetDatabase.SaveAssets();
        }

        static void Apply(PluginImporter importer, string path)
        {
            importer.SetCompatibleWithAnyPlatform(false);

            if (path.Contains("/Android/"))
            {
                // ARM64 only: that is the one ABI the native build produces, and
                // claiming another would install on a device the library cannot
                // load on.
                importer.SetCompatibleWithPlatform(BuildTarget.Android, true);
                importer.SetPlatformData(BuildTarget.Android, "CPU", "ARM64");
            }
            else if (path.Contains("/iOS/"))
            {
                importer.SetCompatibleWithPlatform(BuildTarget.iOS, true);
                importer.SetPlatformData(BuildTarget.iOS, "CPU", "ARM64");
            }
            else if (path.Contains("/macOS/"))
            {
                // The editor loads this one too, which is what the tests use.
                importer.SetCompatibleWithEditor(true);
                importer.SetEditorData("OS", "OSX");
                importer.SetCompatibleWithPlatform(BuildTarget.StandaloneOSX, true);
            }
        }
    }
}
