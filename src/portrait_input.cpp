#include "portrait_input.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace l2d {

namespace {

HWND g_window = nullptr;
ULONGLONG g_window_checked = 0;
PortraitFrame g_frame;          // render thread only: set by the hook just before the painter asks

BOOL CALLBACK Pick(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetClientRect(hwnd, &r) || r.right < 320 || r.bottom < 240) return TRUE;
    auto* best = (HWND*)param;
    RECT b = {};
    if (*best) GetClientRect(*best, &b);
    if (!*best || (r.right * r.bottom > b.right * b.bottom)) *best = hwnd;
    return TRUE;
}

// the game's main window: the largest visible top-level window of this process; looked up again when it goes away
HWND GameWindow() {
    const ULONGLONG now = GetTickCount64();
    if (g_window && IsWindow(g_window)) return g_window;
    if (now - g_window_checked < 1000) return nullptr;
    g_window_checked = now;
    HWND best = nullptr;
    EnumWindows(Pick, (LPARAM)&best);
    g_window = best;
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

} // namespace

void SetPortraitFrame(const PortraitFrame& frame) { g_frame = frame; }

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
