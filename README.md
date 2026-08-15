# Doctype

HTML and CSS as a game UI for Unity, laid out by
[litehtml](https://github.com/litehtml/litehtml) and drawn entirely on the GPU.
No embedded browser, no CPU rasterizer, no per-platform web view. One mesh, one
draw call, and it compiles everywhere Unity does, iOS and Android included.

<p align="center">
  <img src="Native/demo.png" width="560" alt="The demo page rendered by the reference rasterizer">
</p>

---

## Why this exists

The usual ways to get HTML into a game engine all embed a browser: Ultralight,
CEF, a platform web view. That buys fidelity and costs you the mobile platforms,
the binary size, and a rasterizer you cannot see into.

litehtml is a different kind of dependency. It is plain C++, it has no renderer
at all, and it does not draw a single pixel. It parses HTML and CSS, runs layout,
and then calls *you* through a `document_container` interface: fill this
rectangle, put this glyph here, stroke this border, paint this gradient.

This project is the other half of that interface. It turns those calls into a
flat stream of quads with the shape parameters baked into their vertices, hands
the whole page to the GPU as one mesh, and reconstructs rounded corners, borders,
gradients and glyph coverage analytically in a fragment shader. What comes out is
a `RenderTexture` you can put anywhere Unity accepts one: a `RawImage`, a
world-space quad, a material slot on a monitor prop.

## What works

- **CSS you would actually use.** Flexbox, floats, tables, `position:
  absolute/fixed`, `overflow:auto` with real scrolling, `vw`/`vh`/`%` units,
  border radius, linear/radial/conic gradients, images, web-safe and embedded
  fonts.
- **Interaction.** `:hover` and `:active`, anchor clicks, element clicks by id,
  scrolling, and dragging, including dragging an item **from one surface onto
  another**, which is what lets a HUD be several independent panels instead of
  one screen-sized sheet.
- **Mutation without re-parsing.** `SetText` and `SetStyle` change a text node or
  an inline style in place. Re-parsing a document is ~11 ms on a budget phone;
  these are ~0.2 ms, and they keep hover, focus and scroll state that a re-parse
  would throw away.
- **Partial redraw.** The native side byte-diffs every recorded frame against the
  previous one and reports a dirty rectangle; the renderer scissors to it and
  keeps the rest of the target from the last frame. Changing a score repaints
  the score, not the page, and on a phone that is the difference between 21 ms
  of GPU and 3 ms. Scrolling goes further: a frame proven to be the previous
  one translated is drawn as a pixel copy plus the strip that scrolled in.
- **Transparency that composites correctly.** A page can be a HUD over a running
  game, with a premultiplied-alpha material and touch pass-through so the parts
  it does not paint are not a sheet of glass over the game.
- **Platforms.** macOS, Android (arm64), iOS. Editor and player, built-in
  pipeline / URP / HDRP. The renderer issues its own `CommandBuffer` against an
  explicit target, so it does not care.

## How it fits together

```
Unity C# (managed)                     Native plugin (C++)

HtmlView          ── HTML/CSS ──▶  litehtml: parse, cascade, layout
  │                                      │
  │                                      ▼
  │                                    lhu_container: records draw calls
  │                                    as an analytic quad stream
  │                                      │
  │                   ◀── quads ─────────┘   (retained; only dirty subtrees
  │                                           are re-recorded)
  ▼
HtmlMeshBuilder   one quad → 4 vertices, shape params in 8 UV channels
  ▼
HtmlRenderer      one CommandBuffer, one DrawMesh, into a RenderTexture
  ▼
HtmlRawImage      composites it on a Canvas, forwards pointer input back
```

Nothing is rasterized on the CPU except glyph coverage, which is cached in an
atlas and uploaded once.

## Getting started

```bash
git clone https://github.com/DilaverSerif/doctype-unity
cd doctype-unity
Native/build_macos.sh          # or build_android.sh / build_ios.sh
```

Open the project in Unity 6000.5+ and load
`Assets/Doctype/Samples/HtmlDemo.unity`.

In code:

```csharp
var view = gameObject.AddComponent<HtmlView>();
view.LoadHtml("<body style='font-family:sans-serif'><h1>Merhaba</h1></body>");
// view.Texture is a RenderTexture

view.SetText("#score", "1280");                       // no re-parse
view.SetStyle("#slot3", "border-color:#3b82f6");      // no re-parse
string id = view.ElementAt(pointInCssPixels);          // pure query, no hover change
```

The package README under [`Assets/Doctype/`](Assets/Doctype/README.md)
covers fonts, resources, HUD layout and the input model in more depth.

---

## Measurements

This is the part that shaped the design, so it gets the space.

Everything below was measured on a **Xiaomi 22101316I at 1080×2400** (a budget
Android phone, deliberately) with a spinning-cube load running underneath, 300
frames per scenario, Vulkan, frame pacing off, GPU times from
`FrameTimingManager`. The harness is in the repo
([`HtmlBenchmark.cs`](Assets/Doctype/Samples/HtmlBenchmark.cs));
build it with `Tools/Doctype/Build Benchmark APK` and it writes a CSV to the
device.

Each panel is a quarter of the screen holding a scrollable list: 383 quads, the
size of a real HUD panel.

| scenario | quads | CPU ms/frame | GPU ms | vertex KB/frame |
|---|---|---|---|---|
| no HUD at all | 0 | 0.00 | 2.69 | 0 |
| HUD up, nothing changing | 381 | **0.00** | **3.11** | **0** |
| four HUDs up, nothing changing | 1524 | **0.00** | **4.65** | **0** |
| one text node changed per frame | 383 | 1.69 | **3.37** | 171 |
| one inline style changed per frame | 383 | 1.50 | **4.09** | 165 |
| scrolled every frame | 410 | 1.22 | **4.68** | 183 |
| resized every frame | 412 | 4.13 | 21.77 | 183 |
| re-parsed every frame | 384 | 12.15 | **6.25** | 171 |
| four panels, one text node each | 1532 | 6.74 | **5.07** | 2729 |

### A HUD that is not changing is free

Zero CPU, zero bytes uploaded, and 1.9 ms of GPU over the game-only baseline for
four panels and 1524 quads. That is not a rounding artifact: the view skips
layout, recording, mesh build and draw entirely when nothing has invalidated it,
so an idle page costs one `if`. For UI that updates a few times a second, the
work is already done.

### The cost is pixels, not geometry

Scrolling and resizing, the two scenarios that repaint a whole panel, land at
21 and 22 ms of GPU. Before partial redraw existed, *everything* did: changing
one character cost the same 21 ms as re-parsing the entire document. That
plateau took a controlled experiment to read correctly:

| full redraw | quads | vertex bytes | pixels | GPU |
|---|---|---|---|---|
| full resolution | 383 | 224 KB | 1× | 21.09 ms |
| half resolution | **383** | **224 KB** | **¼** | **8.14 ms** |

Same document, same quad count, the same 224 KB uploaded. Only the pixel count
changed, and the cost fell by 3.3×. So the GPU time is fill and overdraw: the
page paints its panel background, then a rectangle per row, then a border, then
the glyphs, and every one of those fragments is shaded by an SDF shader with an
antialiasing skirt.

The plateau numbers already include one consequence of that reading: the exact
sRGB transfer function used to run per fragment, ahead of every branch, on a
colour the mesh writes once per quad. Moving it to the vertex stage (identical
output, since four corners carrying one value interpolate to that value) took a
consistent **14% off every redraw scenario** (24.4 → 21.1 ms on one panel,
36.3 → 31.8 on four). One `pow` per channel, measured at three milliseconds a
frame on this phone.

Two things follow. Vertex bandwidth is **not** the bottleneck, and that has
since been tested directly rather than inferred: putting the vertex on a diet
from 144 to 108 bytes (half-precision radii, border widths and clip radii,
which are small lengths where float16 is exact to a fraction of a pixel, and an
implied z) cut a four-panel HUD's upload from 3.6 MB to 2.7 MB per frame and
moved neither the CPU nor the GPU column by a measurable amount. The smaller
vertex stays because it costs nothing, but the win was never there. And
`RenderScale`, which exists on `HtmlRawImage`, turns out to be a shipping-grade
optimisation and not just a measurement knob: half resolution, 3.5× cheaper,
softer text.

### Redraw only the pixels that changed

If the cost is fill, the cheapest pixel is the one you do not shade. The native
side already re-records only dirty subtrees; it now also byte-diffs each
recorded frame against the previous one and reports the result as none, a
rectangle, or everything. The diff is a common prefix plus a common suffix, so
a quad inserted in the middle (a score gaining a digit) dirties its own range
and not everything after it. The renderer scissors to that rectangle, keeps
every other pixel from the last frame, and clears inside the scissor by drawing
an opaque quad, because a scissored `ClearRenderTarget` is precisely the
operation graphics APIs disagree about.

The GPU column in the first table shows what that buys on this phone:

- A text or style change fell from 21 ms to 3.4-4.1 ms, within noise of an idle
  HUD. Four panels each changing a text node per frame: 31.8 ms down to 5.1 ms.
- Re-parsing the document fell from 21 ms to 6.2 ms without anyone optimising
  re-parse: the diff is byte-based and does not care why the recording ran, so
  a re-parse that reproduces the same quads is a small dirty rectangle.
- A resize still pays the full 21 ms, honestly. Every pixel of the panel
  really does change.

### Scrolling is a translation, so treat it like one

Scrolling used to sit on the same 21 ms plateau, which is absurd on its face:
almost every pixel of a scrolled frame is already sitting in the render
target, six pixels away from where it needs to be. So the diff now takes the
applied scroll delta as a hypothesis, and the frame has to prove it quad by
quad: content must be the previous frame's bytes translated (exactly, through
a memcmp), what scrolled in may paint only inside the strip the repaint will
cover, what scrolled out must leave the copied region entirely, and everything
that did not move, from the heading above the list to the background the rows
slide across, must look the same shifted. The first quad that fits none of
these falls the frame back to the plain rect, so the fast path can be wrong
about nothing: hover restyles mid-scroll, gradients under the content and
rounded clips all degrade to a full repaint rather than to a wrong pixel.

