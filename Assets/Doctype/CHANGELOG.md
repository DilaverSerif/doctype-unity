# Changelog

All notable changes to this package are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
