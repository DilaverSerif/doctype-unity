# Vendored dependencies

## litehtml

    https://github.com/litehtml/litehtml.git
    b9e89f0b9494ff9a5f008800af35503efabddf59   (2026-08-01)

Checked in as plain source rather than as a submodule, because it carries local
patches that have to travel with this repository. Search for `LHU PATCH` and
`LHU EXPERIMENT` to find them; each one states why it exists so it can be judged
on its own when litehtml is next updated.

Patched at the time of writing:

| file | why |
|---|---|
| `src/formatting_context.cpp`, `include/litehtml/formatting_context.h` | float placement scanned every float already placed, which is quadratic in rows. An index over the lists makes it linear. |
| `src/document_container.cpp`, `include/litehtml/document_container.h` | inline `style=` attributes were re-tokenised per element; identical declaration blocks are now parsed once per container. |
| `src/html_tag.cpp` | uses the cache above. |
| `src/el_text.cpp`, `include/litehtml/el_text.h` | text nodes could not be rewritten after parsing, which is what `lhu_set_text` needs. |
| `src/element.cpp`, `src/render_item.cpp`, `src/render_table.cpp`, `include/litehtml/document.h`, `include/litehtml/render_item.h` | hooks the retained quad cache needs to see draws and restyles. |

To update litehtml: clone the new revision beside this one, diff each patched
file against the old upstream revision above, and re-apply what still applies.
The benchmark's correctness gates (`Native/tests/bench.cpp`, `harness.cpp`) are
what tell you whether a re-applied patch is right.
