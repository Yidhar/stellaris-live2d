#include "portrait_input.hpp"

#include <windows.h>

namespace l2d {

namespace {

HWND g_window = nullptr;
ULONGLONG g_window_checked = 0;

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

} // namespace

bool PortraitScreenRect(const void*, ScreenRect*) { return false; }

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

} // namespace l2d
