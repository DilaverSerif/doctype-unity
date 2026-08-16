# Third-party notices

This repository vendors the following code under `Native/third_party/`, each
under its own licence. Their full licence texts travel with their sources.

| component | licence | what it does |
|---|---|---|
| [litehtml](https://github.com/litehtml/litehtml) | BSD-3-Clause | HTML/CSS parsing, the cascade, and layout |
| gumbo | Apache-2.0 | HTML5 parser, bundled inside litehtml |
| stb_truetype | public domain | glyph rasterization into the font atlas |

litehtml is pinned to a specific revision and carries local patches: two
performance fixes, an entry point for rewriting text after parsing, and the
hooks the retained quad cache needs to observe draws and restyles. Each patch is
marked `LHU PATCH` in the source and states why it exists.
[`Native/third_party/VENDORING.md`](Native/third_party/VENDORING.md) records the
pinned revision and how to re-apply the patches against a newer upstream.
