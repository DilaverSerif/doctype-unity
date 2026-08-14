// Allocation accounting for experiment E6 (inline style="..." parse cache).
//
// Same interposition method as tests/probe_alloc.cpp in experiment E4: BOTH the
// global operator new/delete family AND the malloc family are replaced, because
// gumbo is C and goes through malloc. Defining malloc in the executable only
// rebinds calls made from our own object files, so libc++abi keeps using
// libSystem's malloc and nothing is double counted.
//
// A separate binary on purpose: tests/bench.cpp is untouched, since adding
// passes to it has been observed to move the timings of untouched code by
// 10-28%.
//
// Reports, per bench page, for a DOCUMENT BUILD (one lhu_load_html on a context
// that has just been created, i.e. a cold cache) and for a warm rebuild:
//   allocations, bytes, and how many of those came through malloc().
//
// Run it twice, LHU_EXP_STYLECACHE=1 and =0, and compare.

#include "lhu_api.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <malloc/malloc.h>

// ---------------------------------------------------------------------------
// tracker
// ---------------------------------------------------------------------------

namespace track
{
bool     g_on       = false;
uint64_t g_allocs   = 0;
uint64_t g_bytes    = 0;
uint64_t g_mallocs  = 0;
uint64_t g_frees    = 0;

inline void note_alloc(size_t n, bool via_malloc)
{
    if(!g_on)
    {
        return;
    }
    ++g_allocs;
    g_bytes += n;
    if(via_malloc)
    {
        ++g_mallocs;
    }
}

inline void note_free()
{
    if(g_on)
    {
        ++g_frees;
    }
}

void reset()
{
    g_allocs = g_bytes = g_mallocs = g_frees = 0;
}
} // namespace track

// --- raw allocator access ----------------------------------------------------
//
// operator new must NOT call std::malloc here: this translation unit also
// defines malloc, so that would recurse into the tracker and double count.

static void* (*g_real_malloc)(size_t)         = nullptr;
static void* (*g_real_calloc)(size_t, size_t) = nullptr;
static void* (*g_real_realloc)(void*, size_t) = nullptr;
static void (*g_real_free)(void*)             = nullptr;
static bool g_resolving                       = false;

static void resolve_real()
{
    if(g_resolving)
    {
        return;
    }
    g_resolving    = true;
    g_real_malloc  = (void* (*) (size_t)) dlsym(RTLD_NEXT, "malloc");
    g_real_calloc  = (void* (*) (size_t, size_t)) dlsym(RTLD_NEXT, "calloc");
    g_real_realloc = (void* (*) (void*, size_t)) dlsym(RTLD_NEXT, "realloc");
    g_real_free    = (void (*)(void*)) dlsym(RTLD_NEXT, "free");
    g_resolving    = false;
}

static inline void* raw_alloc(size_t n)
{
    if(__builtin_expect(g_real_malloc == nullptr, 0))
    {
        resolve_real();
        if(!g_real_malloc)
        {
            return malloc_zone_malloc(malloc_default_zone(), n);
        }
    }
    return g_real_malloc(n);
}

static inline void* raw_calloc(size_t c, size_t n)
{
    if(__builtin_expect(g_real_calloc == nullptr, 0))
    {
        resolve_real();
        if(!g_real_calloc)
        {
            return malloc_zone_calloc(malloc_default_zone(), c, n);
        }
    }
    return g_real_calloc(c, n);
}

static inline void raw_free(void* p)
{
    if(__builtin_expect(g_real_free == nullptr, 0))
    {
        resolve_real();
        if(!g_real_free)
        {
            malloc_zone_t* z = malloc_zone_from_ptr(p);
            malloc_zone_free(z ? z : malloc_default_zone(), p);
            return;
        }
    }
    g_real_free(p);
}

// --- interposed operator new / delete ---------------------------------------

