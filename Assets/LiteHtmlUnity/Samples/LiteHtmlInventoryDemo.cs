using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using UnityEngine;
using UnityEngine.UI;

namespace LiteHtmlUnity.Samples
{
    /// <summary>
    /// An inventory you can drag items out of and drop onto a hotbar, drawn as
    /// two separate surfaces over a stand-in game.
    /// </summary>
    /// <remarks>
    /// Two documents, not one. The bag and the hotbar are independent
    /// LiteHtmlViews with their own RectTransforms, and dragging an item from
    /// one into the other crosses a surface boundary — which is the thing this
    /// sample is here to show. uGUI hands an entire drag to the object it began
    /// on, so the hotbar never hears the drop on its own; <c>LiteHtmlDrag.View</c>
    /// answers with whichever page is actually under the finger.
    /// <para>
    /// Splitting them costs nothing and buys the layout: each surface is only as
    /// large as its own panel, the band between them is not covered by anything
    /// at all, and neither page has to know the other exists. The alternative —
    /// one screen-sized surface with a transparent middle — pays for a whole
    /// screen of texture in order to paint two strips of it.
    /// </para>
    /// <para>
    /// It also exercises the rest of the interaction surface: <c>DragFilter</c>
    /// claims the gesture so the page does not scroll, <c>ElementAt</c> answers
    /// what is under the finger without disturbing hover, <c>SetStyle</c>
    /// highlights the slot being hovered without re-parsing anything, and
    /// <c>SetText</c> keeps the status line current.
    /// </para>
    /// <para>
    /// The dragged icon is a plain Unity RawImage rather than part of a page. It
    /// has to be: each document is clipped to its own surface, so an icon inside
    /// the bag would be cut off at the bag's edge instead of following the
    /// finger down to the hotbar.
    /// </para>
    /// </remarks>
    [AddComponentMenu("LiteHtml/Samples/LiteHtml Inventory Demo")]
    public class LiteHtmlInventoryDemo : MonoBehaviour
    {
        const int InventorySlots = 12;
        const int HotbarSlots = 5;

        // Mirrors .slot in the bag's stylesheet, and only exists so the dragged
        // ghost can be sized to the icon it was lifted out of.
        const float SlotVw = 20.5f;
        const float IconOfSlot = 0.75f;

        [SerializeField] private LiteHtmlView _bagView;
        [SerializeField] private LiteHtmlView _hotbarView;

        /// <summary>The bag's surface. Assigned by the scene builder.</summary>
        public LiteHtmlView BagView { get => _bagView; set => _bagView = value; }

        /// <summary>The hotbar's surface. Assigned by the scene builder.</summary>
        public LiteHtmlView HotbarView { get => _hotbarView; set => _hotbarView = value; }

        /// <summary>Which icon each slot holds, or null when empty.</summary>
        readonly string[] _inventory = new string[InventorySlots];
        readonly string[] _hotbar = new string[HotbarSlots];
        readonly Dictionary<string, int> _counts = new Dictionary<string, int>();

        readonly StringBuilder _sb = new StringBuilder(4096);

        LiteHtmlRawImage _bagRaw;
        LiteHtmlRawImage _hotRaw;
        LiteHtmlResources _resources;

        RawImage _ghost;
        RectTransform _ghostRect;
        Canvas _canvas;
        LiteHtmlDemoWorld _world;

        int _worldTaps;

        string _dragIcon;            // icon being carried
        string _dragFrom;            // slot id it came from
        string _hovered;             // slot currently highlighted
        LiteHtmlView _hoveredView;   // and the page that owns it
        string _status = "Bir esyayi tutup asagidaki hotbar'a surukle.";

        void Awake()
        {
            _canvas = GetComponentInParent<Canvas>();

            _bagRaw = _bagView != null ? _bagView.GetComponent<LiteHtmlRawImage>() : null;
            _hotRaw = _hotbarView != null ? _hotbarView.GetComponent<LiteHtmlRawImage>() : null;

            BuildIcons();
            Deal();
            CreateWorld();
            CreateGhost();
        }

        void OnEnable()
        {
            Hook(_bagRaw);
            Hook(_hotRaw);

            // Each panel ends up exactly as tall as its own page, which is what
            // keeps the band between them free of any surface at all.
            SetUp(_bagView);
            SetUp(_hotbarView);

            _bagView.LoadHtml(BuildBagPage());
            _hotbarView.LoadHtml(BuildHotbarPage());
        }

