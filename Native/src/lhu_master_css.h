// A trimmed default stylesheet for game UI.
//
// litehtml re-parses its default stylesheet for every document it creates, and
// that single step is ~95% of the cost of building a document — far more than
// parsing the page's own markup. Measured with Native/tests/probe_parse.cpp on
// an Apple Silicon Mac:
//
//   createFromString + render, litehtml's master css   0.364 ms
//   createFromString + render, this stylesheet         0.154 ms
//   createFromString + render, no stylesheet at all    0.017 ms
//
// The saving comes from two places: less CSS to tokenize, and fewer selectors to
// match against every element afterwards.
//
// WHAT IS DROPPED relative to litehtml's own master.css:
//   * form controls: input, select, textarea, option, optgroup, keygen, isindex
//   * legacy presentational tags: marquee, xmp, plaintext, listing, dir, menu,
//     tt, strike, nobr, blink
//   * definition lists (dl/dt/dd), figure/figcaption, blockquote indentation,
//     del/ins/cite/samp/kbd/var/abbr/acronym
//   * sub/sup vertical alignment
//   * the table[align] / table[border] attribute selectors
//   * nested list-style-type alternation (ul ul, ol ol ...)
//
// Everything a game UI actually uses — the box model, headings, inline styling,
// links, lists, tables, preformatted text — is kept and matches litehtml's
// values exactly.
//
// Use LHU_MASTER_CSS_FULL when a page needs any of the above.

#ifndef LHU_MASTER_CSS_H
#define LHU_MASTER_CSS_H

namespace lhu
{

inline const char* trimmed_master_css()
{
    return R"CSS(
html { display: block; position: relative; }
head, style, script, title, meta, link, base { display: none; }
body { display: block; margin: 8px; }

div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, hr, pre, form, article, section,
header, footer, nav, aside, main, figure { display: block; }

span, a, b, strong, i, em, u, s, small, code, label { display: inline; }

p { margin: 1em 0; }
h1 { font-size: 2em;    font-weight: bold; margin: 0.67em 0; }
h2 { font-size: 1.5em;  font-weight: bold; margin: 0.83em 0; }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0; }
h4 {                    font-weight: bold; margin: 1.33em 0; }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }

b, strong { font-weight: bold; }
i, em { font-style: italic; }
u { text-decoration: underline; }
s { text-decoration: line-through; }
small { font-size: smaller; }

a[href] { color: #0000EE; text-decoration: underline; cursor: pointer; }

ul, ol { margin: 1em 0; padding-left: 40px; }
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
li { display: list-item; }

br { display: inline-block; }
img { display: inline-block; }
hr { display: block; margin: 0.5em auto; border-width: 1px; border-style: inset; }

pre, code { font-family: monospace; }
pre { display: block; margin: 1em 0; white-space: pre; }

table { display: table; border-collapse: separate; border-spacing: 2px;
        border-color: gray; }
caption { display: table-caption; text-align: center; }
thead { display: table-header-group; vertical-align: middle; border-color: inherit; }
tbody { display: table-row-group; vertical-align: middle; border-color: inherit; }
tfoot { display: table-footer-group; vertical-align: middle; border-color: inherit; }
tr { display: table-row; vertical-align: inherit; border-color: inherit; }
td, th { display: table-cell; padding: 1px; vertical-align: inherit; }
th { font-weight: bold; text-align: center; }

button { display: inline-block; }
)CSS";
}

} // namespace lhu

#endif // LHU_MASTER_CSS_H
