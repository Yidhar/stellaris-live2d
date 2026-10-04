#include "portrait_input.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

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

// the last left button press seen: when, where (GUI units and client pixels), and a counter of presses
uint64_t g_press_serial = 0;
ULONGLONG g_press_time = 0;
float g_press_gui_x = 0, g_press_gui_y = 0;
int g_press_px = 0, g_press_py = 0;
bool g_button_was_down = false;
std::unordered_map<const void*, uint64_t> g_seen_press;  // per portrait: the press it has looked at

void PollButton() {
    bool down = false;
    float mx, my, ww, wh;
    if (MouseInGameWindow(&mx, &my, &ww, &wh)) down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !g_button_was_down && g_frame.has_mouse) {
        ++g_press_serial;
        g_press_time = GetTickCount64();
        g_press_gui_x = g_frame.mouse_x;
        g_press_gui_y = g_frame.mouse_y;
        g_press_px = (int)mx;
        g_press_py = (int)my;
    }
    g_button_was_down = down;
}

void SetPortraitFrame(const PortraitFrame& frame) {
    g_frame = frame;
    PollButton();
}

bool PortraitMirrored(const void* portrait) { return g_frame.portrait == portrait && g_frame.has_rect && g_frame.rect.mirrored; }

bool ConsumeClick(const void* portrait) {
    uint64_t& seen = g_seen_press[portrait];
    if (seen == g_press_serial) return false;
    seen = g_press_serial;
    if (g_frame.portrait != portrait || !g_frame.has_rect || GetTickCount64() - g_press_time > 300) return false;
    const ScreenRect& r = g_frame.rect;
    if (g_press_gui_x < r.x || g_press_gui_x > r.x + r.w || g_press_gui_y < r.y || g_press_gui_y > r.y + r.h) return false;
    if (r.has_clip && (g_press_px < r.clip_x0 || g_press_px >= r.clip_x1 || g_press_py < r.clip_y0 || g_press_py >= r.clip_y1)) return false;
    return true;
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
