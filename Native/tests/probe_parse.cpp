// Isolates where document-creation time actually goes.
//
// The benchmark showed a ~0.5 ms floor even for a 200-byte page, which is far
// too much to be about content. This splits creation into its parts to find the
// fixed cost.

#include "lhu_container.h"
#include "lhu_font.h"

#include <litehtml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if(!f)
    {
        return {};
    }

    const std::streamsize n = f.tellg();
    f.seekg(0);

    std::vector<uint8_t> out(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    return out;
}

template <typename Fn>
double timed(int iterations, Fn&& fn)
{
    std::vector<double> samples;
    samples.reserve(iterations);

    for(int i = 0; i < iterations; ++i)
    {
        const auto t0 = Clock::now();
        fn();
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }

    return median(std::move(samples));
}

} // namespace

int main(int argc, char** argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 300;

    lhu::FontManager fonts;

    const auto arial = read_file("/System/Library/Fonts/Supplemental/Arial.ttf");
    if(arial.empty())
    {
        std::printf("no system font\n");
        return 1;
    }

    fonts.register_font("sans-serif", 400, false, arial.data(), arial.size());
    fonts.set_default_family("sans-serif");

    LhuHostCallbacks host {};
    lhu::Container   container(fonts, host);
    container.set_viewport(680.f, 460.f);

    const std::string tiny = "<body><p>merhaba</p></body>";
    const std::string master(litehtml::master_css);

    std::printf("master_css is %zu bytes\n\n", master.size());

    // 1. What the wrapper does today.
    const double with_master = timed(iterations, [&] {
        auto doc = litehtml::document::createFromString(litehtml::estring(tiny), &container, master, "");
        doc->render(litehtml::pixel_t(680.f));
    });

    // 2. The same, with no master stylesheet at all. The difference is what
    //    parsing and sorting the default stylesheet costs.
    const double without_master = timed(iterations, [&] {
        auto doc = litehtml::document::createFromString(litehtml::estring(tiny), &container, "", "");
        doc->render(litehtml::pixel_t(680.f));
    });

    // 3. Parsing the master stylesheet on its own, nothing else.
    const double parse_master = timed(iterations, [&] {
        litehtml::css sheet;
        auto          doc = litehtml::document::createFromString(litehtml::estring("<body></body>"), &container, "", "");
        sheet.parse_css_stylesheet(master, "", doc);
        sheet.sort_selectors();
    });

    // 4. HTML parsing alone (gumbo + element tree), no stylesheet, no layout.
    const double html_only = timed(iterations, [&] {
        auto doc = litehtml::document::createFromString(litehtml::estring(tiny), &container, "", "");
        (void) doc;
    });

    // A game UI never uses form controls, ruby, legacy presentational tags or
    // most of the table variants. This keeps the box model and typography rules
    // that actually matter and drops the rest.
    static const char* const kTrimmedMaster = R"CSS(
html { display:block; position:relative; }
head,style,script,title,meta,link { display:none; }
body { display:block; margin:8px; }
div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,hr,pre,figure,blockquote { display:block; }
p { margin:1em 0; }
h1 { font-size:2em; font-weight:bold; margin:.67em 0; }
h2 { font-size:1.5em; font-weight:bold; margin:.83em 0; }
h3 { font-size:1.17em; font-weight:bold; margin:1em 0; }
h4 { font-weight:bold; margin:1.33em 0; }
h5 { font-size:.83em; font-weight:bold; margin:1.67em 0; }
h6 { font-size:.67em; font-weight:bold; margin:2.33em 0; }
b,strong { font-weight:bold; }
i,em { font-style:italic; }
u { text-decoration:underline; }
s { text-decoration:line-through; }
a { color:#00e; text-decoration:underline; }
ul,ol { margin:1em 0; padding-left:40px; }
ul { list-style-type:disc; }
ol { list-style-type:decimal; }
li { display:list-item; }
br { display:inline-block; }
img { display:inline-block; }
pre,code { font-family:monospace; }
pre { display:block; margin:1em 0; white-space:pre; }
hr { display:block; margin:.5em auto; border:1px inset; }
table { display:table; border-collapse:separate; border-spacing:2px; }
tbody { display:table-row-group; }
thead { display:table-header-group; }
tfoot { display:table-footer-group; }
tr { display:table-row; }
td,th { display:table-cell; padding:1px; vertical-align:inherit; }
th { font-weight:bold; text-align:center; }
caption { display:table-caption; text-align:center; }
button { display:inline-block; }
span { display:inline; }
)CSS";

    const std::string trimmed(kTrimmedMaster);

    const double with_trimmed = timed(iterations, [&] {
        auto doc = litehtml::document::createFromString(litehtml::estring(tiny), &container, trimmed, "");
        doc->render(litehtml::pixel_t(680.f));
    });

    std::printf("%-46s %8s\n", "", "median ms");
    std::printf("%-46s %8.3f\n", "createFromString + render, with master css", with_master);
    std::printf("%-46s %8.3f\n", "createFromString + render, no master css", without_master);
    std::printf("%-46s %8.3f\n", "parse+sort master css alone", parse_master);
    std::printf("%-46s %8.3f\n", "html parse + element tree only", html_only);
    std::printf("%-46s %8.3f   (%zu bytes)\n", "createFromString + render, trimmed master", with_trimmed,
                trimmed.size());

    std::printf("\nmaster css is %.0f%% of a full document creation (%.3f of %.3f ms)\n",
                (with_master - without_master) / with_master * 100.0, with_master - without_master, with_master);
    std::printf("trimming it saves %.3f ms per document (%.0f%% of creation)\n", with_master - with_trimmed,
                (with_master - with_trimmed) / with_master * 100.0);

    return 0;
}
