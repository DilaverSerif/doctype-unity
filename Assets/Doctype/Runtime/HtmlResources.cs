using System;
using System.Collections.Generic;
using UnityEngine;

namespace Doctype
{
    /// <summary>
    /// Supplies images to a document from ordinary Unity textures, so markup can
    /// say <c>&lt;img src="gold"&gt;</c> and get the project's own icon.
    /// </summary>
    /// <remarks>
    /// Every image UV the engine emits points into one atlas, because the whole
    /// page is drawn with a single material in a single draw call. This packs the
    /// textures it is given into that atlas and hands back the rectangles.
    /// <para>
    /// Source textures must have Read/Write enabled in their import settings —
    /// packing reads their pixels. A texture without it is reported by name
    /// rather than silently skipped.
    /// </para>
    /// </remarks>
    [AddComponentMenu("Doctype/Resources")]
    public sealed class HtmlResources : MonoBehaviour, IHtmlResourceProvider
    {
        [Serializable]
        public struct ImageEntry
        {
            [Tooltip("The name markup refers to, e.g. gold for <img src=\"gold\">.")]
            public string Name;

            public Texture2D Texture;
        }

        [Tooltip("Images this document may use, named as the markup refers to them.")]
        [SerializeField] private ImageEntry[] _images = Array.Empty<ImageEntry>();

        [Tooltip("Also look under a Resources folder when a name is not in the list above.")]
        [SerializeField] private bool _loadFromResources = true;

        [Tooltip("Upper bound for the packed atlas. Packing fails loudly rather than silently " +
                 "dropping images that do not fit.")]
        [SerializeField] private int _maxAtlasSize = 2048;

        [SerializeField, Range(0, 8)] private int _padding = 2;

        private readonly Dictionary<string, Texture2D> _sources = new Dictionary<string, Texture2D>();
        private readonly Dictionary<string, Rect> _uvs = new Dictionary<string, Rect>();
        private readonly HashSet<string> _missing = new HashSet<string>();

        private Texture2D _atlas;
        private int _version;

        // Source count the last refused pack saw. BeginLoadImage retries the
        // pack for every still-unpacked url, so without this a refusal would
        // re-run and re-log once per record until the end of time.
        private int _refusedAtSourceCount = -1;

        /// <inheritdoc />
        public int Version => _version;

        /// <inheritdoc />
        public Texture ImageAtlas => _atlas;

        /// <summary>
        /// Upper bound for the packed atlas, exposed so tests and runtime code
        /// can configure it. Packing is refused loudly when the sources do not
        /// fit at full resolution; see <see cref="Repack"/>.
        /// </summary>
        public int MaxAtlasSize
        {
            get => _maxAtlasSize;
            set
            {
                int wanted = Mathf.Max(1, value);
                if (_maxAtlasSize == wanted)
                {
                    return;
                }

                _maxAtlasSize = wanted;

                // A standing refusal was judged under the old limit; the error
                // message tells the user to raise it, so raising it has to
                // actually reopen the attempt.
                _refusedAtSourceCount = -1;
            }
        }

        private void Awake()
        {
            foreach (ImageEntry entry in _images)
            {
                if (!string.IsNullOrEmpty(entry.Name) && entry.Texture != null)
                {
                    _sources[entry.Name] = entry.Texture;
                }
            }
        }

        private void OnDestroy()
        {
            DestroyTexture(_atlas);
            _atlas = null;
        }

        private static void DestroyTexture(Texture2D texture)
        {
            if (texture == null)
            {
                return;
            }

#if UNITY_EDITOR
            if (Application.isPlaying)
            {
                Destroy(texture);
            }
            else
            {
                DestroyImmediate(texture);
            }
#else
            Destroy(texture);
#endif
        }

        /// <summary>
        /// Makes a texture available under a name at runtime, for images that are
        /// not known when the scene is authored.
        /// </summary>
        public void Register(string name, Texture2D texture)
        {
            if (string.IsNullOrEmpty(name) || texture == null)
            {
                return;
            }

            _sources[name] = texture;
            _missing.Remove(name);

            // A new or replaced source changes what a pack would produce, so a
            // standing refusal no longer describes the world.
            _refusedAtSourceCount = -1;

            // Already packed under this name: the atlas has to be rebuilt or the
            // old picture would keep being drawn.
            if (_uvs.Remove(name))
            {
                Repack();
            }
        }

        /// <inheritdoc />
        public bool TryGetImageSize(string url, out int width, out int height)
        {
            width = 0;
            height = 0;

            Texture2D source = Resolve(url);
            if (source == null)
            {
                return false;
            }

            // Answered from the source, not the atlas: layout asks for this
            // before anything has been packed.
            width = source.width;
            height = source.height;
            return true;
        }

