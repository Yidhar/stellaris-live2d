#include "portrait_input.hpp"
#include "live2d.hpp"

#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <utility>
#include <unordered_map>

namespace l2d {

namespace {

constexpr UINT_PTR kSubclassId = 0x4C325C1;
constexpr UINT kRemoveMessage = WM_APP + 0x4C3;
constexpr ULONGLONG kFresh = 300;   // ms a portrait's rectangle stays good without a new draw
constexpr ULONGLONG kEventAge = 500; // ms an event waits for its portrait

HWND g_window = nullptr;
ULONGLONG g_window_checked = 0;
std::atomic<HWND> g_subclassed{ nullptr };
PortraitFrame g_frame;  // set by the hook just before the painter asks (the window thread: the same one that gets the messages)

struct Seen {
    ScreenRect rect;
    float gui_w = 0, gui_h = 0;
    ULONGLONG tick = 0;
};
std::unordered_map<const void*, Seen> g_rects;  // every portrait drawn lately
struct Queued {
    const void* portrait;
    PortraitEvent event;
    ULONGLONG tick;
};
std::deque<Queued> g_events;
const void* g_hovered = nullptr;

BOOL CALLBACK Pick(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    // the game's window is the one titled "Stellaris", whatever its size (it is 0x0 while minimized); failing that the largest
    wchar_t title[64] = {};
    GetWindowTextW(hwnd, title, 64);
    RECT r = {};
    GetWindowRect(hwnd, &r);
    const long long area = (long long)(r.right - r.left) * (r.bottom - r.top) + (wcscmp(title, L"Stellaris") == 0 ? (1ll << 40) : 0);
    auto* best = (std::pair<HWND, long long>*)param;
    if (!best->first || area > best->second) *best = { hwnd, area };
    return TRUE;
}

// the game's main window: the largest visible top-level window of this process; looked up again when it goes away
HWND GameWindow() {
    const ULONGLONG now = GetTickCount64();
    if (g_window && IsWindow(g_window)) return g_window;
    if (now - g_window_checked < 1000) return nullptr;
    g_window_checked = now;
    std::pair<HWND, long long> best{ nullptr, 0 };
    EnumWindows(Pick, (LPARAM)&best);
    g_window = best.first;
    return g_window;
}

// the pointer in client pixels and the client size; false while the game window is not in front
bool MouseInGameWindow(float* x, float* y, float* width, float* height) {
    HWND hwnd = GameWindow();
    if (!hwnd || GetForegroundWindow() != hwnd) return false;
    POINT p;
    RECT r;
    if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &r) || r.right <= 0 || r.bottom <= 0) return false;
    *x = (float)p.x;
    *y = (float)p.y;
    *width = (float)r.right;
    *height = (float)r.bottom;
    return true;
}

// The portrait whose picture holds the client pixel (px, py): among the portraits drawn lately whose rectangle (turned from GUI units
// into client pixels) and clip area contain it, the one whose centre is nearest. u, v: where in that rectangle, 0..1.
const void* PortraitAt(HWND hwnd, int px, int py, float* u, float* v) {
    if (g_rects.empty()) return nullptr;
    RECT client;
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return nullptr;
    const ULONGLONG now = GetTickCount64();
    const void* best = nullptr;
    float best_distance = 1e30f;
    for (auto it = g_rects.begin(); it != g_rects.end();) {
        const void* portrait = it->first;
        const Seen& s = it->second;
        if (now - s.tick > 4000) {  // long gone
            it = g_rects.erase(it);
            continue;
        }
        ++it;
        if (now - s.tick > kFresh || s.gui_w <= 0 || s.gui_h <= 0) continue;
        const float sx = (float)client.right / s.gui_w, sy = (float)client.bottom / s.gui_h;  // GUI units -> client pixels
        const float x0 = s.rect.x * sx, y0 = s.rect.y * sy, w = s.rect.w * sx, h = s.rect.h * sy;
        if (px < x0 || px > x0 + w || py < y0 || py > y0 + h) continue;
        if (s.rect.has_clip && (px < s.rect.clip_x0 || px >= s.rect.clip_x1 || py < s.rect.clip_y0 || py >= s.rect.clip_y1)) continue;
        const float dx = px - (x0 + w * 0.5f), dy = py - (y0 + h * 0.5f);
        const float d = dx * dx + dy * dy;
        if (d < best_distance) {
            best_distance = d;
            best = portrait;
            *u = w > 0 ? (px - x0) / w : 0.5f;
            *v = h > 0 ? (py - y0) / h : 0.5f;
        }
    }
    return best;
}

