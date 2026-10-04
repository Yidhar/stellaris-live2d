"""Live check of the portrait interactions in a running game with the plugin loaded and a screen with portraits open (the council,
the leaders list). Waits until the user has been idle for a while, brings the game to the front, does the actions and saves
screenshots. It moves the mouse and, for `click`, presses the left button: pick a spot where that is harmless.

    python test_interactions.py shot  <name>                     screenshot of the game
    python test_interactions.py follow <name> x,y [x,y ...]      park the pointer at each client-pixel spot, screenshot each
    python test_interactions.py click <name> x,y [--frames N]    click at the spot; screenshots right before and N times after (0.25 s apart)
    python test_interactions.py council <name>                   open the council with F2 (toggles: it must be closed), screenshot it,
                                                                 park the pointer far left and far right, click the first portrait

The council check needs the stellaris-perf helper scripts (game_session.press sends the key); the first council slot of the test save
is at client pixels (315..642, 130..346).

Screenshots go to captures/<name>_*.png. The plugin's log (stellaris_live2d.log, next to stellaris.exe) says what a click triggered.
"""
import argparse
import ctypes
import ctypes.wintypes as w
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_game as cg  # noqa: E402
import desktop_guard as dg  # noqa: E402

user32 = ctypes.WinDLL("user32")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def prepare(idle_seconds, timeout):
    hwnd = cg.game_hwnd()
    if not hwnd:
        raise SystemExit("no Stellaris window")
    print(f"waiting for {idle_seconds:.0f} s without keyboard or mouse input (now {dg.idle_seconds():.0f} s)...", flush=True)
    if not dg.wait_until_idle(idle_seconds, timeout):
        raise SystemExit("the user did not become idle; nothing was done")
    if not dg.bring_to_front(hwnd):
        raise SystemExit("could not bring the game to the front")
    return hwnd


def client_origin(hwnd):
    pt = w.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def move(hwnd, x, y):
    ox, oy = client_origin(hwnd)
    for dx in (-3, 3, 0):  # a wiggle so the engine sees mouse-move messages
        user32.SetCursorPos(ox + x + dx, oy + y)
        time.sleep(0.05)


def shot(path):
    cg.capture(path)


def spot(text):
    x, y = text.split(",")
    return int(x), int(y)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["shot", "follow", "click", "council"])
    ap.add_argument("name")
    ap.add_argument("spots", nargs="*")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--idle", type=float, default=20.0)
    ap.add_argument("--timeout", type=float, default=540.0)
    a = ap.parse_args()
    out = lambda tag: os.path.join(ROOT, "captures", f"{a.name}_{tag}.png")
    hwnd = prepare(a.idle, a.timeout)
    if a.action == "council":
        sys.path.insert(0, r"D:\stellaris-perf\bench\scripts")
        import game_session as gs
        gs.VK["F2"] = 0x71
        gs.press(gs.game_pids()[0], "F2")
        time.sleep(3.0)
        shot(out("council_open"))
        move(hwnd, 60, 300)
        time.sleep(1.8)
        shot(out("council_left"))
        move(hwnd, 1850, 300)
        time.sleep(1.8)
        shot(out("council_right"))
        move(hwnd, 480, 250)
        time.sleep(1.0)
        shot(out("council_click_before"))
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.08)
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        for i in range(6):
            time.sleep(0.25)
            shot(out(f"council_click_after{i}"))
        move(hwnd, 960, 800)
        return
    if a.action == "shot":
        shot(out("shot"))
    elif a.action == "follow":
        for i, s in enumerate(a.spots):
            move(hwnd, *spot(s))
            time.sleep(1.8)  # the look eases in over a fraction of a second, a step is 1/30 s
            shot(out(f"follow{i}"))
    else:
        x, y = spot(a.spots[0])
        move(hwnd, x, y)
        time.sleep(1.0)
        shot(out("click_before"))
        user32.mouse_event(0x0002, 0, 0, 0, 0)  # left down
        time.sleep(0.08)
        user32.mouse_event(0x0004, 0, 0, 0, 0)  # left up
        for i in range(a.frames):
            time.sleep(0.25)
            shot(out(f"click_after{i}"))


if __name__ == "__main__":
    main()