        /// <inheritdoc />
        public void BeginLoadImage(string url)
        {
            if (string.IsNullOrEmpty(url) || _uvs.ContainsKey(url) || Resolve(url) == null)
            {
                return;
            }

            // Packing happens here rather than in TryGetImageUv because that one
            // is called while a frame is already being recorded: repacking there
            // would move UVs the recorder had handed out moments earlier.
            Repack();
        }

        /// <inheritdoc />
        public bool TryGetImageUv(string url, out Rect uv)
        {
            return _uvs.TryGetValue(url ?? string.Empty, out uv);
        }

        /// <inheritdoc />
        public string LoadCss(string url, string baseUrl)
        {
            // Stylesheets are supplied inline today; nothing asks for this yet.
            return null;
        }

        private Texture2D Resolve(string url)
        {
            if (string.IsNullOrEmpty(url))
            {
                return null;
            }

            if (_sources.TryGetValue(url, out Texture2D known))
            {
                return known;
            }

            if (!_loadFromResources || _missing.Contains(url))
            {
                return null;
            }

            var loaded = Resources.Load<Texture2D>(url);
            if (loaded == null)
            {
                // Remembered so a missing icon costs one lookup, not one per frame.
                _missing.Add(url);
                return null;
            }

            _sources[url] = loaded;
            return loaded;
        }

        /// <summary>
        /// Rebuilds the atlas from every source resolved so far.
        /// </summary>
        /// <remarks>
        /// Repacking moves existing UVs, so <see cref="Version"/> is bumped and
        /// the view re-records. Packing everything again on each new image is
        /// quadratic in the number of images, which is the right trade for a
        /// handful of icons resolved once during the first layout.
        /// </remarks>
        private void Repack()
        {
            var names = new List<string>(_sources.Count);
            var textures = new List<Texture2D>(_sources.Count);

            foreach (KeyValuePair<string, Texture2D> pair in _sources)
            {
                if (pair.Value == null)
                {
                    continue;
                }

                if (!pair.Value.isReadable)
                {
                    Debug.LogError($"[Doctype] '{pair.Key}' ({pair.Value.name}) needs Read/Write enabled in its " +
                                   "import settings before it can be packed into the image atlas.", this);
                    continue;
                }

                names.Add(pair.Key);
                textures.Add(pair.Value);
            }

            if (textures.Count == 0)
            {
                return;
            }

            // A refused pack stays refused until the sources change; retrying
            // with the same inputs would only produce the same error, once per
            // record, forever.
            if (_sources.Count == _refusedAtSourceCount)
            {
                return;
            }

            // Packed into a fresh texture rather than the live atlas:
            // PackTextures mutates its target in place, so a pack that has to
            // be refused would otherwise already have scrambled the texels
            // every existing UV points into. The old atlas stays authoritative
            // until the new one is proven whole.
            var packed = new Texture2D(1, 1, TextureFormat.RGBA32, false) { name = "Doctype image atlas" };

            Rect[] rects = packed.PackTextures(textures.ToArray(), _padding, _maxAtlasSize, false);
            if (rects == null)
            {
                DestroyTexture(packed);
                _refusedAtSourceCount = _sources.Count;
                Debug.LogError($"[Doctype] could not pack {textures.Count} image(s) into a {_maxAtlasSize}px " +
                               "atlas; raise Max Atlas Size or use smaller textures.", this);
                return;
            }

            // PackTextures does not promise to fail when the sources exceed the
            // maximum: its documentation allows it to SCALE THEM DOWN to fit.
            // Layout draws every image at its intrinsic source size, so a
            // silently downscaled texel block renders blurry with no error
            // anywhere. Refuse the pack instead, by comparing every returned
            // rect's pixel size against its source.
            for (int i = 0; i < textures.Count; i++)
            {
                float packedW = rects[i].width * packed.width;
                float packedH = rects[i].height * packed.height;

                if (packedW < textures[i].width - 0.5f || packedH < textures[i].height - 0.5f)
                {
                    Debug.LogError($"[Doctype] packing {textures.Count} image(s) into a {_maxAtlasSize}px atlas " +
                                   $"would shrink '{names[i]}' from {textures[i].width}x{textures[i].height} to " +
                                   $"{Mathf.RoundToInt(packedW)}x{Mathf.RoundToInt(packedH)} and draw it blurry; " +
                                   "raise Max Atlas Size or use smaller textures.", this);
                    DestroyTexture(packed);
                    _refusedAtSourceCount = _sources.Count;
                    return;
                }
            }

            DestroyTexture(_atlas);
            _atlas = packed;

            _uvs.Clear();
            for (int i = 0; i < names.Count; i++)
            {
                _uvs[names[i]] = rects[i];
            }

            // Tells the view that previously-unavailable content arrived, which
            // re-lays-out and drops any cached draw commands holding old UVs.
            _version++;
        }
    }
}
