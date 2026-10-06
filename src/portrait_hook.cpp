// Hooks CPortraitObject::UpdatePortrait, the engine's per-frame renderer of one visible portrait. The engine draws the
// portrait's 2D layers and its skeletal "room" into a render target (a TextureGFX, a wrapper around a D3D11 texture) that
// the GUI then shows through a masked sprite. After the original returns, the plugin may overwrite that render target:
// the frame, the mask and the visibility handling all stay the game's.
//
// The engine's own TextureGFX layout is not located statically. The first time a render target is seen, its memory is
// scanned for a COM object that is a D3D11 texture of exactly the portrait's size, bound as a render target; the offset
// that matched is remembered and re-validated on every use.
#include "live2d.hpp"
#include "utf8_path.hpp"
#include "portrait_live2d.hpp"
#include "voice.hpp"
#include "portrait_input.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace l2d {

float GameVolume(const std::string& channel);  // defined below, outside the unnamed namespace

// ---- log -----------------------------------------------------------------------------------------------------

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}

std::string PluginDir() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH * 4] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&PluginDir, &self))
        GetModuleFileNameW(self, path, (DWORD)(sizeof path / sizeof path[0]));
    std::wstring s(path);
    return U8(s.substr(0, s.find_last_of(L"\\/") + 1));
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) {
        const std::wstring dir = W(PluginDir() + "logs");
        CreateDirectoryW(dir.c_str(), nullptr);
        g_log = _wfopen((dir + L"\\stellaris_live2d.log").c_str(), L"a");
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

namespace {

using FnUpdatePortrait = void (*)(void* portrait, void* graphics, void* context);
using FnRender = void (*)(void* self, void* gui, void* context, const float* matrix, float alpha, uint16_t state, void* texture);
using FnGreeting = void* (*)(void* portrait);

uintptr_t g_base = 0;
bool g_installed = false;
FnUpdatePortrait g_orig_update = nullptr;
FnRender g_orig_render = nullptr;
FnGreeting g_orig_greeting = nullptr;
std::atomic<int> g_in_hook{ 0 };  // detours currently running, drained on unload

// what the hook paints after the engine rendered a portrait: 0 nothing, 1 the test pattern, 2 the Live2D model
std::atomic<int> g_mode{ 0 };
std::atomic<int> g_only_w{ 0 }, g_only_h{ 0 };

// counters, read by StatsLine
std::atomic<uint64_t> g_calls{ 0 }, g_painted{ 0 }, g_no_rt{ 0 }, g_filtered{ 0 }, g_no_texture{ 0 }, g_bad_format{ 0 },
    g_faults{ 0 };
std::atomic<uint64_t> g_l2d_failed{ 0 }, g_unregistered{ 0 };
std::atomic<uint64_t> g_engine_renders{ 0 };  // calls in which the engine itself had the portrait flagged for re-rendering
std::atomic<uint64_t> g_fill_ticks{ 0 }, g_upload_ticks{ 0 };  // QueryPerformanceCounter ticks spent filling / uploading
std::atomic<int> g_painting{ 0 };  // paints in progress; the restore waits for it to reach 0

uint64_t Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

const uint64_t g_ticks_per_second = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (uint64_t)f.QuadPart;
}();

// offset inside a TextureGFX at which the ID3D11Texture2D pointer sits; -1 until discovered
std::atomic<int> g_tex_off{ -1 };
constexpr int kProbeBytes = 0x200;

// ---- D3D11 texture discovery (no C++ objects with destructors in the __try functions) ---------------------------

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

bool InD3D11(const void* address) {
    static HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    HMODULE mod = nullptr;
    return d3d11 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)address, &mod) && mod == d3d11;
}