void* operator new(size_t n)
{
    if(n == 0)
    {
        n = 1;
    }
    void* p = raw_alloc(n);
    if(!p)
    {
        throw std::bad_alloc();
    }
    track::note_alloc(n, false);
    return p;
}
void* operator new[](size_t n)
{
    return operator new(n);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept
{
    void* p = raw_alloc(n ? n : 1);
    track::note_alloc(n ? n : 1, false);
    return p;
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept
{
    return operator new(n, std::nothrow);
}
void operator delete(void* p) noexcept
{
    if(!p)
    {
        return;
    }
    track::note_free();
    raw_free(p);
}
void operator delete[](void* p) noexcept
{
    operator delete(p);
}
void operator delete(void* p, size_t) noexcept
{
    operator delete(p);
}
void operator delete[](void* p, size_t) noexcept
{
    operator delete(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    operator delete(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    operator delete(p);
}

// --- interposed malloc family (gumbo, stb_truetype) --------------------------

extern "C" {

void* malloc(size_t n)
{
    void* p = raw_alloc(n ? n : 1);
    track::note_alloc(n ? n : 1, true);
    return p;
}

void* calloc(size_t c, size_t n)
{
    void* p = raw_calloc(c, n);
    track::note_alloc(c * n, true);
    return p;
}

void* realloc(void* old, size_t n)
{
    if(old)
    {
        track::note_free();
    }
    if(!g_real_realloc)
    {
        resolve_real();
    }
    void* p = g_real_realloc ? g_real_realloc(old, n ? n : 1)
                             : malloc_zone_realloc(malloc_default_zone(), old, n ? n : 1);
    track::note_alloc(n ? n : 1, true);
    return p;
}

void free(void* p)
{
    if(!p)
    {
        return;
    }
    track::note_free();
    raw_free(p);
}

char* strdup(const char* s)
{
    const size_t n = std::strlen(s) + 1;
    char*        p = static_cast<char*>(malloc(n));
    if(p)
    {
        std::memcpy(p, s, n);
    }
    return p;
}

} // extern "C"

// ---------------------------------------------------------------------------
// pages (copied verbatim from tests/bench.cpp)
// ---------------------------------------------------------------------------

namespace
{

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

std::string page_hud()
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#fff'>"
                       "<div style='padding:8px'>"
                       "<div id='hp' style='font-size:22px'>HP 84 / 100</div>"
                       "<div style='height:8px;border-radius:4px;background:#333'>"
                       "<i style='display:block;width:84%;height:8px;border-radius:4px;background:#e11'></i></div>"
                       "<div style='margin-top:6px;font-size:13px;color:#aaa'>Bolge: Kuzey Gecidi</div>"
                       "</div></body>");
}

std::string page_menu()
{
    return std::string("<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                       "<div style='margin:14px'>"
                       "<div style='margin-bottom:12px'>"
                       "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Genel</span>"
                       "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Animasyon</span>"
                       "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Performans</span>"
                       "<span style='padding:6px 13px;border-radius:8px;background:#1a1f2e'>Tipografi</span>"
                       "</div>"
                       "<div style='padding:16px 18px;border-radius:14px;border:1px solid #2a3350;"
                       "background:linear-gradient(135deg,#161b29,#1d2540)'>"
                       "<h1 style='font-size:21px;margin:0 0 4px 0'>Ayarlar</h1>"
                       "<p style='margin:0 0 14px 0;color:#8e97b3;font-size:13px'>Oyun ve goruntu secenekleri</p>"
                       "<table style='width:100%;border-collapse:collapse;font-size:13px'>"
                       "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Cozunurluk</td>"
                       "<td id='res' style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>"
                       "1920x1080</td></tr>"
                       "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Golgeler</td>"
                       "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>Yuksek</td></tr>"
                       "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Doku kalitesi</td>"
                       "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>Ultra</td></tr>"
                       "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Dikey esitleme</td>"
                       "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>Kapali</td></tr>"
                       "<tr><td style='padding:6px 8px;border-bottom:1px solid #232b45'>Kare siniri</td>"
                       "<td style='padding:6px 8px;border-bottom:1px solid #232b45;text-align:right'>61 fps</td></tr>"
                       "</table>"
                       "<div style='margin-top:14px'>"
                       "<button style='padding:9px 18px;border-radius:9px;background:#233056;color:#dbe6ff'>"
                       "Kaydet</button>"
                       "<button style='padding:9px 18px;border-radius:9px;background:#233056;color:#dbe6ff'>"
                       "Geri</button>"
                       "</div></div></div></body>");
}

std::string page_list(int rows)
{
    std::string out = "<body style='margin:0;font-family:sans-serif;color:#e6e9f0;background:#0f1117'>"
                      "<div style='margin:10px'>";
    const int   marked = rows / 2;
    for(int i = 0; i < rows; ++i)
    {
        const bool is_marked = i == marked;
        out += "<div style='padding:8px 10px;margin-bottom:6px;border-radius:8px;background:#171c2b;"
               "border:1px solid #232b45'>"
               "<span" +
               std::string(is_marked ? " id='mark'" : "") + " style='font-size:14px'>Esya " + std::to_string(i + 1) +
               "</span>"
               "<span style='float:right;color:#8e97b3;font-size:13px'>x" +
               std::to_string((i * 7) % 40 + 1) + "</span></div>";
    }
    out += "</div></body>";
    return out;
}

std::vector<uint8_t> g_regular;
std::vector<uint8_t> g_bold;

LhuContext* make_ctx()
{
    LhuContext* ctx = lhu_create(nullptr);
    lhu_register_font(ctx, "sans-serif", 400, 0, g_regular.data(), static_cast<int32_t>(g_regular.size()));
    if(!g_bold.empty())
    {
        lhu_register_font(ctx, "sans-serif", 700, 0, g_bold.data(), static_cast<int32_t>(g_bold.size()));
    }
    lhu_set_default_font(ctx, "sans-serif", 16.f);
    return ctx;
}

struct Counts
{
    uint64_t allocs = 0, bytes = 0, mallocs = 0;
};

Counts snap()
{
    return Counts {track::g_allocs, track::g_bytes, track::g_mallocs};
}

void report(const char* page, const char* phase, const Counts& c)
{
    std::printf("  %-20s %-16s %8llu allocs  %10.2f KB  (%llu via malloc)\n", page, phase,
                (unsigned long long) c.allocs, c.bytes / 1024.0, (unsigned long long) c.mallocs);
}

// One page. `cold` builds the document on a context whose cache has never been
// used; `warm` is the second build on the same context.
void measure(const char* name, const std::string& html, float w, float h)
{
    // cold: fresh context, one document build
    {
        LhuContext* ctx = make_ctx();
        lhu_set_viewport(ctx, w, h);
        // Warm the font atlas so glyph rasterization is not counted as parse.
        lhu_load_html(ctx, html.c_str(), nullptr);
        lhu_layout(ctx, w);
        LhuFrame f {};
        lhu_record(ctx, &f);
        lhu_destroy(ctx);
    }

    LhuContext* ctx = make_ctx();
    lhu_set_viewport(ctx, w, h);

    // Prime fonts/atlas on this context so those allocations are not attributed
    // to the measured build. The cache is deliberately NOT primed: this first
    // build is the cold-cache number.
    lhu_load_html(ctx, "<body style='font-family:sans-serif'>x</body>", nullptr);
    lhu_layout(ctx, w);
    LhuFrame f0 {};
    lhu_record(ctx, &f0);

    track::reset();
    track::g_on = true;
    lhu_load_html(ctx, html.c_str(), nullptr);
    track::g_on = false;
    const Counts cold = snap();

    track::reset();
    track::g_on = true;
    lhu_load_html(ctx, html.c_str(), nullptr);
    track::g_on = false;
    const Counts warm = snap();

    lhu_layout(ctx, w);
    LhuFrame f {};
    lhu_record(ctx, &f);

    track::reset();
    track::g_on = true;
    lhu_layout(ctx, w);
    track::g_on = false;
    const Counts layout = snap();

    track::reset();
    track::g_on = true;
    LhuFrame f2 {};
    lhu_record(ctx, &f2);
    track::g_on = false;
    const Counts record = snap();

    report(name, "build (cold)", cold);
    report(name, "build (warm)", warm);
    report(name, "layout", layout);
    report(name, "record", record);

    lhu_destroy(ctx);
}

} // namespace

int main()
{
    const char* font_env             = std::getenv("LHU_FONT");
    const char* regular_candidates[] = {font_env, "/System/Library/Fonts/Supplemental/Arial.ttf",
                                        "/system/fonts/Roboto-Regular.ttf"};
    for(const char* c : regular_candidates)
    {
        if(c && !(g_regular = read_file(c)).empty())
        {
            break;
        }
    }
    if(g_regular.empty())
    {
        std::printf("no usable font found; set LHU_FONT\n");
        return 1;
    }
    g_bold = read_file("/System/Library/Fonts/Supplemental/Arial Bold.ttf");

    const char* v = std::getenv("LHU_EXP_STYLECACHE");
    std::printf("LHU_EXP_STYLECACHE=%s  (cache %s)\n", v ? v : "(unset)",
                (v && !std::strcmp(v, "0")) ? "OFF" : "ON");
    std::printf("  %-20s %-16s %8s %13s\n", "page", "phase", "allocs", "bytes");

    measure("HUD", page_hud(), 420.f, 120.f);
    measure("Settings menu", page_menu(), 680.f, 460.f);
    measure("Inventory 40", page_list(40), 480.f, 900.f);
    measure("Inventory 150", page_list(150), 480.f, 900.f);
    return 0;
}
