"""Reads the plugin's self-test from its log: the live half of the SDK check (tools/validate.py is the static half). The plugin runs the
checks itself once the first portrait is on screen and writes `selftest:` lines (portrait key and kind, rectangle, window and GUI sizes, the
game's volumes, and the pointer once the game window is in front with the pointer inside it).

    python tools/live_verify.py [--log <stellaris_live2d.log>] [--wait SECONDS]

Needs a running game with the plugin loaded and a screen with portraits open. Exit code 1 if a check failed or none ran.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
from plugin_paths import LOG  # noqa: E402


def last_block(lines):
    """The selftest lines of the most recent run (a run starts at `selftest: key`)."""
    start = None
    for i, line in enumerate(lines):
        if "selftest: key" in line:
            start = i
    return [l for l in lines[start:] if "selftest:" in l] if start is not None else []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", default=LOG)
    ap.add_argument("--wait", type=float, default=0.0, help="wait this long for the pointer check")
    a = ap.parse_args()
    deadline = time.time() + a.wait
    while True:
        with open(a.log, encoding="utf-8", errors="replace") as f:
            block = last_block(f.read().splitlines())
        if any("pointer" in l for l in block) or time.time() >= deadline:
            break
        time.sleep(1.0)
    if not block:
        print("no self-test in the log: the plugin must be loaded and a portrait on screen")
        return 1
    for line in block:
        print(line.split("] ", 1)[-1])
    failed = [l for l in block if " FAIL " in l]
    if not any("pointer" in l for l in block):
        print("note: the pointer check has not run (the game window must be in front with the pointer inside it)")
    print("live_verify:", "FAILED" if failed else "ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