One honest subtlety survived contact with a real page: a scrolled window's
edge usually sits on a fractional pixel (the text sizes above it decide where
it lands), and the pixel row straddling that edge blends content with
background. A whole-pixel copy cannot move a blend, so those rows ride in a
second, sliver-sized dirty rect and get repainted instead.

The renderer then moves the surviving pixels with two texel copies (through a
scratch target, because a texture cannot copy onto itself) and repaints a
16-pixel strip instead of an 1119-pixel window. On the phone that turned
21.4 ms of GPU into 4.7 ms. Touch dragging gets the same path because
`HtmlRawImage` quantizes drag deltas to whole document pixels and carries the
fraction, which no finger can tell and every frame can copy.

One consequence: `RenderScale` used to be the big lever, and now it only
matters for full redraws. A mutating panel at half resolution measures 3.19 ms
against 3.34 at full, because the dirty region is small either way.

### Re-parsing is the expensive thing, so don't

Rebuilding a document from a string costs 12.6 ms of CPU, of which **10.9 ms is
parsing**, and most of that is litehtml re-parsing its own default stylesheet,
a fixed price per document regardless of page size. Two things take it off the
table: a trimmed master stylesheet for game UI (~2.4× faster document creation),
and a check that skips the parse entirely when the markup is byte-identical to
what is already loaded. Which is why the API pushes you toward `SetText` and
`SetStyle` instead.

