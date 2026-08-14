using System;
using System.Collections.Generic;
using System.Text;
using UnityEngine;
using UnityEngine.UI;

namespace LiteHtmlUnity.Samples
{
    /// <summary>
    /// An inventory you can drag items out of and drop onto a hotbar, laid out
    /// for a 1080x1920 portrait phone.
    /// </summary>
    /// <remarks>
    /// Exercises the whole interaction surface at once: <c>DragFilter</c> claims
    /// the gesture so the page does not scroll, <c>ElementAt</c> answers what is
    /// under the finger without disturbing hover, <c>SetStyle</c> highlights the
    /// slot being hovered without re-parsing anything, and <c>SetText</c> keeps
    /// the counts current.
    /// <para>
    /// The dragged icon is a plain Unity RawImage rather than part of the page.
    /// It has to be: the document is clipped to its own surface, so an icon
    /// inside it would be cut off at the edge instead of following the finger
    /// down to the hotbar.
    /// </para>
    /// </remarks>
    [RequireComponent(typeof(LiteHtmlView))]
    [AddComponentMenu("LiteHtml/Samples/LiteHtml Inventory Demo")]
    public class LiteHtmlInventoryDemo : MonoBehaviour
    {
        const int InventorySlots = 12;
        const int HotbarSlots = 5;

        /// <summary>Which icon each slot holds, or null when empty.</summary>
        readonly string[] _inventory = new string[InventorySlots];
        readonly string[] _hotbar = new string[HotbarSlots];
        readonly Dictionary<string, int> _counts = new Dictionary<string, int>();

        readonly StringBuilder _sb = new StringBuilder(4096);

        LiteHtmlView _view;
        LiteHtmlRawImage _raw;
        LiteHtmlResources _resources;

        RawImage _ghost;
        RectTransform _ghostRect;
        Canvas _canvas;

        string _dragIcon;        // icon being carried
        string _dragFrom;        // slot id it came from
        string _hovered;         // slot currently highlighted
        string _status = "Bir esyayi tutup asagidaki hotbar'a surukle.";

        void Awake()
        {
            _view = GetComponent<LiteHtmlView>();
            _raw = GetComponent<LiteHtmlRawImage>();
            _canvas = GetComponentInParent<Canvas>();

            BuildIcons();
            Deal();
            CreateGhost();
        }

        void OnEnable()
        {
            _raw.DragFilter = BeginItemDrag;
            _raw.ItemDragged += OnDragMoved;
            _raw.ItemDropped += OnDropped;

            _view.LoadHtml(BuildPage());
        }

        void OnDisable()
        {
            _raw.DragFilter = null;
            _raw.ItemDragged -= OnDragMoved;
            _raw.ItemDropped -= OnDropped;
        }

        // --- content -----------------------------------------------------------

