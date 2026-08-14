#include "utf8_strings.h"
#include "document_container.h"

void litehtml::document_container::split_text(const char* text, const std::function<void(const char*)>& on_word,
                                              const std::function<void(const char*)>& on_space)
{
    std::u32string str;
    std::u32string str_in = static_cast<const char32_t*>(utf8_to_utf32(text));
    for(auto c : str_in)
    {
        if(c <= ' ' && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'))
        {
            if(!str.empty())
            {
                on_word(utf32_to_utf8(str));
                str.clear();
            }
            str += c;
            on_space(utf32_to_utf8(str));
            str.clear();
        }
        // CJK character range
        else if(c >= 0x4E00 && c <= 0x9FCC)
        {
            if(!str.empty())
            {
                on_word(utf32_to_utf8(str));
                str.clear();
            }
            str += c;
            on_word(utf32_to_utf8(str));
            str.clear();
        } else
        {
            str += c;
        }
    }
    if(!str.empty())
    {
        on_word(utf32_to_utf8(str));
    }
}

// --- LHU PATCH (experiment E6): inline-style parse cache ---------------------
//
// See the comment on document_container::cached_inline_style in
// document_container.h for why memoizing this is safe. This file holds the
// implementation so that style.h stays out of document_container.h.
//
// Toggle: LHU_EXP_STYLECACHE=0 disables the cache and puts
// html_tag::compute_styles back on litehtml's original code path exactly. Read
// once, so a single binary can be A/B'd by the launcher without a rebuild.

#include "style.h"

#include <cstdlib>
#include <string_view>

namespace
{
bool lhu_inline_style_cache_enabled()
{
    static const bool enabled = []() {
        const char* v = std::getenv("LHU_EXP_STYLECACHE");
        return !(v && v[0] == '0' && v[1] == '\0'); // default ON, only "0" disables
    }();
    return enabled;
}

// A page uses a handful of distinct style strings; this only exists so that a
// long-lived context that loads unboundedly many different pages cannot grow the
// cache without limit. Past the cap we simply stop memoizing and parse directly,
// which changes nothing but the speed.
constexpr size_t kLhuInlineStyleCacheMax = 1024;
} // namespace

const litehtml::style* litehtml::document_container::cached_inline_style(const char* text)
{
    if(!text || !lhu_inline_style_cache_enabled())
    {
        return nullptr;
    }

    const std::string_view key(text);
    auto                   it = m_inline_style_cache.find(key);
    if(it != m_inline_style_cache.end())
    {
        return it->second.get();
    }

    // LHU PATCH (experiment E7): a caller that knows this string is a one-shot turns
    // insertion off. Missing here is not an error -- the caller parses directly.
    if(!m_inline_style_memoize || m_inline_style_cache.size() >= kLhuInlineStyleCacheMax)
    {
        return nullptr;
    }

    auto parsed = std::make_shared<litehtml::style>();
    // Exactly the call html_tag::compute_styles() would have made.
    parsed->add(std::string(text), "", this);

    return m_inline_style_cache.emplace(std::string(text), std::move(parsed)).first->second.get();
}
