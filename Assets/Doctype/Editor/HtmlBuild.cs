using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;
using UnityEngine.Rendering;

namespace Doctype.EditorTools
{
    /// <summary>
    /// Builds the demo as an Android player from the command line.
    /// </summary>
    /// <remarks>
    /// <code>
    /// Unity -batchmode -quit -projectPath &lt;project&gt; \
    ///       -executeMethod Doctype.EditorTools.HtmlBuild.Android \
    ///       -logFile build.log
    /// </code>
    /// Forces ARM64: the native plugin is only built for arm64-v8a, and a player
    /// that also targets armeabi-v7a would install on a 32-bit device and then
    /// fail to find the library at runtime.
    /// </remarks>
    public static class HtmlBuild
    {
        const string DemoScene = "Assets/Doctype/Samples/HtmlDemo.unity";
        const string InventoryScene = "Assets/Doctype/Samples/HtmlInventoryDemo.unity";
        const string BenchmarkScene = "Assets/Doctype/Samples/HtmlBenchmark.unity";
        const string OutputDir = "Native/build/out";

        /// <summary>
        /// The portrait drag-and-drop inventory, as its own package so it can sit
        /// on a device next to the main demo rather than replacing it.
        /// </summary>
        [MenuItem("Tools/Doctype/Build Inventory APK")]
        public static void AndroidInventory()
        {
            Build(InventoryScene, "com.dopaminefact.doctypeinventory", "Doctype Inventory",
                  "doctype-inventory.apk", UIOrientation.Portrait);
        }

        /// <summary>
        /// The on-device benchmark. Its own package so it can be left installed
        /// next to the demos and re-run whenever something in the pipeline
        /// changes.
        /// </summary>
        [MenuItem("Tools/Doctype/Build Benchmark APK")]
        public static void AndroidBenchmark()
        {
            Build(BenchmarkScene, "com.dopaminefact.doctypebench", "Doctype Bench",
                  "doctype-bench.apk", UIOrientation.Portrait, forMeasurement: true);
        }

        [MenuItem("Tools/Doctype/Build Demo APK")]
        public static void Android()
        {
            Build(ResolveScene(), "com.dopaminefact.doctypedemo", "Doctype Demo",
                  "doctype-demo.apk", UIOrientation.AutoRotation);
        }

        static void Build(string scene, string bundleId, string product, string apkName,
                          UIOrientation orientation, bool forMeasurement = false)
        {
            string apk = Path.GetFullPath(Path.Combine(Application.dataPath, "..", OutputDir, apkName));
            Directory.CreateDirectory(Path.GetDirectoryName(apk));

            // Assert rather than assume: an empty .meta leaves the native plugin
            // out of the player and the failure only appears on a device.
            AssertPluginIsCurrent();
            HtmlPluginSettings.ConfigureAll();
            HtmlPluginSettings.EnsureShadersIncluded();

            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
            PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel24;
            PlayerSettings.SetScriptingBackend(NamedBuildTarget.Android, ScriptingImplementation.IL2CPP);
            PlayerSettings.SetApplicationIdentifier(NamedBuildTarget.Android, bundleId);
            PlayerSettings.productName = product;
            PlayerSettings.defaultInterfaceOrientation = orientation;

            // The demo drives its own frame rate; leaving vsync on would hide
            // what the CPU numbers on the overview page are actually saying.
            QualitySettings.vSyncCount = 0;

            // Vulkan first on every Android build, GLES3 only as a fallback.
            // Two reasons, and the second is the one that made this necessary:
            // it is what a game shipping today runs on, and it is the only one
            // of the two that answers FrameTimingManager. Under GLES3 every GPU
            // figure the benchmark reports is "n/a", which is exactly the number
            // the whole exercise exists to find.
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.Android,
                                           new[] { GraphicsDeviceType.Vulkan, GraphicsDeviceType.OpenGLES3 });

            // FrameTimingManager returns nothing at all unless this is on, on
            // every platform and every graphics API. Without it the benchmark's
            // GPU column is "n/a" from top to bottom, which reads like the
            // device refused to answer rather than like nobody asked.
            PlayerSettings.enableFrameTimingStats = forMeasurement;

            // Unity paces frames to a whole divisor of the refresh rate. On a
            // phone that misses 16.7 ms once, that means a hard lock to 30 fps —
            // and then every scenario cheaper than 33 ms reports 33 ms and the
            // frame-time column says nothing. A shipping game wants the pacer;
            // a measurement of one does not.
            PlayerSettings.Android.optimizedFramePacing = !forMeasurement;

            var options = new BuildPlayerOptions
            {
                scenes = new[] { scene },
                locationPathName = apk,
                target = BuildTarget.Android,
                targetGroup = BuildTargetGroup.Android,
                options = BuildOptions.None,
            };

            Debug.Log($"[Doctype] building {scene} -> {apk}");

            BuildReport report = BuildPipeline.BuildPlayer(options);
            BuildSummary summary = report.summary;

            if (summary.result != BuildResult.Succeeded)
            {
                // Batch mode returns 0 for a failed build unless the method
                // throws, which would leave a script reporting success.
                throw new Exception($"[Doctype] Android build {summary.result}: " +
                                    $"{summary.totalErrors} error(s)");
            }

            Debug.Log($"[Doctype] built {apk} ({summary.totalSize / 1048576f:0.0} MB) " +
                      $"in {summary.totalTime.TotalSeconds:0} s");
        }

        /// <summary>
        /// Refuses to build against a native plugin older than the sources it
        /// was compiled from.
        /// </summary>
        /// <remarks>
        /// Nothing rebuilds the .so as part of a player build, so adding a
        /// native entry point and then building an APK ships a library without
        /// it. That surfaces as EntryPointNotFoundException at the call site,
        /// which — if the call is inside a UI event — swallows the rest of the
        /// event and looks like the interaction is broken rather than the
        /// library being stale.
        /// </remarks>
        static void AssertPluginIsCurrent()
        {
            string root = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            string plugin = Path.Combine(root, "Assets/Doctype/Plugins/Android/libs/arm64-v8a/libDoctype.so");

            if (!File.Exists(plugin))
            {
                throw new Exception("[Doctype] Android plugin missing. Run Native/build_android.sh arm64-v8a");
            }

            DateTime built = File.GetLastWriteTimeUtc(plugin);
            var sources = new List<string>();

            foreach (string dir in new[] { "Native/src", "Native/third_party/litehtml/src",
                                           "Native/third_party/litehtml/include" })
            {
                string full = Path.Combine(root, dir);
                if (Directory.Exists(full))
                {
                    sources.AddRange(Directory.GetFiles(full, "*.*", SearchOption.AllDirectories));
                }
            }

            foreach (string file in sources)
            {
                if (File.GetLastWriteTimeUtc(file) > built)
                {
                    throw new Exception($"[Doctype] {Path.GetFileName(file)} is newer than the Android plugin. " +
                                        "Run Native/build_android.sh arm64-v8a first.");
                }
            }
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
                throw new Exception($"[Doctype] {DemoScene} is missing and no scene is enabled in build settings");
            }

            Debug.LogWarning($"[Doctype] {DemoScene} not found; falling back to {enabled[0]}");
            return enabled[0];
        }
    }
}
