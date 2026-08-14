# Measured but not applied

Two experiments that were built and benchmarked on a Dimensity 1080 but are not
in the tree. Both apply to the tree as it stood before `lhu_set_text` landed, so
expect to rebase them.

## master-css-cache.diff

Parses litehtml's default stylesheet once per context instead of once per
document. Verified byte-identical over a 41-page-load script covering both
master-CSS modes, mode switches, two live contexts and quirks/no-quirks pages.

Worth a fixed ~0.4 ms per document load on an A78 core, not a percentage: it is
decisive on small pages (HUD parse 0.566 -> 0.133 ms) and lost in the noise on
large ones. Its value dropped once `lhu_set_text` removed most re-parses, so it
now only pays on page switches and first load.

Carries a guard that disables sharing if either master sheet ever gains an
`@media` block or a class/id selector, because those are the two things that
make a parsed sheet document-dependent.

## viewport-clip-WIP.diff

Restricts recording to a rectangle, so a list taller than the viewport stops
emitting quads for rows nobody can see. Culls 52% of the quads on the 150-row
page, which roughly halves both vertex upload and overdraw.

NOT SHIPPABLE AS-IS. Two defects, both only when the clip is scrolled (y != 0):
`position:fixed` elements are culled away, and the page backdrop stays pinned to
the document origin so the scrolled region has no background. The first is
caused by `Container::get_viewport` always reporting origin (0,0).
