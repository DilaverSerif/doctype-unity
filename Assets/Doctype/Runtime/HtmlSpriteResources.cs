using System;
using System.Collections.Generic;
using UnityEngine;

namespace Doctype
{
    /// <summary>
    /// Supplies images to a document from sprites that already share one
    /// texture — a SpriteAtlas packed at build time, or sprites sliced from a
    /// single sheet. The production alternative to <see cref="HtmlResources"/>:
    /// no runtime packing, no Read/Write-enabled sources, no repacking ever.
    /// </summary>
    /// <remarks>
    /// The whole page draws as one call with one image texture, so every
    /// sprite must resolve to the same <c>sprite.texture</c>. That is exactly
    /// what a SpriteAtlas produces — under three conditions this provider
    /// validates and otherwise refuses loudly, because each one silently
    /// breaks the single-texture, plain-rect contract:
    /// <list type="bullet">
    /// <item><b>Allow Rotation must be off.</b> The image quad carries an
    /// axis-aligned UV rect; a rotated sprite would need swizzled UVs the
    /// shader does not have.</item>
    /// <item><b>Tight Packing must be off.</b> A tight-packed sprite has no
    /// rectangular texture region to point at.</item>
    /// <item><b>One atlas page.</b> Sprites that spill onto a second page
    /// come back with a different texture and are rejected by name, so the
    /// fix (raise the atlas size, drop a sprite) is visible instead of a
    /// wrong-texture sample.</item>
    /// </list>
    /// </remarks>
    [AddComponentMenu("Doctype/Sprite Resources")]
    public sealed class HtmlSpriteResources : MonoBehaviour, IHtmlResourceProvider
    {
        [Serializable]
        public struct SpriteEntry
        {
            [Tooltip("The name markup refers to, e.g. gold for <img src=\"gold\">. " +
                     "Empty uses the sprite's own name.")]
            public string Name;

            public Sprite Sprite;
        }

        [Tooltip("Sprites this document may use. All of them must live on the same texture " +
                 "(one SpriteAtlas page, rotation and tight packing off).")]
        [SerializeField] private SpriteEntry[] _sprites = Array.Empty<SpriteEntry>();

        private readonly Dictionary<string, Sprite> _byName = new Dictionary<string, Sprite>();
        private readonly Dictionary<string, Rect> _uvs = new Dictionary<string, Rect>();

        private Texture _atlas;
        private int _version;
        private bool _built;

        /// <inheritdoc />
        public int Version => _version;

        /// <inheritdoc />
        public Texture ImageAtlas
        {
            get
            {
                Build();
                return _atlas;
            }
        }

        private void Awake()
        {
            Build();
        }

        /// <summary>
        /// Adds a sprite at runtime under a name. Subject to the same
        /// validation; a sprite on a different texture is refused.
        /// </summary>
        public void Register(string name, Sprite sprite)
        {
            if (string.IsNullOrEmpty(name) || sprite == null)
            {
                return;
            }

            Build();

            if (Accept(name, sprite))
            {
                _byName[name] = sprite;
                _version++;
            }
        }

        /// <inheritdoc />
        public bool TryGetImageSize(string url, out int width, out int height)
        {
            Build();

            width = 0;
            height = 0;

            if (url == null || !_byName.TryGetValue(url, out Sprite sprite) || sprite == null)
            {
                return false;
            }

            width = Mathf.RoundToInt(sprite.rect.width);
            height = Mathf.RoundToInt(sprite.rect.height);
            return true;
        }

        /// <inheritdoc />
        public void BeginLoadImage(string url)
        {
            // Everything a sprite provider can ever serve is known up front;
            // there is nothing to begin.
            Build();
        }

        /// <inheritdoc />
        public bool TryGetImageUv(string url, out Rect uv)
        {
            Build();
            return _uvs.TryGetValue(url ?? string.Empty, out uv);
        }

        /// <inheritdoc />
        public string LoadCss(string url, string baseUrl) => null;

        private void Build()
        {
            if (_built)
            {
                return;
            }

            _built = true;

            foreach (SpriteEntry entry in _sprites)
            {
                if (entry.Sprite == null)
                {
                    continue;
                }

                string name = string.IsNullOrEmpty(entry.Name) ? entry.Sprite.name : entry.Name;

                if (Accept(name, entry.Sprite))
                {
                    _byName[name] = entry.Sprite;
                }
            }

            if (_uvs.Count > 0)
            {
                _version++;
            }
        }

        /// <summary>
        /// Validates one sprite against the contract and, when it holds,
        /// stores its UVs. Refusals are loud and name the sprite: a missing
        /// icon with a console error beats a wrong texture sample.
        /// </summary>
        private bool Accept(string name, Sprite sprite)
        {
            if (sprite.packed && sprite.packingMode == SpritePackingMode.Tight)
            {
                Debug.LogError($"[Doctype] sprite '{name}' is tight-packed; it has no rectangular " +
                               "texture region to draw from. Turn Tight Packing off on the atlas.", this);
                return false;
            }

            if (sprite.packed && sprite.packingRotation != SpritePackingRotation.None)
            {
                Debug.LogError($"[Doctype] sprite '{name}' is packed rotated; the image quad carries an " +
                               "axis-aligned UV rect. Turn Allow Rotation off on the atlas.", this);
                return false;
            }

            Texture texture = sprite.texture;
            if (texture == null)
            {
                Debug.LogError($"[Doctype] sprite '{name}' has no texture.", this);
                return false;
            }

            if (_atlas == null)
            {
                _atlas = texture;
            }
            else if (_atlas != texture)
            {
                Debug.LogError($"[Doctype] sprite '{name}' lives on texture '{texture.name}' but this " +
                               $"provider's atlas is '{_atlas.name}'. One page per provider: everything " +
                               "the page draws must share a texture, or it is no longer one draw call. " +
                               "Raise the atlas size or split the HUD across providers.", this);
                return false;
            }

            Rect px = sprite.textureRect;
            _uvs[name] = new Rect(px.x / texture.width, px.y / texture.height,
                                  px.width / texture.width, px.height / texture.height);
            return true;
        }
    }
}