void Queue(const void* portrait, PortraitEvent::Type type, float u, float v) {
    g_events.push_back({ portrait, { type, u, v }, GetTickCount64() });
    while (g_events.size() > 32) g_events.pop_front();
}

// The window's own thread, for every message the game window gets. Cheap when the pointer is not over a portrait: one pass over a
// handful of rectangles, and nothing is changed or swallowed.
LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE: {
        float u = 0, v = 0;
        const void* portrait = PortraitAt(hwnd, (short)LOWORD(lp), (short)HIWORD(lp), &u, &v);
        if (msg == WM_LBUTTONDOWN) {
            if (portrait) Queue(portrait, PortraitEvent::Type::Click, u, v);
        } else if (portrait != g_hovered) {
            g_hovered = portrait;
            if (portrait) Queue(portrait, PortraitEvent::Type::Hover, u, v);
        }
        break;
    }
    case kRemoveMessage:
        RemoveWindowSubclass(hwnd, InputProc, id);
        g_subclassed = nullptr;
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, InputProc, id);
        g_subclassed = nullptr;
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// on the window's thread only (the portrait hook is on it): once per window
void EnsureSubclass() {
    static ULONGLONG last_try = 0;
    HWND hwnd = GameWindow();
    if (!hwnd || hwnd == g_subclassed.load()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - last_try < 5000) return;  // not on every frame while it keeps failing
    last_try = now;
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) {
        Log("input: the game window %p belongs to thread %lu, the portrait hook runs on thread %lu: mouse events are not available", (void*)hwnd,
            GetWindowThreadProcessId(hwnd, nullptr), GetCurrentThreadId());
        return;
    }
    if (SetWindowSubclass(hwnd, InputProc, kSubclassId, 0)) {
        g_subclassed = hwnd;
        Log("input: watching the mouse messages of the game window %p", (void*)hwnd);
    } else {
        Log("input: could not subclass the game window %p (error %lu): mouse events are not available", (void*)hwnd, GetLastError());
    }
}

} // namespace

void SetPortraitFrame(const PortraitFrame& frame) {
    g_frame = frame;
    EnsureSubclass();
    if (frame.has_rect && frame.portrait) {
        Seen& s = g_rects[frame.portrait];
        s.rect = frame.rect;
        s.gui_w = frame.gui_w;
        s.gui_h = frame.gui_h;
        s.tick = GetTickCount64();
    }
}

bool ReleaseInput() {
    HWND hwnd = g_subclassed.load();
    if (!hwnd || !IsWindow(hwnd)) return true;
    DWORD_PTR result = 0;
    // the window's own thread runs InputProc for this message and takes the subclass off; it returns once that happened
    if (!SendMessageTimeoutW(hwnd, kRemoveMessage, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result)) {
        Log("input: the game window did not answer; the plugin stays loaded");
        return false;
    }
    return g_subclassed.load() == nullptr;
}

bool PortraitMirrored(const void* portrait) { return g_frame.portrait == portrait && g_frame.has_rect && g_frame.rect.mirrored; }

bool PopPortraitEvent(const void* portrait, PortraitEvent* out) {
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_events.begin(); it != g_events.end();) {
        if (now - it->tick > kEventAge) { it = g_events.erase(it); continue; }
        if (it->portrait == portrait) {
            *out = it->event;
            g_events.erase(it);
            return true;
        }
        ++it;
    }
    return false;
}

bool ComputeLookTarget(const void* portrait, float* x, float* y) {
    *x = *y = 0.0f;
    float mx, my, ww, wh;
    if (!MouseInGameWindow(&mx, &my, &ww, &wh)) return false;
    const bool mine = g_frame.portrait == portrait;
    if (mine && g_frame.has_rect && g_frame.has_mouse) {
        const ScreenRect& r = g_frame.rect;
        const float gw = g_frame.gui_w, gh = g_frame.gui_h;
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        *x = std::clamp((g_frame.mouse_x - cx) / (0.4f * gw), -1.0f, 1.0f);
        *y = std::clamp((cy - g_frame.mouse_y) / (0.4f * gh), -1.0f, 1.0f);
        if (r.mirrored) *x = -*x;
        return true;
    }
    // the portrait's place is not known: the Cubism samples' choice, the pointer relative to the middle of the window
    *x = std::clamp((mx - ww * 0.5f) / (ww * 0.5f), -1.0f, 1.0f);
    *y = std::clamp((wh * 0.5f - my) / (wh * 0.5f), -1.0f, 1.0f);
    return true;
}

} // namespace l2d