---

## What measuring actually found

Every one of these was invisible until something measured it, and several were
bugs in the measurement itself. They are worth listing because they are the
argument for having a benchmark at all.

**`vw`/`vh` were frozen at parse time.** litehtml resolves viewport units when it
*computes* styles, against a media snapshot taken once. A host sets the viewport
after loading the page, because the surface only learns its size a frame later, so
every viewport-relative length answered for a size the page was never displayed
at. A 4-column grid silently wrapped to 3.

**Fixing that broke scrolling.** The restyle initially rebuilt the render tree,
and a freshly created `scroll_view` reports nothing to scroll until a second
render has measured its content. The page scrolled, moved zero pixels, and looked
like the touch was ignored. The fix was to stop rebuilding: `media_changed()`
already does it when a media query genuinely flips.

**Transparent meant white.** The surface composites with `Blend One
OneMinusSrcAlpha`, so colours must be premultiplied. The default clear was
`(1,1,1,0)`: white at zero alpha, which is not a valid premultiplied value and
adds white to everything behind wherever the page paints nothing. Invisible until
the first page with a genuinely empty region.

**A stats panel and a benchmark want opposite things.** `ParseMs`/`LayoutMs`/
`DrawMs` mean "the last time this ran", and they keep reporting it on every frame
where it did not. An idle HUD confidently reported 2.67 ms of parsing per frame.
The view now also carries monotonic counters, and the benchmark divides
differences by frames rather than taking medians of a sticky value.