        void BuildIcons()
        {
            _resources = gameObject.GetComponent<LiteHtmlResources>() ??
                         gameObject.AddComponent<LiteHtmlResources>();

            _resources.Register("coin", LiteHtmlDemoIcons.Coin(96));
            _resources.Register("gem", LiteHtmlDemoIcons.Gem(96));
            _resources.Register("potion", LiteHtmlDemoIcons.Potion(96));

            _view.Resources = _resources;
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

        void CreateGhost()
        {
            var go = new GameObject("DragGhost", typeof(RectTransform), typeof(RawImage));
            go.transform.SetParent(_canvas != null ? _canvas.transform : transform.parent, false);

            _ghostRect = (RectTransform)go.transform;
            _ghostRect.sizeDelta = new Vector2(140f, 140f);

            _ghost = go.GetComponent<RawImage>();
            _ghost.raycastTarget = false;   // must not eat the drag it is following
            _ghost.color = new Color(1f, 1f, 1f, 0.9f);

            go.SetActive(false);

            // Last sibling so it draws over the page it was lifted out of.
            go.transform.SetAsLastSibling();
        }

        // --- page --------------------------------------------------------------

        string BuildPage()
        {
            _sb.Clear();
            _sb.Append(@"<style>
  body { margin:0; background:#0b0f1a; color:#e8ecf6;
         font:28px -apple-system,'Helvetica Neue',Arial,sans-serif; }
  .wrap { padding:36px 32px; }
  h1 { font-size:46px; margin:0 0 6px 0; }
  .sub { color:#8e97b3; font-size:26px; margin:0 0 30px 0; }
  .panel { background:#111726; border:2px solid #222b45; border-radius:24px;
           padding:24px; margin-bottom:34px; }
  .label { color:#8e97b3; font-size:24px; margin:0 0 16px 0; }
  .grid { display:flex; }
  .slot { width:152px; height:152px; margin:0 14px 14px 0; border-radius:20px;
          background:#0d1322; border:2px solid #232d49; }
  .hot { width:168px; height:168px; margin-right:16px; border-radius:22px;
         background:#0d1322; border:2px solid #2f3b5e; }
  .count { color:#9fb4e8; font-size:22px; margin:-34px 0 0 14px; }
  .status { color:#7f8aa8; font-size:24px; margin:0; }
</style>");

            _sb.Append("<div class=\"wrap\">");
            _sb.Append("<h1>Envanter</h1>");
            _sb.Append("<p class=\"sub\">Surukle-birak: litehtml sayfasi, Unity hayaleti.</p>");

            _sb.Append("<div class=\"panel\"><p class=\"label\">CANTA</p><div class=\"grid\" style=\"flex-wrap:wrap\">");
            for (int i = 0; i < InventorySlots; i++)
            {
                AppendSlot("inv" + i, _inventory[i], "slot");
            }
            _sb.Append("</div></div>");

            _sb.Append("<div class=\"panel\"><p class=\"label\">HOTBAR</p><div class=\"grid\">");
            for (int i = 0; i < HotbarSlots; i++)
            {
                AppendSlot("hot" + i, _hotbar[i], "hot");
            }
            _sb.Append("</div></div>");

            _sb.Append("<p id=\"status\" class=\"status\">").Append(_status).Append("</p>");
            _sb.Append("</div>");

            return _sb.ToString();
        }

        void AppendSlot(string id, string icon, string cls)
        {
            _sb.Append("<div id=\"").Append(id).Append("\" class=\"").Append(cls).Append("\">");

            if (icon != null)
            {
                int pad = cls == "hot" ? 24 : 20;
                _sb.Append("<img src=\"").Append(icon).Append("\" style=\"width:")
                   .Append(152 - pad * 2).Append("px;height:").Append(152 - pad * 2)
                   .Append("px;margin:").Append(pad).Append("px\">");

                if (_counts.TryGetValue(icon, out int n) && n > 1)
                {
                    _sb.Append("<div class=\"count\">x").Append(n).Append("</div>");
                }
            }

            _sb.Append("</div>");
        }

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

            _ghost.gameObject.SetActive(true);
            MoveGhost(drag.ScreenPosition);

            SetStatus($"{icon} tasiniyor...");
            return true;
        }

        void OnDragMoved(LiteHtmlDrag drag)
        {
            MoveGhost(drag.ScreenPosition);
            Highlight(drag.ElementId);
        }

        void OnDropped(LiteHtmlDrag drag)
        {
            _ghost.gameObject.SetActive(false);
            Highlight(null);

            string target = drag.ElementId;

            if (target != null && target != _dragFrom && IsSlot(target))
            {
                Swap(_dragFrom, target);
                _view.LoadHtml(BuildPage());     // contents changed: rebuild once
                SetStatus($"{_dragIcon} -> {Pretty(target)}");
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

        void MoveGhost(Vector2 screenPosition)
        {
            if (RectTransformUtility.ScreenPointToLocalPointInRectangle(
                    (RectTransform)_ghostRect.parent, screenPosition, null, out Vector2 local))
            {
                _ghostRect.anchoredPosition = local;
            }
        }

        /// <summary>
        /// Lights up the slot under the finger. Uses SetStyle, so the page is not
        /// re-parsed once per frame just to move a border colour.
        /// </summary>
        void Highlight(string id)
        {
            if (id == _hovered)
            {
                return;
            }

            if (_hovered != null)
            {
                _view.SetStyle("#" + _hovered, "");
            }

            _hovered = IsSlot(id) ? id : null;

            if (_hovered != null)
            {
                _view.SetStyle("#" + _hovered, "border-color:#3b82f6;background:#16203a");
            }
        }

        void SetStatus(string text)
        {
            _status = text;
            _view.SetText("#status", text);
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
