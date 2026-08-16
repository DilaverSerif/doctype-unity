# Changelog

All notable changes to this package are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.1] - 2026-08-16

### Fixed

- CRITICAL: a click handler that loads a new document (the ordinary menu
  pattern: `BindClick`/`AnchorClicked` handler calling `LoadHtml`) crashed
  the editor with a pure virtual call. The handler fired while the clicked
  document's native frames were still on the stack; replacing the document
  made a temporary reference inside the dispatch its last owner, and the
  document destroyed itself under its own element's member function. Two
  layers now prevent this: every dispatching native export pins the document
  for the duration of the call, and the managed layer defers user events
  (AnchorClicked, ElementClicked, click bindings, CursorChanged) until the
  input call unwinds, so a handler may reload or even dispose freely.

### Added

- An interaction gauntlet in PlayMode: reload-from-click open/close
  toggling, anchor navigation loops, button spam with mutating handlers,
  drag-scroll storms, hover flicker over live mutations. Input the way a
  player delivers it, asserted to end in the state the last input asked for.

## [0.2.0] - 2026-08-16

### Added

- Gamepad/keyboard focus: elements opt in with `tabindex`, carry the `focus`
  pseudo-class (style with `:focus`), and a spatial metric with
  `data-nav-*` overrides moves focus directionally. `Activate` runs the real
  click path (anchors and element clicks fire) without touching pointer
  state. New API: `HtmlView.SetFocus/MoveFocus/Activate/FocusedId` and the
  `HtmlFocusNavigator` component bridging Unity's EventSystem
  Move/Submit/Cancel. Multi-panel HUDs wire each navigator's
  Up/Right/Down/Left neighbours: walking off a panel's edge hands focus to
  the neighbour as a transaction (the giver keeps focus unless the receiver
  really takes it), each panel remembers its last focused element for the
  way back, and the EventSystem selection follows. Text input and IME are
  out of scope.

- Persistent mesh with ranged uploads: the frame now carries the stable quad
  prefix/suffix from the native diff, and the mesh builder keeps its buffers
  alive and rewrites only the changed span. On the benchmark phone a text
  mutation's upload fell from 171 KB to 1.4 KB per frame and its CPU from
  0.93 to 0.66 ms; four mutating panels from 2.7 MB to 5.7 KB and 2.81 to
  1.69 ms. Pixel-for-pixel equivalence against full rebuilds is pinned by
  EditMode tests, and the state-transition matrix gained a third oracle that
  verifies the stable-range claim on every frame of every action pair.
- The benchmark's vertex columns are now measured at the buffer instead of
  derived from the quad count.

- HtmlSpriteResources: a production image provider over sprites that share
  one texture (a build-time SpriteAtlas page). No runtime packing; refuses
  rotated, tight-packed or off-page sprites loudly by name.
- Locale-aware text-transform: simple case mappings for Latin-1 Supplement
  and Latin Extended-A for every language (café/über/çağrı all case
  correctly), and the four-way Turkish dotted/dotless i when the document
  language is set to Turkish via the new `SetLanguage("tr", "tr-TR")`.
  One-to-many mappings (ß to SS) stay out of scope.

### Fixed

- HtmlResources no longer accepts an image atlas that Unity's PackTextures
  silently downscaled to fit Max Atlas Size (measured really happening: a
  16x16 icon came back 12x12). Layout draws images at their intrinsic size,
  so the shrunken texels rendered blurry with no error anywhere. Packing now
  goes through a scratch texture, verifies every packed rect is full
  resolution, refuses loudly otherwise, and leaves the previous atlas and
  its UVs untouched on refusal.

## [0.1.0] - 2026-08-16

First packaged release. Working and measured on device (Xiaomi 22101316I,
Vulkan): see the repository README for the full benchmark table.

### Added

- litehtml-backed HTML/CSS layout with a GPU-only renderer: every draw call
  is recorded as an analytic quad and reconstructed in a fragment shader
  (rounded rects, borders, linear/radial/conic gradients, glyphs, images).
  One mesh, one draw call per page.
- `HtmlView` / `HtmlRawImage` components: RenderTexture output, pointer input
  forwarding, `:hover`, anchor clicks, scrolling, cross-surface drag and drop,
  touch pass-through for transparent HUD areas.
- Incremental mutation API: `SetText` and `SetStyle` change the document in
  place with no re-parse; the dirtied layout is answered by re-laying only the
  touched subtree when that is provably safe.
- Retained quad cache with byte-diffed partial redraw: an unchanged document
  performs no parse, layout, recording, mesh upload or offscreen redraw, and
  a changed one repaints a dirty rectangle instead of the page. Scrolling is
  detected as a translation and drawn as a pixel copy plus the entered strip.
- Input dirty contract: native reports whether an event dirtied paint only or
  layout too, so hover effects that move geometry re-lay out and ones that
  only recolor do not.
- Exception-sealed C ABI: no C++ exception can cross the P/Invoke boundary;
  failures surface as return codes plus `lhu_last_error()`.
- Prebuilt native plugins: macOS universal bundle, Android arm64-v8a, iOS
  static library. Build scripts for all three plus a CMake path for CI.
- Test coverage: 140 native checks against a reference CPU rasterizer, 693
  cached-vs-uncached frame comparisons, 43 EditMode and 37 PlayMode tests.