**Three benchmark scenarios were measuring nothing.** The style-churn row keyed
its colour off `frame & 1` while sweeping an even number of slots, so every slot
was revisited on the same parity and `SetStyle` kept answering "already that".
The resize row moved the rect by one canvas unit, which rounds away to nothing on
a small screen. The scroll row aimed at a point that landed on the header. All
three reported clean numbers for work that never happened.

**The benchmark's panels were slivers.** Their parent `RectTransform` was left at
its default zero size, so four "quarter-screen" panels became four slivers around
the canvas centre, and every number was about a HUD nobody would ship.

**The frame-time column was lying twice.** Unity paces frames to a whole divisor
of the refresh rate, so a run that misses 16.7 ms once locks to 30 fps and every
scenario cheaper than 33 ms reports 33 ms. And with 240 cubes the baseline was
already GPU-bound, so the HUD had a 33 ms shadow to hide in. The benchmark now
turns pacing off, uses an unclamped clock, and flags per row when work was
measured that the frame time did not move for.

**`FrameTimingManager` returns nothing unless you ask.** `enableFrameTimingStats`
is a Player Setting and defaults to off, on every platform and every graphics
API. The GPU column read "n/a" from top to bottom, which looks exactly like a
device refusing to answer.

---

## Testing

```bash
Native/build_macos.sh harness        # 120 checks, no Unity and no GPU
Native/build/macos/bin/lhu_verify_quadcache   # 693 frame comparisons
Native/bench_android.sh              # CPU benchmark on a real phone, no Unity
```

Play-mode tests (37) and edit-mode tests (43) live in `Assets/Doctype/Tests/`
and run through Unity's test runner. The native harness rasterizes pages through a reference
software rasterizer, so the quad stream can be checked against known pixels
without a GPU in the loop.

## Status

Working and measured, not yet a released package. The interfaces described above
are stable enough to build on; the roadmap is about cost, not correctness:
making the remaining full redraw cheaper (a resize still repaints every
pixel), and separating layout cost from document size.

## Licence and credits

This project is MIT licensed, see [LICENSE](LICENSE).

It vendors patched copies of third-party code under `Native/third_party/`, each
under its own licence, see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md):

| | | |
|---|---|---|
| [litehtml](https://github.com/litehtml/litehtml) | BSD-3-Clause | HTML/CSS parsing and layout |
| gumbo | Apache-2.0 | HTML5 parser, bundled with litehtml |
| stb_truetype | public domain | glyph rasterization |

litehtml is pinned and carries local patches: performance fixes, a text-mutation
entry point, and hooks the retained quad cache needs. Every patch is marked
`LHU PATCH` in the source and explained where it sits;
[`Native/third_party/VENDORING.md`](Native/third_party/VENDORING.md) says how to
update against upstream.