// Is `p` a D3D11 texture of w x h usable as a render target? Fills *desc when it is.
bool IsPortraitTexture(void* p, UINT w, UINT h, D3D11_TEXTURE2D_DESC* desc) {
    __try {
        if (!Readable(p, 8)) return false;
        void* vtable = *(void**)p;
        if (!Readable(vtable, 8) || !InD3D11(vtable)) return false;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(((IUnknown*)p)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return false;
        tex->GetDesc(desc);
        tex->Release();
        return desc->Width == w && desc->Height == h && (desc->BindFlags & D3D11_BIND_RENDER_TARGET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int DiscoverTextureOffset(const uint8_t* texture_gfx, UINT w, UINT h, D3D11_TEXTURE2D_DESC* desc) {
    __try {
        for (int off = 0; off + 8 <= kProbeBytes; off += 8) {
            void* p = *(void* const*)(texture_gfx + off);
            if ((uintptr_t)p > 0x10000 && IsPortraitTexture(p, w, h, desc)) return off;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

// ---- the test pattern -----------------------------------------------------------------------------------------

std::vector<uint32_t> g_pixels;  // render thread only

// RGBA bytes packed for the target's format; false when the format is not one this proof of concept writes
bool PackFormat(DXGI_FORMAT f, bool* bgra) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: *bgra = false; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: *bgra = true; return true;
    default: return false;
    }
}

void FillPattern(uint32_t w, uint32_t h, bool bgra, uint32_t seed, uint32_t frame) {
    g_pixels.resize((size_t)w * h);
    const uint32_t r0 = 60 + (seed * 37u) % 150, g0 = 60 + (seed * 91u) % 150, b0 = 60 + (seed * 53u) % 150;
    const uint32_t bar = (frame * 6u) % w;
    auto pack = [&](uint32_t r, uint32_t g, uint32_t b) -> uint32_t {
        return bgra ? (0xFFu << 24 | r << 16 | g << 8 | b) : (0xFFu << 24 | b << 16 | g << 8 | r);
    };
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t shade = 255 - (y * 120u) / h;  // darker towards the bottom
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t r = r0 * shade / 255, g = g0 * shade / 255, b = b0 * shade / 255;
            if (x % 32 == 0 || y % 32 == 0) r = g = b = 235;                        // grid
            if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) r = g = b = 255;         // border
            if (x >= bar && x < bar + 14) { r = 255; g = 230; b = 40; }               // moving bar
            if (x < 48 && y < 48) { r = 255; g = 40; b = 40; }                        // top-left: red
            if (x >= w - 48 && y >= h - 48) { r = 40; g = 90; b = 255; }              // bottom-right: blue
            g_pixels[(size_t)y * w + x] = pack(r, g, b);
        }
    }
}

// The key of the portrait the engine shows in this object (`human_female_01`), copied into `out`; empty when unreadable.
// An engine CString keeps up to 15 characters inline, longer ones behind a pointer.
void ReadCString(const uint8_t* str, char* out, size_t cap) {
    out[0] = 0;
    __try {
        const uint64_t capacity = *(const uint64_t*)(str + sdk::cstring::kCapacity);
        const uint64_t length = *(const uint64_t*)(str + sdk::cstring::kLength);
        const char* data = capacity >= sdk::cstring::kInlineCapacity ? *(const char* const*)(str + sdk::cstring::kInline)
                                                                     : (const char*)(str + sdk::cstring::kInline);
        if (!data || length == 0 || length >= cap || length > 256) return;
        for (uint64_t i = 0; i < length; ++i) {
            const char c = data[i];
            if (c < 0x20 || c > 0x7E) { out[0] = 0; return; }  // not a portrait key: do not trust it
            out[i] = c;
        }
        out[length] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

void ReadPortraitKey(const uint8_t* portrait, char* out, size_t cap) { ReadCString(portrait + sdk::rt::CPortraitObject_key, out, cap); }

// What the portrait was picked for, as text for the log: `pop group #12 of human`, `leader #3 of human`, `species #1 of human`.
void DescribePortraitScope(const uint8_t* portrait, char* out, size_t cap) {
    out[0] = 0;
    uint64_t type = 0;
    uint32_t id = 0;
    __try {
        type = *(const uint64_t*)(portrait + sdk::rt::CPortraitObject_scope_type);
        id = *(const uint32_t*)(portrait + sdk::rt::CPortraitObject_scope_id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    char group[64];
    ReadCString(portrait + sdk::rt::CPortraitObject_group, group, sizeof group);
    const char* name = type == 0x100 ? "leader" : type == 0x800 ? "species" : type == 0x20 ? "pop group" : nullptr;
    char what[32];
    if (!name) {
        snprintf(what, sizeof what, "scope 0x%llX", (unsigned long long)type);
        name = what;
    }
    if (id != 0xFFFFFFFFu) snprintf(out, cap, "%s #%u of %s", name, id, group[0] ? group : "?");
    else snprintf(out, cap, "%s of %s", name, group[0] ? group : "?");
}

// Where the GUI draws portraits. The engine's GUI draw (CPortraitObject::Render, once per drawn portrait per frame) is hooked:
// the object is a GUI sprite that holds its absolute position, scale and mirror flag, and the call gets the engine's
// CGuiGraphics, which holds the mouse pointer. The painter runs earlier in the next frame (UpdatePortrait comes before the GUI
// draw) and uses what was recorded. Values are checked for plausibility; a failed check just leaves them out.
struct DrawnRect {
    ScreenRect rect;
    uint64_t ticks = 0;
};
std::unordered_map<const void*, DrawnRect> g_drawn;  // render thread only
std::atomic<void*> g_gui_graphics{ nullptr };

// The GUI keeps its coordinates around the middle of the screen with y up, and a sprite's position is its lower-left corner: the
// result is turned into units from the top-left corner, y down, which is where the pointer is measured from.
bool ReadDrawnRect(void* portrait, void* gui, ScreenRect* out) {
    __try {
        const auto* g = (const uint8_t*)gui;
        const int gui_w = *(const int*)(g + sdk::rt::CGuiGraphics_width), gui_h = *(const int*)(g + sdk::rt::CGuiGraphics_height);
        if (gui_w < 320 || gui_w > 16384 || gui_h < 240 || gui_h > 16384) return false;
        const auto* o = (const uint8_t*)portrait;
        const float x = *(const float*)(o + sdk::rt::CPortraitObject_pos), y = *(const float*)(o + sdk::rt::CPortraitObject_pos + 4);
        const float scale = *(const float*)(o + sdk::rt::CPortraitObject_scale);
        int size[4] = { 0, 0, 0, 0 };
        void** vtable = *(void***)portrait;
        ((void (*)(void*, int*))vtable[sdk::vt::C2dObject_GetSize])(portrait, size);
        const bool sane = std::isfinite(x) && std::isfinite(y) && std::fabs(x) < 30000.0f && std::fabs(y) < 30000.0f &&
                          std::isfinite(scale) && scale > 0.0f && scale < 50.0f && size[0] >= 4 && size[0] <= 8192 && size[1] >= 4 &&
                          size[1] <= 8192;
        if (!sane) return false;
        out->w = (float)size[0];
        out->h = (float)size[1];
        out->x = (float)gui_w * 0.5f + (float)(int)x;
        out->y = (float)gui_h * 0.5f - (float)(int)y - out->h;
        out->mirrored = o[sdk::rt::CPortraitObject_mirrored] != 0;
        if (o[sdk::rt::CPortraitObject_has_scissor] == 1) {
            const int* sc = (const int*)(o + sdk::rt::CPortraitObject_scissor);
            if (sc[0] < sc[2] && sc[1] < sc[3] && sc[0] > -30000 && sc[2] < 30000 && sc[1] > -30000 && sc[3] < 30000) {
                out->has_clip = true;
                out->clip_x0 = sc[0];
                out->clip_y0 = sc[1];
                out->clip_x1 = sc[2];
                out->clip_y1 = sc[3];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadGuiMouse(void* gui, float* x, float* y, float* w, float* h) {
    __try {
        const auto* g = (const uint8_t*)gui;
        *x = *(const float*)(g + sdk::rt::CGuiGraphics_mouse_x);
        *y = *(const float*)(g + sdk::rt::CGuiGraphics_mouse_y);
        *w = (float)*(const int*)(g + sdk::rt::CGuiGraphics_gui_width);    // the pointer is in GUI units: the pixels divided by the UI scale
        *h = (float)*(const int*)(g + sdk::rt::CGuiGraphics_gui_height);
        return std::isfinite(*x) && std::isfinite(*y) && std::fabs(*x) < 30000.0f && std::fabs(*y) < 30000.0f && *w >= 320.0f &&
               *w <= 16384.0f && *h >= 240.0f && *h <= 16384.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// what the painter gets for one portrait: the rectangle from the last GUI draw if that is recent, and the pointer
void BuildPortraitFrame(void* portrait, PortraitFrame* out) {
    *out = PortraitFrame{};
    out->portrait = portrait;
    const auto it = g_drawn.find(portrait);
    if (it != g_drawn.end() && Ticks() - it->second.ticks < g_ticks_per_second / 4) {
        out->has_rect = true;
        out->rect = it->second.rect;
    }
    if (void* gui = g_gui_graphics.load()) out->has_mouse = ReadGuiMouse(gui, &out->mouse_x, &out->mouse_y, &out->gui_w, &out->gui_h);
}

// What the portrait is for: 0 character, 1 character_large, 2 room, 3 empty_room, 4 character_without_room, 5 planet; -1 when unreadable.
int ReadPortraitKind(const uint8_t* portrait) {
    __try {
        const int kind = *(const int32_t*)(portrait + sdk::rt::CPortraitObject_kind);
        return kind >= 0 && kind <= 5 ? kind : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// One portrait: after the engine rendered it, overwrite its render target.
void PaintPortrait(void* portrait) {
    const auto* base = (const uint8_t*)portrait;
    const UINT w = *(const uint16_t*)(base + sdk::rt::CPortraitObject_width);
    const UINT h = *(const uint16_t*)(base + sdk::rt::CPortraitObject_height);
    const auto* rt = *(const uint8_t* const*)(base + sdk::rt::CPortraitObject_render_target);
    if (!rt || !w || !h) { ++g_no_rt; return; }
    const int ow = g_only_w.load(), oh = g_only_h.load();
    if ((ow && (int)w != ow) || (oh && (int)h != oh)) { ++g_filtered; return; }

    D3D11_TEXTURE2D_DESC desc;
    int off = g_tex_off.load();
    if (off < 0) {
        off = DiscoverTextureOffset(rt, w, h, &desc);
        if (off < 0) {
            if (++g_no_texture == 1) Log("no D3D11 texture found in the first 0x%X bytes of the render target %p (%ux%u)", kProbeBytes, rt, w, h);
            return;
        }
        g_tex_off = off;
        Log("TextureGFX holds its ID3D11Texture2D at +0x%X: %ux%u format %d bind 0x%X usage %d mips %u samples %u array %u",
            off, desc.Width, desc.Height, (int)desc.Format, desc.BindFlags, (int)desc.Usage, desc.MipLevels,
            desc.SampleDesc.Count, desc.ArraySize);
    }
    auto* tex = *(ID3D11Texture2D* const*)(rt + off);
    if (!IsPortraitTexture(tex, w, h, &desc)) { ++g_no_texture; return; }
    bool bgra = false;
    if (!PackFormat(desc.Format, &bgra) || desc.SampleDesc.Count != 1) {
        if (++g_bad_format == 1) Log("render target format %d / %u samples is not supported by the test pattern", (int)desc.Format, desc.SampleDesc.Count);
        return;
    }
    if (g_mode.load() == 2) {
        char key[96];
        ReadPortraitKey(base, key, sizeof key);
        char scope[112];
        DescribePortraitScope(base, scope, sizeof scope);
        switch (Painter().Paint(portrait, key, ReadPortraitKind(base), scope, tex, desc)) {
        case PaintResult::Painted: ++g_painted; break;
        case PaintResult::Skipped: ++g_unregistered; break;
        default: ++g_l2d_failed; break;
        }
        return;
    }
    static uint32_t frame = 0;
    const uint64_t t0 = Ticks();
    FillPattern(w, h, bgra, (uint32_t)((uintptr_t)portrait >> 4), ++frame);
    const uint64_t t1 = Ticks();
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        ctx->UpdateSubresource(tex, 0, nullptr, g_pixels.data(), w * 4, 0);
        ctx->Release();
        ++g_painted;
        g_fill_ticks += t1 - t0;
        g_upload_ticks += Ticks() - t1;
    }
    dev->Release();
}

void PaintGuarded(void* portrait) {
    __try {
        PaintPortrait(portrait);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

// Every portrait object the engine has, visible or not: the engine only re-renders a portrait while its "needs render"
// flag is set, so after the plugin has painted over a render target the flag is what brings the original picture back.
void MarkAllPortraitsDirty() {
    __try {
        auto** data = *(uint8_t***)(g_base + sdk::glob::CPortraitObjectController_PortraitObjects_data);
        const int count = *(const int*)(g_base + sdk::glob::CPortraitObjectController_PortraitObjects_count);
        if (!data || count <= 0 || count > 100000) return;
        for (int i = 0; i < count; ++i) {
            if (data[i]) data[i][sdk::rt::CPortraitObject_needs_render] = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

// Stops painting and puts the game's own pictures back. A paint begins by counting itself in and only then looks at
// g_mode, so once g_painting has dropped to 0 after g_mode went 0, no paint is running or can start.
void StopPainting() {
    const bool was_on = g_mode.exchange(0) != 0;
    for (int i = 0; i < 500 && g_painting.load() != 0; ++i) Sleep(2);
    if (was_on) {
        MarkAllPortraitsDirty();
        Log("test pattern off: every portrait marked for re-render");
    }
}

// the first few portraits' rectangles go to the log so they can be compared with a screenshot
void LogFrameOnce(void* portrait, const PortraitFrame& f) {
    static int logged = 0;
    if (logged >= 12 || !f.has_rect) return;
    char key[96];
    ReadPortraitKey((const uint8_t*)portrait, key, sizeof key);
    static char seen[12][96];
    for (int i = 0; i < logged; ++i)
        if (!strcmp(seen[i], key)) return;
    strcpy_s(seen[logged++], key);
    Log("portrait %s: GUI rect x %.0f y %.0f size %.0fx%.0f mirrored %d, clip %s(%d,%d)-(%d,%d), pointer %s(%.1f, %.1f)", key, f.rect.x, f.rect.y,
        f.rect.w, f.rect.h, (int)f.rect.mirrored, f.rect.has_clip ? "" : "none ", f.rect.clip_x0, f.rect.clip_y0, f.rect.clip_x1, f.rect.clip_y1,
        f.has_mouse ? "" : "unknown ", f.mouse_x, f.mouse_y);
}

// What the exe cannot confirm without live objects, checked once the first portrait is on screen and written to the log as `selftest:` lines
// (tools/live_verify.py reads them): the portrait key and kind, the rectangle, the window and GUI sizes against the real window, the game's volumes,
// and, once the game window is in front with the pointer inside it, the pointer the engine reports against the one the system reports.
bool ReadGuiSizes(void* gui, int* w, int* h, int* gw, int* gh) {
    __try {
        const auto* g = (const uint8_t*)gui;
        *w = *(const int*)(g + sdk::rt::CGuiGraphics_width);
        *h = *(const int*)(g + sdk::rt::CGuiGraphics_height);
        *gw = *(const int*)(g + sdk::rt::CGuiGraphics_gui_width);
        *gh = *(const int*)(g + sdk::rt::CGuiGraphics_gui_height);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SelfTest(void* portrait, const PortraitFrame& f) {
    static bool basic = false, pointer = false;
    if (basic && pointer) return;
    void* gui = g_gui_graphics.load();
    if (!gui) return;
    if (!basic && f.has_rect) {
        basic = true;
        int pass = 0, fail = 0;
        auto report = [&](const char* name, bool ok, const char* detail) {
            Log("selftest: %-9s %s  %s", name, ok ? "PASS" : "FAIL", detail);
            ++(ok ? pass : fail);
        };
        char detail[200], key[96];
        ReadPortraitKey((const uint8_t*)portrait, key, sizeof key);
        report("key", key[0] != 0, key);
        const int kind = ReadPortraitKind((const uint8_t*)portrait);
        snprintf(detail, sizeof detail, "kind %d", kind);
        report("kind", kind >= 0 && kind <= 5, detail);
        int w = 0, h = 0, gw = 0, gh = 0;
        const bool sized = ReadGuiSizes(gui, &w, &h, &gw, &gh);
        const ScreenRect& r = f.rect;
        snprintf(detail, sizeof detail, "x %.0f y %.0f %.0fx%.0f in a GUI of %dx%d", r.x, r.y, r.w, r.h, gw, gh);
        report("rect", sized && r.w >= 4 && r.h >= 4 && r.w <= gw && r.h <= gh && r.x > -gw && r.x < gw && r.y > -gh && r.y < gh, detail);
        int cw = 0, ch = 0;
        if (sized && GameClientSize(&cw, &ch)) {
            snprintf(detail, sizeof detail, "window %dx%d px (client %dx%d), GUI %dx%d units, UI scale %.2f", w, h, cw, ch, gw, gh, gw ? (double)w / gw : 0.0);
            report("sizes", std::abs(w - cw) <= 1 && std::abs(h - ch) <= 1 && gw > 0 && gw <= w && w <= gw * 2 && std::abs((double)w / gw - (double)h / gh) < 0.02, detail);
        } else {
            Log("selftest: sizes     SKIP  the game window is minimized");
        }
        const float voice = GameVolume("voice"), effects = GameVolume("effects");
        snprintf(detail, sizeof detail, "voice %.3f, effects %.3f", voice, effects);
        report("volumes", voice >= 0.0f && effects >= 0.0f, detail);
        Log("selftest: %d passed, %d failed (the pointer check follows once the game window is in front with the pointer inside it)", pass, fail);
    }
    if (basic && !pointer && f.has_mouse && f.gui_w > 0) {
        float px, py, cw, ch;
        if (ClientPointer(&px, &py, &cw, &ch)) {
            pointer = true;
            const float scale = cw / f.gui_w;
            const bool ok = std::fabs(f.mouse_x * scale - px) <= 3.0f && std::fabs(f.mouse_y * (ch / f.gui_h) - py) <= 3.0f;
            Log("selftest: pointer   %s  the engine says (%.0f, %.0f) GUI units = (%.0f, %.0f) px, the system says (%.0f, %.0f) px", ok ? "PASS" : "FAIL", f.mouse_x, f.mouse_y,
                f.mouse_x * scale, f.mouse_y * (ch / f.gui_h), px, py);
        }
    }
}

void UpdatePortraitDetour(void* portrait, void* graphics, void* context) {
    g_in_hook.fetch_add(1);
    // the engine re-renders a portrait only while this flag is set (it clears it itself)
    const bool drawn = ((const uint8_t*)portrait)[sdk::rt::CPortraitObject_needs_render] != 0;
    if (drawn) ++g_engine_renders;
    if (g_mode.load() == 2) {
        PortraitFrame frame;
        BuildPortraitFrame(portrait, &frame);
        SetPortraitFrame(frame);
        LogFrameOnce(portrait, frame);
        SelfTest(portrait, frame);
    }
    g_orig_update(portrait, graphics, context);
    ++g_calls;
    g_painting.fetch_add(1);
    if (g_mode.load()) PaintGuarded(portrait);
    g_painting.fetch_sub(1);
    g_in_hook.fetch_sub(1);
}

// The GUI draws a portrait: note where, then let the engine draw it.
void RenderDetour(void* self, void* gui, void* context, const float* matrix, float alpha, uint16_t state, void* texture) {
    g_in_hook.fetch_add(1);
    if (g_mode.load() == 2) {
        DrawnRect d;
        if (ReadDrawnRect(self, gui, &d.rect)) {
            d.ticks = Ticks();
            g_drawn[self] = d;
        }
        g_gui_graphics = gui;
    }
    g_orig_render(self, gui, context, matrix, alpha, state, texture);
    g_in_hook.fetch_sub(1);
}

// The game plays a portrait's greeting sound (the diplomacy window opens, a species is previewed): emit the portrait's `greeting` event, and
// when the mod's greeting action replaces the sound, hand the caller nothing to play (the callers accept a missing sound).
void* GreetingDetour(void* portrait) {
    g_in_hook.fetch_add(1);
    void* effect = g_orig_greeting(portrait);
    if (effect && g_mode.load() == 2) {
        char key[96];
        ReadPortraitKey((const uint8_t*)portrait, key, sizeof key);
        QueuePortraitEvent(portrait, PortraitEvent::Type::Greeting);
        if (Painter().GreetingReplaced(key)) effect = nullptr;
    }
    g_in_hook.fetch_sub(1);
    return effect;
}

bool ExeMatchesSdk(uintptr_t base) {
    auto dos = (PIMAGE_DOS_HEADER)base;
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    if (stamp != sdk::kExeTimestamp) {
        Log("exe TimeDateStamp 0x%08X does not match the SDK (0x%08X); not installing. Rebuild after running tools/locate.py.",
            stamp, sdk::kExeTimestamp);
        return false;
    }
    return true;
}

} // namespace

bool Install(uintptr_t base) {
    if (!ExeMatchesSdk(base)) return false;
    g_base = base;
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed (%d)", (int)st);
        return false;
    }
    const uintptr_t target = base + sdk::fn::CPortraitObject_UpdatePortrait;
    if (MH_CreateHook((LPVOID)target, (LPVOID)&UpdatePortraitDetour, (LPVOID*)&g_orig_update) != MH_OK ||
        MH_EnableHook((LPVOID)target) != MH_OK) {
        Log("could not hook CPortraitObject::UpdatePortrait at 0x%llX", (unsigned long long)target);
        return false;
    }
    Log("hooked CPortraitObject::UpdatePortrait at 0x%llX", (unsigned long long)target);
    const uintptr_t render = base + sdk::fn::CPortraitObject_Render;
    if (MH_CreateHook((LPVOID)render, (LPVOID)&RenderDetour, (LPVOID*)&g_orig_render) != MH_OK || MH_EnableHook((LPVOID)render) != MH_OK)
        Log("could not hook CPortraitObject::Render at 0x%llX: portraits will not know where they are on the screen", (unsigned long long)render);
    else
        Log("hooked CPortraitObject::Render at 0x%llX", (unsigned long long)render);
    const uintptr_t greeting = base + sdk::fn::CPortraitObject_GetGreetingSoundEffect;
    if (MH_CreateHook((LPVOID)greeting, (LPVOID)&GreetingDetour, (LPVOID*)&g_orig_greeting) != MH_OK || MH_EnableHook((LPVOID)greeting) != MH_OK)
        Log("could not hook CPortraitObject::GetGreetingSoundEffect at 0x%llX: no greeting event", (unsigned long long)greeting);
    else
        Log("hooked CPortraitObject::GetGreetingSoundEffect at 0x%llX", (unsigned long long)greeting);
    g_installed = true;
    return true;
}

// The volume of the game's own settings for one channel, 0..1: master * dev_master * the channel's slider, each 0..100 in the settings
// object; negative when it cannot be read.
float GameVolume(const std::string& channel) {
    __try {
        const auto* settings = *(const uint8_t* const*)(g_base + sdk::glob::CSettings);
        if (!settings) return -1.0f;
        auto field = [&](std::ptrdiff_t off) { return *(const float*)(settings + off); };
        const float master = field(sdk::rt_settings::master), dev = field(sdk::rt_settings::dev_master);
        const float slider = channel == "effects" ? field(sdk::rt_settings::sfx) : field(sdk::rt_settings::voice);
        for (float v : { master, dev, slider })
            if (!(v >= 0.0f && v <= 200.0f)) return -1.0f;
        return master / 100.0f * (dev / 100.0f) * (slider / 100.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

// The voice volume: the plugin's own `volume` times the game's (so the sliders of the game's settings move it), and silent while the game is not
// the foreground window when the ini says so. Cheap; called every couple of seconds.
void UpdateVoiceVolume(const Settings& s) {
    float volume = s.volume;
    if (s.volume_channel != "none") {
        const float game = GameVolume(s.volume_channel);
        if (game >= 0.0f) volume *= game;
    }
    if (s.mute_in_background && !GameWindowInFront()) volume = 0.0f;
    Voice::Get().SetVolume(volume);
}

bool Uninstall() {
    if (!g_installed) { g_mode = 0; Painter().Shutdown(); return ReleaseInput(); }
    StopPainting();
    Sleep(300);  // a few frames for the engine to re-render the portraits it was flagged to
    const bool input_released = ReleaseInput();  // the window subclass: its code is in this DLL
    MH_DisableHook(MH_ALL_HOOKS);
    // a render thread may still be inside the detour or its trampoline: wait until it has left, then a moment more
    for (int i = 0; i < 1000 && g_in_hook.load() != 0; ++i) Sleep(10);
    Sleep(300);
    MH_Uninitialize();
    g_installed = false;
    Painter().Shutdown();
    return input_released;
}

void Apply(const Settings& s, const Registry& registry) {
    g_only_w = s.only_width;
    g_only_h = s.only_height;
    if (!g_installed) return;
    Painter().Configure(s, registry);  // loads or unloads the models on this (the worker) thread
    const int want = (s.live2d && Painter().Ready()) ? 2 : s.test_pattern ? 1 : 0;
    if (want) g_mode = want;
    else StopPainting();
}

std::string StatsLine() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double painted = (double)g_painted.load();
    const double us = 1e6 / (double)f.QuadPart;
    char buf[600];
    snprintf(buf, sizeof buf,
             "mode=%d | UpdatePortrait calls %llu (engine re-rendered %llu), painted %llu (avg fill %.0f us, upload %.0f us), "
             "no render target %llu, size-filtered %llu, no texture %llu, bad format %llu, faults %llu, live2d failed %llu, unregistered %llu | texture offset %d",
             g_mode.load(), (unsigned long long)g_calls.load(), (unsigned long long)g_engine_renders.load(),
             (unsigned long long)g_painted.load(),
             painted ? g_fill_ticks.load() * us / painted : 0.0, painted ? g_upload_ticks.load() * us / painted : 0.0,
             (unsigned long long)g_no_rt.load(), (unsigned long long)g_filtered.load(), (unsigned long long)g_no_texture.load(),
             (unsigned long long)g_bad_format.load(), (unsigned long long)g_faults.load(), (unsigned long long)g_l2d_failed.load(),
             (unsigned long long)g_unregistered.load(),
             g_tex_off.load());
    std::string line = buf;
    if (g_mode.load() == 2) line += " | " + Painter().Stats();
    return line;
}

} // namespace l2d
