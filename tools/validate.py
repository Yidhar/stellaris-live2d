"""Regression check of the SDK against what was verified by hand, like the main repo's tools/sdk_dumper/validate.py. Run after every game patch,
right after tools/locate.py:

    python tools/validate.py

1. the locator finds, in the installed exe, what the committed sdk/stellaris_sdk.hpp says (locate.py --check);
2. every value that was checked against the running game or the disassembly (GROUND_TRUTH below) is still what the header says, and the
   structural invariants of the layout hold (they also hold on a build that has not been verified by hand);
3. says whether the layout constants that no fingerprint finds were verified for this exe (kExeTimestamp / LAYOUT_VERIFIED_FOR).

The values the exe cannot confirm statically (live objects: the pointer, the rectangle, the settings) are checked by the plugin itself once a
portrait is on screen: tools/live_verify.py reads that self-test from its log.
Exit code 1 if anything is off.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "sdk" / "stellaris_sdk.hpp"
LAYOUT_VERIFIED_FOR = 0x6ABEAA3F  # Stellaris 4.5.2; 4.5.1 was 0x6AB5181D

# name in the header -> the value verified for the exe above (live in the game, or in the disassembly); only compared for that exe
GROUND_TRUTH = {
    "CPortraitObject_UpdatePortrait": 0xFB19C0,
    "CPortraitObject_Render": 0xFADCF0,
    "CPortraitObject_GetGreetingSoundEffect": 0xFB4420,
    "CPortraitObjectController_PortraitObjects_data": 0x28808B8,
    "CPortraitObject_width": 0x528,
    "CPortraitObject_height": 0x52A,
    "CPortraitObject_needs_render": 0x52C,
    "CPortraitObject_render_target": 0x530,
    "CPortraitObject_key": 0x818,
    "CPortraitObject_kind": 0xA48,
    "CPortraitObject_scope_type": 0x248,
    "CPortraitObject_scope_id": 0x250,
    "CPortraitObject_group": 0x878,
    "CPortraitObject_pos": 0x68,
    "CPortraitObject_scale": 0xC4,
    "CPortraitObject_mirrored": 0xC8,
    "CPortraitObject_scissor": 0xA8,
    "CPortraitObject_has_scissor": 0xBE,
    "CGuiGraphics_mouse_x": 0x350,
    "CGuiGraphics_mouse_y": 0x354,
    "CGuiGraphics_width": 0x28,
    "CGuiGraphics_height": 0x2C,
    "CGuiGraphics_gui_width": 0x30,
    "CGuiGraphics_gui_height": 0x34,
    "CSettings": 0x322EE90,
    "master": 0x174,
    "dev_master": 0x178,
    "music": 0x180,
    "sfx": 0x17C,
    "ambient": 0x184,
    "voice": 0xA5C,
    "tts": 0xA60,
}


def values():
    text = HEADER.read_text(encoding="utf-8")
    found = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"constexpr \w+(?:::\w+)? (\w+) = (0x[0-9a-fA-F]+)", text)}
    return text, found


def main():
    bad = 0
    check = subprocess.run([sys.executable, str(ROOT / "tools" / "locate.py"), "--check"], capture_output=True, text=True)
    print(check.stdout.strip() or check.stderr.strip())
    if check.returncode:
        bad += 1
    text, v = values()
    stamp = v.get("kExeTimestamp")
    print(f"header is for exe timestamp {stamp:#010x}" if stamp is not None else "no exe timestamp in the header")
    verified = stamp == LAYOUT_VERIFIED_FOR
    if verified:
        for name, expected in GROUND_TRUTH.items():
            got = v.get(name)
            if got != expected:
                print(f"MISMATCH  {name}: header {got if got is None else hex(got)}, verified {expected:#x}")
                bad += 1
        print(f"{len(GROUND_TRUTH)} verified values compared")
    else:
        print(f"NOTE: the layout constants were verified for exe {LAYOUT_VERIFIED_FOR:#010x}; this exe needs them checked again "
              "(docs/engine-notes.md says how each was found), then LAYOUT_VERIFIED_FOR and GROUND_TRUTH updated")
    # structural invariants that must hold whatever the build
    invariants = [
        ("height follows width", v.get("CPortraitObject_height") == v.get("CPortraitObject_width", -9) + 2),
        ("the render target sits after the flag", v.get("CPortraitObject_render_target", 0) > v.get("CPortraitObject_needs_render", 1)),
        ("settings fields are four bytes apart", v.get("dev_master") == v.get("master", -9) + 4),
        ("the GUI size follows the window size", v.get("CGuiGraphics_gui_width") == v.get("CGuiGraphics_width", -9) + 8),
        ("the pointer is two floats", v.get("CGuiGraphics_mouse_y") == v.get("CGuiGraphics_mouse_x", -9) + 4),
    ]
    for name, ok in invariants:
        if not ok:
            print("INVARIANT BROKEN:", name)
            bad += 1
    print("validate:", "FAILED" if bad else "ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
