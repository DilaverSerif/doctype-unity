using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace LiteHtmlUnity.EditorTools
{
    /// <summary>
    /// Builds the demo as an Android player from the command line.
    /// </summary>
    /// <remarks>
    /// <code>
    /// Unity -batchmode -quit -projectPath &lt;project&gt; \
    ///       -executeMethod LiteHtmlUnity.EditorTools.LiteHtmlBuild.Android \
    ///       -logFile build.log
    /// </code>
    /// Forces ARM64: the native plugin is only built for arm64-v8a, and a player
    /// that also targets armeabi-v7a would install on a 32-bit device and then
    /// fail to find the library at runtime.
    /// </remarks>
    public static class LiteHtmlBuild
    {
        const string DemoScene = "Assets/LiteHtmlUnity/Samples/LiteHtmlDemo.unity";
        const string OutputDir = "Native/build/out";

        public static void Android()
        {
            string scene = ResolveScene();
            string apk = Path.GetFullPath(Path.Combine(Application.dataPath, "..", OutputDir, "litehtml-demo.apk"));
            Directory.CreateDirectory(Path.GetDirectoryName(apk));

            // Assert rather than assume: an empty .meta leaves the native plugin
            // out of the player and the failure only appears on a device.
            LiteHtmlPluginSettings.ConfigureAll();
            LiteHtmlPluginSettings.EnsureShadersIncluded();

            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
            PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel24;
            PlayerSettings.SetScriptingBackend(NamedBuildTarget.Android, ScriptingImplementation.IL2CPP);
            PlayerSettings.SetApplicationIdentifier(NamedBuildTarget.Android, "com.dopaminefact.litehtmldemo");
            PlayerSettings.productName = "LiteHtml Demo";

            // The demo drives its own frame rate; leaving vsync on would hide
            // what the CPU numbers on the overview page are actually saying.
            QualitySettings.vSyncCount = 0;

            var options = new BuildPlayerOptions
            {
                scenes = new[] { scene },
                locationPathName = apk,
                target = BuildTarget.Android,
                targetGroup = BuildTargetGroup.Android,
                options = BuildOptions.None,
            };

            Debug.Log($"[LiteHtml] building {scene} -> {apk}");

            BuildReport report = BuildPipeline.BuildPlayer(options);
            BuildSummary summary = report.summary;

            if (summary.result != BuildResult.Succeeded)
            {
                // Batch mode returns 0 for a failed build unless the method
                // throws, which would leave a script reporting success.
                throw new Exception($"[LiteHtml] Android build {summary.result}: " +
                                    $"{summary.totalErrors} error(s)");
            }

            Debug.Log($"[LiteHtml] built {apk} ({summary.totalSize / 1048576f:0.0} MB) " +
                      $"in {summary.totalTime.TotalSeconds:0} s");
        }

        /// <summary>
        /// Prefers the demo scene, and says so rather than silently building
        /// whatever happens to be first in the build settings.
        /// </summary>
        static string ResolveScene()
        {
            if (File.Exists(DemoScene))
            {
                return DemoScene;
            }

            string[] enabled = EditorBuildSettings.scenes.Where(s => s.enabled).Select(s => s.path).ToArray();
            if (enabled.Length == 0)
            {
                throw new Exception($"[LiteHtml] {DemoScene} is missing and no scene is enabled in build settings");
            }

            Debug.LogWarning($"[LiteHtml] {DemoScene} not found; falling back to {enabled[0]}");
            return enabled[0];
        }
    }
}