        void OnDisable()
        {
            Unhook(_bagRaw);
            Unhook(_hotRaw);
        }

        void OnDestroy()
        {
            if (_world != null)
            {
                _world.Tapped -= OnWorldTapped;
            }
        }

        void Hook(LiteHtmlRawImage raw)
        {
            if (raw == null)
            {
                return;
            }

            // Set here rather than left to the scene asset: a panel that quietly
            // swallows touches through its own rounded corners is the kind of
            // thing that only shows up on a device.
            raw.PassThroughEmptyAreas = true;

            raw.DragFilter = BeginItemDrag;
            raw.ItemDragged += OnDragMoved;
            raw.ItemDropped += OnDropped;
        }

        void Unhook(LiteHtmlRawImage raw)
        {
            if (raw == null)
            {
                return;
            }

            raw.DragFilter = null;
            raw.ItemDragged -= OnDragMoved;
            raw.ItemDropped -= OnDropped;
        }

        void SetUp(LiteHtmlView view)
        {
            view.Resources = _resources;
            view.AutoHeight = true;
        }

        // --- panel sizing ------------------------------------------------------

        void LateUpdate()
        {
            Fit(_bagView);
            Fit(_hotbarView);
        }

        /// <summary>
        /// Gives the panel the height its page laid out to.
        /// </summary>
        /// <remarks>
        /// AutoHeight grows the surface but deliberately never touches a
        /// RectTransform — a view can sit on a world-space quad, or on nothing
        /// anyone wants resized. On a uGUI panel it has to be done here, and in
        /// canvas units: the surface is sized in screen pixels, so the height
        /// comes back divided by the canvas scale factor, not by DeviceScale
        /// (which bottoms out at 0.5 and is a different number on a small
        /// screen).
        /// </remarks>
        void Fit(LiteHtmlView view)
        {
            if (view == null || view.Texture == null || _canvas == null)
            {
                return;
            }

            float scale = _canvas.scaleFactor;
            if (scale <= 0f)
            {
                return;
            }

            var rect = (RectTransform)view.transform;
            float wanted = view.Texture.height / scale;

            if (Mathf.Abs(rect.sizeDelta.y - wanted) > 0.5f)
            {
                rect.sizeDelta = new Vector2(rect.sizeDelta.x, wanted);
            }
        }

        // --- content -----------------------------------------------------------

        void BuildIcons()
        {
            _resources = gameObject.GetComponent<LiteHtmlResources>() ??
                         gameObject.AddComponent<LiteHtmlResources>();

            _resources.Register("coin", LiteHtmlDemoIcons.Coin(96));
            _resources.Register("gem", LiteHtmlDemoIcons.Gem(96));
            _resources.Register("potion", LiteHtmlDemoIcons.Potion(96));
        }

        void Deal()
        {
            _inventory[0] = "coin";
            _inventory[1] = "gem";
            _inventory[2] = "potion";
            _inventory[4] = "gem";
            _inventory[7] = "coin";

            _counts["coin"] = 128;
            _counts["gem"] = 7;
            _counts["potion"] = 3;
        }

        /// <summary>
        /// Puts a stand-in game behind the panels, so the space between them
        /// reads as empty rather than as a dark background.
        /// </summary>
        void CreateWorld()
        {
            if (_canvas == null)
            {
                return;
            }

            _world = LiteHtmlDemoWorld.Create(_canvas);
            _world.Tapped += OnWorldTapped;
        }

        void OnWorldTapped(Vector2 screenPosition)
        {
            _worldTaps++;
            SetStatus($"Bosluga {_worldTaps} dokunus dustu — paneller yutmuyor.");
        }

        void CreateGhost()
        {
            var go = new GameObject("DragGhost", typeof(RectTransform), typeof(RawImage));
            go.transform.SetParent(_canvas != null ? _canvas.transform : transform, false);

            _ghostRect = (RectTransform)go.transform;

            _ghost = go.GetComponent<RawImage>();
            _ghost.raycastTarget = false;   // must not eat the drag it is following
            _ghost.color = new Color(1f, 1f, 1f, 0.9f);

            go.SetActive(false);

            // Last sibling so it draws over the panels it was lifted out of.
            go.transform.SetAsLastSibling();
        }

