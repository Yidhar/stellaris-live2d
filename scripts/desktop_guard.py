"""Helpers for scripts that drive the game window on a desktop somebody may be using: wait until the user has been idle for a
while before taking the foreground or moving the mouse, and only take screenshots of the game while it is in front (a grab of the
screen region where the game should be shows whatever covers it)."""
import ctypes
import ctypes.wintypes as w
import time

user32 = ctypes.WinDLL("user32")
kernel32 = ctypes.WinDLL("kernel32")


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", w.UINT), ("dwTime", w.DWORD)]


def idle_seconds():
    info = LASTINPUTINFO(cbSize=ctypes.sizeof(LASTINPUTINFO))
    user32.GetLastInputInfo(ctypes.byref(info))
    return (kernel32.GetTickCount() - info.dwTime) / 1000.0


def wait_until_idle(seconds=15.0, timeout=900.0):
    """Blocks until there has been no keyboard or mouse input for `seconds`; False if that never happened within `timeout`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if idle_seconds() >= seconds:
            return True
        time.sleep(1.0)
    return False


def bring_to_front(hwnd):
    user32.keybd_event(0x12, 0, 0, 0)  # an Alt tap lets this process take the foreground
    user32.keybd_event(0x12, 0, 2, 0)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    return user32.GetForegroundWindow() == hwnd


def is_foreground(hwnd):
    return user32.GetForegroundWindow() == hwnd
