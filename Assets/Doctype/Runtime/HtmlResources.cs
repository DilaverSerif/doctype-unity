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

        /// <inheritdoc />
        public int Version => _version;

        /// <inheritdoc />
        public Texture ImageAtlas => _atlas;

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
            if (_atlas != null)
            {
                Destroy(_atlas);
                _atlas = null;
            }
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

            if (_atlas == null)
            {
                _atlas = new Texture2D(1, 1, TextureFormat.RGBA32, false) { name = "Doctype image atlas" };
            }

            Rect[] rects = _atlas.PackTextures(textures.ToArray(), _padding, _maxAtlasSize, false);
            if (rects == null)
            {
                Debug.LogError($"[Doctype] could not pack {textures.Count} image(s) into a {_maxAtlasSize}px " +
                               "atlas; raise the limit or use smaller textures.", this);
                return;
            }

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