        // --- pages -------------------------------------------------------------
        //
        // Two documents that share nothing but a colour scheme. Each fills its
        // own surface, so the insets and the band between them are the panels'
        // RectTransforms rather than anything in CSS. vw is a fraction of the
        // panel here, not of the screen, which is what keeps the column counts
        // fixed on any display.

        const string PanelCss = @"
  html, body { margin:0; background:transparent; color:#e8ecf6;
               font:2.8vw -apple-system,'Helvetica Neue',Arial,sans-serif; }
  .panel { background:#111726; border:2px solid #222b45; border-radius:2.4vw;
           padding:2.4vw; }
  .label { color:#8e97b3; font-size:2.4vw; margin:0 0 1.8vw 0; }
  .grid { display:flex; }
  .count { color:#9fb4e8; font-size:2.2vw; margin:-3.8vw 0 0 1.6vw; }
";

        string BuildBagPage()
        {
            _sb.Clear();
            _sb.Append("<style>").Append(PanelCss).Append(@"
  /* Four across. A flex line breaks on an item's outer width including its
     right margin, so the row has to fit 4 x (slot + gap), not 4 x slot +
     3 x gap. The difference is under a pixel on a wide panel and one whole
     column on a narrow one, and it wraps to three without a word. */
  .slot { width:20.5vw; height:20.5vw; margin:0 2vw 2vw 0; border-radius:2vw;
          background:#0d1322; border:2px solid #232d49; }
</style>");

            _sb.Append("<div id=\"bag\" class=\"panel\">");
            _sb.Append("<p class=\"label\">CANTA</p>");
            _sb.Append("<div class=\"grid\" style=\"flex-wrap:wrap\">");

            for (int i = 0; i < InventorySlots; i++)
            {
                AppendSlot("inv" + i, _inventory[i], "slot", SlotVw);
            }

            _sb.Append("</div></div>");
            return _sb.ToString();
        }

        string BuildHotbarPage()
        {
            _sb.Clear();
            _sb.Append("<style>").Append(PanelCss).Append(@"
  .hot { width:16vw; height:16vw; margin-right:1.6vw; border-radius:2vw;
         background:#0d1322; border:2px solid #2f3b5e; }
  #status { color:#7f8aa8; font-size:2.4vw; margin:0 0 1.8vw 0; }
</style>");

            _sb.Append("<div id=\"hotbar\" class=\"panel\">");
            _sb.Append("<p id=\"status\">").Append(_status).Append("</p>");
            _sb.Append("<div class=\"grid\">");

            for (int i = 0; i < HotbarSlots; i++)
            {
                AppendSlot("hot" + i, _hotbar[i], "hot", 16f);
            }

            _sb.Append("</div></div>");
            return _sb.ToString();
        }

        void AppendSlot(string id, string icon, string cls, float slotVw)
        {
            _sb.Append("<div id=\"").Append(id).Append("\" class=\"").Append(cls).Append("\">");

            if (icon != null)
            {
                float art = slotVw * IconOfSlot;
                float pad = (slotVw - art) / 2f;

                _sb.Append("<img src=\"").Append(icon).Append("\" style=\"width:")
                   .Append(Vw(art)).Append(";height:").Append(Vw(art))
                   .Append(";margin:").Append(Vw(pad)).Append("\">");

                if (_counts.TryGetValue(icon, out int n) && n > 1)
                {
                    _sb.Append("<div class=\"count\">x").Append(n).Append("</div>");
                }
            }

            _sb.Append("</div>");
        }

        /// <summary>
        /// A vw length. Invariant culture, because a comma decimal separator
        /// turns one CSS length into two and the page silently loses the rule.
        /// </summary>
        static string Vw(float v) => v.ToString("0.##", CultureInfo.InvariantCulture) + "vw";

        // --- dragging ----------------------------------------------------------

        bool BeginItemDrag(LiteHtmlDrag drag)
        {
            string icon = IconIn(drag.ElementId);
            if (icon == null)
            {
                return false;   // empty slot or bare page: let the page scroll
            }

            _dragIcon = icon;
            _dragFrom = drag.ElementId;

            _ghost.texture = _resources.ImageAtlas;
            if (_resources.TryGetImageUv(icon, out Rect uv))
            {
                _ghost.uvRect = uv;
            }

            SizeGhostTo(drag.View);

            _ghost.gameObject.SetActive(true);
            MoveGhost(drag.ScreenPosition);

            SetStatus($"{icon} tasiniyor...");
            return true;
        }

        void OnDragMoved(LiteHtmlDrag drag)
        {
            MoveGhost(drag.ScreenPosition);
            Highlight(drag.View, drag.ElementId);
        }

        void OnDropped(LiteHtmlDrag drag)
        {
            _ghost.gameObject.SetActive(false);
            Highlight(null, null);

            string target = drag.ElementId;

            if (target != null && target != _dragFrom && IsSlot(target))
            {
                Swap(_dragFrom, target);

                // Status first, pages second. LoadHtml only marks a document for
                // reload, so a SetText issued after it lands on the document
                // about to be thrown away, and the new page comes up still
                // narrating the drag. Assigning _status lets the page carry it.
                _status = $"{_dragIcon} -> {Pretty(target)}";

                // Both of them, always: an item that left the bag arrived in the
                // hotbar, and neither page can render the other's half.
                _bagView.LoadHtml(BuildBagPage());
                _hotbarView.LoadHtml(BuildHotbarPage());
            }
            else
            {
                SetStatus(target == _dragFrom
                              ? "Ayni yere birakildi."
                              : "Gecerli bir slot degil, esya yerinde kaldi.");
            }

            _dragIcon = null;
            _dragFrom = null;
        }

        /// <summary>
        /// Matches the ghost to the icon it was lifted out of. The slot is sized
        /// in vw, and vw is a fraction of the panel rather than of the screen,
        /// so the source panel's own width is what converts it.
        /// </summary>
        void SizeGhostTo(LiteHtmlView source)
        {
            float panel = source != null ? ((RectTransform)source.transform).rect.width : 0f;
            if (panel <= 0f)
            {
                return;
            }

            float art = SlotVw * IconOfSlot / 100f * panel;
            _ghostRect.sizeDelta = new Vector2(art, art);
        }

        void MoveGhost(Vector2 screenPosition)
        {
            if (RectTransformUtility.ScreenPointToLocalPointInRectangle(
                    (RectTransform)_ghostRect.parent, screenPosition, null, out Vector2 local))
            {
                _ghostRect.anchoredPosition = local;
            }
        }

        /// <summary>
        /// Lights up the slot under the finger, on whichever page reported it.
        /// Uses SetStyle, so a page is not re-parsed once per frame just to move
        /// a border colour.
        /// </summary>
        void Highlight(LiteHtmlView view, string id)
        {
            if (id == _hovered)
            {
                return;
            }

            if (_hovered != null && _hoveredView != null)
            {
                _hoveredView.SetStyle("#" + _hovered, "");
            }

            _hovered = IsSlot(id) ? id : null;
            _hoveredView = _hovered != null ? view : null;

            if (_hoveredView != null)
            {
                _hoveredView.SetStyle("#" + _hovered, "border-color:#3b82f6;background:#16203a");
            }
        }

        void SetStatus(string text)
        {
            _status = text;
            _hotbarView.SetText("#status", text);
        }

        // --- model -------------------------------------------------------------

        static bool IsSlot(string id) =>
            id != null && (id.StartsWith("inv") || id.StartsWith("hot"));

        string IconIn(string id)
        {
            if (!IsSlot(id))
            {
                return null;
            }

            string[] bank = Bank(id, out int index);
            return index >= 0 && index < bank.Length ? bank[index] : null;
        }

        string[] Bank(string id, out int index)
        {
            bool inv = id.StartsWith("inv");
            string[] bank = inv ? _inventory : _hotbar;
            index = int.TryParse(id.Substring(3), out int i) ? i : -1;
            return bank;
        }

        /// <summary>Swaps two slots, so dropping onto a full slot exchanges them.</summary>
        void Swap(string a, string b)
        {
            string[] bankA = Bank(a, out int ia);
            string[] bankB = Bank(b, out int ib);

            if (ia < 0 || ib < 0 || ia >= bankA.Length || ib >= bankB.Length)
            {
                return;
            }

            (bankA[ia], bankB[ib]) = (bankB[ib], bankA[ia]);
        }

        static string Pretty(string id) =>
            id.StartsWith("hot") ? "hotbar " + (int.Parse(id.Substring(3)) + 1) : "canta";
    }
}
