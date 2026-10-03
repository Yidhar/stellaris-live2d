"""Saves a PNG of the game window's client area (the window must be visible and not covered). Usage: python capture_game.py out.png"""
import ctypes, ctypes.wintypes as w, sys
from PIL import ImageGrab

user32 = ctypes.WinDLL("user32")
# without this, window coordinates are scaled by the display scaling (150% on a 4K screen) while the grab is in real pixels
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except OSError:
    user32.SetProcessDPIAware()


def game_hwnd():
    found = []
    pid_of = ctypes.c_ulong()

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid_of))
        if user32.IsWindowVisible(hwnd) and buf.value.startswith("Stellaris"):
            found.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def capture(out):
    hwnd = game_hwnd()
    if not hwnd:
        raise SystemExit("no visible window titled Stellaris")
    pt = w.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    c = w.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(c))
    vx, vy = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)  # virtual screen origin (negative on a left monitor)
    box = (pt.x - vx, pt.y - vy, pt.x - vx + c.right, pt.y - vy + c.bottom)
    im = ImageGrab.grab(all_screens=True).crop(box)
    im.save(out)
    print(f"client {c.right}x{c.bottom} at screen ({pt.x},{pt.y}), virtual origin ({vx},{vy}) -> {out}")


if __name__ == "__main__":
    capture(sys.argv[1] if len(sys.argv) > 1 else "captures/game.png")
