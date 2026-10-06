"""Locates what the plugin needs inside stellaris.exe and writes sdk/stellaris_sdk.hpp. No address is typed in by hand:
every one is found by a fingerprint taken from the engine's own code, and the run fails instead of guessing when a
fingerprint is not unique. Run it after every game patch:

    python tools/locate.py            # reads STELLARIS_DIR (or the default Steam folder), writes sdk/stellaris_sdk.hpp
    python tools/locate.py --check    # does not write: exit 1 when the committed header is not what the locator finds

What it finds (all for CPortraitObject, the engine's portrait):
  * UpdatePortrait(this, graphics, ctx): the only function that mentions the source location string
    ".../graphics/portraitobject.cpp:467", which it passes when it creates the portrait's render target
  * from that same code: where the object keeps its render-target width, height, texture pointer and "needs render" flag
  * CPortraitObjectController::UpdatePortraits, the only direct caller of UpdatePortrait, and from its loop the global
    array of every portrait object the engine has (data pointer and count)
  * which portrait an object is: every frame UpdatePortrait looks the object's string member (the key of the `portraits = {}`
    entry the engine resolved for it, e.g. "human_male_01") up in the portrait database table, with
    `lea r8, [this + KEY]` / `lea rcx, [database + TABLE]` / `call Find`
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import Image  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# Layout of the engine's 2D GUI object that a CPortraitObject is (CPortraitObject : CSprite : C2dObject : CGraphicalObject) and
# of the CGuiGraphics the engine passes to UpdatePortrait. Read from the disassembly of the constructors, of
# CGuiGraphics::Render2dTree (it stores the absolute position at +0x68 and reads the pointer from the graphics object) and of
# CPortraitObject::Render (see docs/engine-notes.md); verified for this build only. The plugin checks every value it reads
# for plausibility and falls back to the middle of the window when a value is nonsense.
LAYOUT_VERIFIED_FOR = 0x6ABEAA3F
LITERAL = r"C:\mnt\gsg\stellaris\augustus\augustus\source\graphics\portraitobject.cpp:467"
RENDER_STRING = "Invalid alternate sprite configuration index [%i], must be in range [%i, %i)"


def all_strings(im, text):
    """RVAs of every NUL-terminated copy of a string literal."""
    needle = b"\0" + text.encode() + b"\0"
    pos = 0
    while True:
        i = im.img.find(needle, pos)
        if i < 0:
            return
        yield i + 1
        pos = i + 1


def functions_mentioning(im, text):
    out = set()
    for rva in all_strings(im, text):
        out |= set(im.functions_referencing(rva))
    return out


def fail(msg):
    print("FAILED:", msg, file=sys.stderr)
    sys.exit(1)


def mem_disp(op_str, reg=None):
    """Displacement of a `[reg + 0xN]` memory operand (reg=None: any base register), or None."""
    m = re.search(r"\[(\w+)(?: \+ (0x[0-9a-f]+))?\]", op_str)
    if not m or (reg and m.group(1) != reg):
        return None
    return int(m.group(2), 16) if m.group(2) else 0


def main():
    im = Image()
    lit = im.find_string(LITERAL)
    if lit is None:
        fail("the portraitobject.cpp:467 string is not in this executable")
    funcs = im.functions_referencing(lit)
    if len(funcs) != 1:
        fail(f"expected exactly one function referencing the literal, found {[hex(f) for f in funcs]}")
    fn = funcs[0]
    ins = im.disasm_fn(fn, 0x8000)
    rip_calls = [i for i in ins if i.mnemonic == "call" and i.op_str.startswith("qword ptr [rip")]
    if len(ins) < 800 or len(rip_calls) < 5:
        fail(f"{fn:#x} does not look like UpdatePortrait ({len(ins)} instructions, {len(rip_calls)} callback calls)")

    # `this` is the first argument: `mov rsi, rcx` right at the top
    this_reg = next((i.op_str.split(",")[0].strip() for i in ins[:25] if i.mnemonic == "mov" and i.op_str.endswith(", rcx")
                     and i.op_str.split(",")[0].strip() in ("rsi", "rdi", "rbx", "r12", "r13", "r14", "r15", "rbp")), None)
    if not this_reg:
        fail("could not find where UpdatePortrait keeps `this`")

    pos = next(k for k, x in enumerate(ins) if im.rip_target(x) == lit)
    # The create-render-target call: GfxCreateRenderTargetTexture(device, width, height, format, ...). Just before the
    # literal is loaded: `movzx r8d, word ptr [this + H]` (height), `lea rX, [this + W]`, `movzx edx, word ptr [rX]` (width).
    # After the call the result is stored: `mov qword ptr [this + RT], rax`.
    height = width = rt = None
    width_reg = None
    for x in ins[pos - 14:pos]:
        if x.mnemonic == "movzx" and x.op_str.startswith("r8d, word ptr"):
            height = mem_disp(x.op_str, this_reg)
        if x.mnemonic == "lea" and mem_disp(x.op_str, this_reg) is not None and not x.op_str.startswith("rax"):
            width, width_reg = mem_disp(x.op_str, this_reg), x.op_str.split(",")[0].strip()
        if x.mnemonic == "movzx" and x.op_str.startswith("edx, word ptr") and mem_disp(x.op_str, this_reg) is not None:
            width = mem_disp(x.op_str, this_reg)
    for x in ins[pos:pos + 14]:
        if x.mnemonic == "mov" and x.op_str.startswith("qword ptr [") and x.op_str.endswith(", rax"):
            d = mem_disp(x.op_str, this_reg)
            if d is not None:
                rt = d
                break
    if None in (width, height, rt):
        fail(f"could not read the render-target fields: width={width} height={height} rt={rt}")
    if height != width + 2 or rt < width + 8:
        fail(f"unexpected field layout: width {width:#x}, height {height:#x}, render target {rt:#x}")

    # The create call must be guarded by `cmp qword ptr [this + RT], 0` (only created when missing)
    if not any(x.mnemonic == "cmp" and mem_disp(x.op_str, this_reg) == rt and x.op_str.endswith(", 0") for x in ins[:pos]):
        fail("no `render target is null` guard before the create call")

    # `cmp byte ptr [this + D], 0` after the create block: the portrait is only re-rendered while this is set,
    # and the field sits between the height and the texture pointer
    dirty = next((mem_disp(x.op_str, this_reg) for x in ins[pos:pos + 40]
                  if x.mnemonic == "cmp" and x.op_str.startswith("byte ptr [") and x.op_str.endswith(", 0")
                  and mem_disp(x.op_str, this_reg) is not None and height < mem_disp(x.op_str, this_reg) < rt), None)
    if dirty is None:
        fail("could not find the portrait's `needs render` flag")

    # CPortraitObject::Render(this, CGuiGraphics*, ctx, matrix, alpha, state, texture): the GUI draw of the portrait, the only
    # function that mentions the alternate-sprite-configuration range error. It sets the same "needs render" flag.
    rs = im.find_string(RENDER_STRING)
    if rs is None:
        fail("the alternate sprite configuration string is not in this executable")
    rfuncs = im.functions_referencing(rs)
    if len(rfuncs) != 1:
        fail(f"expected exactly one function referencing the alternate sprite configuration string, found {[hex(f) for f in rfuncs]}")
    render = rfuncs[0]
    rins = im.disasm_fn(render, 0x1000)
    if not any(x.mnemonic == "mov" and x.op_str.startswith("byte ptr [") and x.op_str.endswith(", 1") and mem_disp(x.op_str) == dirty for x in rins):
        fail(f"{render:#x} does not set the needs-render flag at +{dirty:#x}")

    # The portrait key: the two sites that look it up in the database table agree on (KEY, TABLE) and the same Find function
    sites = set()
    for k in range(4, len(ins)):
        if ins[k].mnemonic != "call" or not ins[k].op_str.startswith("0x"):
            continue
        leas = {x.op_str.split(",")[0].strip(): x for x in ins[k - 4:k] if x.mnemonic == "lea"}
        if all(r in leas for r in ("r8", "rdx", "rcx")):
            key_off, table_off = mem_disp(leas["r8"].op_str), mem_disp(leas["rcx"].op_str)
            if key_off and table_off and key_off > rt and table_off > 0x100:
                sites.add((ins[k].op_str, key_off, table_off))
    if len({(k, t) for _, k, t in sites}) != 1 or len({f for f, _, _ in sites}) != 1:
        fail(f"could not find the portrait key lookup in UpdatePortrait: {sorted(sites)}")
    _, key, table = next(iter(sites))
    if not any(x.mnemonic == "lea" and x.op_str == f"rcx, [{this_reg} + {key:#x}]" for x in ins):
        fail(f"the key at +{key:#x} is not addressed through `this` ({this_reg})")

    # The controller: the one place that calls UpdatePortrait, once per visible portrait out of a global array
    text = im.img[im.text0:im.text1]
    callers = []
    for m in re.finditer(rb"\xE8", text):  # call rel32
        i = m.start()
        if i + 5 <= len(text) and im.text0 + i + 5 + struct.unpack_from("<i", text, i + 1)[0] == fn:
            callers.append(im.text0 + i)
    if len(callers) != 1:
        fail(f"expected exactly one direct caller of UpdatePortrait, found {[hex(c) for c in callers]}")
    ctrl = im.primary(im.fn_of(callers[0]))
    cins = im.disasm_fn(ctrl, 0x1000)
    # `mov rX, [rip + data]` ... `movsxd rY, dword ptr [rip + count]`, then `lea rZ, [rX + rY*8]` (the end pointer)
    data = count = None
    for j in range(3, len(cins) - 1):
        b, nxt = cins[j], cins[j + 1]
        tb = im.rip_target(b)
        if not (b.mnemonic == "movsxd" and tb and nxt.mnemonic == "lea" and "*8]" in nxt.op_str):
            continue
        reg = b.op_str.split(",")[0].strip()  # the count register; the lea must use data register + count register
        for a in cins[j - 3:j]:
            ta = im.rip_target(a)
            if a.mnemonic == "mov" and ta and a.op_str.startswith("r") and a.op_str.split(",")[0].strip() in nxt.op_str:
                data, count = ta, tb
                break
        if data:
            break
    if data is None or count != data + 0xC:
        fail(f"could not read the portrait array in {ctrl:#x}: data {data} count {count}")

    # The game's volume settings: CGameApplication::UpdateAudioVolume, the only function that mentions the four mixer categories, loads the
    # CSettings object and reads the sliders from it (settings.txt: master_volume, dev_master_volume, sound_fx_volume, music_volume,
    # ambient_volume, voice_volume, tts_volume). Each category's value is stored into the category found by its name, which names the field.
    cats = [functions_mentioning(im, n) for n in ("Effects", "Ambient", "Voice", "TTS")]
    audio_fns = set.intersection(*cats)
    if len(audio_fns) != 1:
        fail(f"expected exactly one function that mentions the Effects, Ambient, Voice and TTS categories, found {[hex(f) for f in audio_fns]}")
    audio_fn = next(iter(audio_fns))
    ains = im.disasm_fn(audio_fn, 0x1200)
    settings_global = None
    loads = {}  # register -> field offset, in the order read
    order = []
    for x in ains[:40]:
        t = im.rip_target(x)
        if settings_global is None:
            if x.mnemonic == "mov" and t and x.op_str.startswith("rax,"):
                settings_global = t
            continue
        m = re.match(r"(xmm\d+), dword ptr \[rax \+ (0x[0-9a-f]+)\]", x.op_str) if x.mnemonic == "movss" else None
        if m:
            loads[m.group(1)] = int(m.group(2), 16)
            order.append(int(m.group(2), 16))
    by_category = {}
    stored_to_context = {}
    for k, x in enumerate(ains):
        if x.mnemonic == "lea" and x.op_str.startswith("rdx, [rip"):
            t = im.rip_target(x)
            name = im.cstr(t, 16) if t else ""
            if name in ("Effects", "Ambient", "Voice", "TTS"):
                for y in ains[k + 1:k + 8]:
                    if y.mnemonic == "movss" and y.op_str.startswith("dword ptr [rax + 8], "):
                        by_category[name] = loads.get(y.op_str.split(",")[1].strip())
                        break
        elif x.mnemonic == "movss":
            m = re.match(r"dword ptr \[rcx \+ (0x[0-9a-f]+)\], (xmm\d+)", x.op_str)
            if m:
                stored_to_context[int(m.group(1), 16)] = loads.get(m.group(2))
    music_off = stored_to_context.get(0x900)  # the context's music volume
    if settings_global is None or len(order) < 5 or None in by_category.values() or len(by_category) != 4 or music_off is None or order[1] != order[0] + 4:
        fail(f"could not read the volume fields in {audio_fn:#x}: global {settings_global}, loads {order}, categories {by_category}, music {music_off}")
    vol = {"master": order[0], "dev_master": order[1], "music": music_off, "sfx": by_category["Effects"], "ambient": by_category["Ambient"],
           "voice": by_category["Voice"], "tts": by_category["TTS"]}

    # CPortraitObject::GetGreetingSoundEffect: the function that logs "Missing sound effect: %s", works on the portrait key, and whose
    # result its callers (two or more of them) hand straight to the sound player (`mov rcx, rax`)
    def touches_key(f):
        for x in im.disasm_fn(f, 0x800):
            m = re.search(r"\+ (0x[0-9a-f]+)\]", x.op_str)
            if m and key <= int(m.group(1), 16) < key + 0x30:
                return True
        return False
    greet = []
    for f in sorted(functions_mentioning(im, "Missing sound effect: %s")):
        if not touches_key(f):
            continue
        sites = []
        for mm in re.finditer(rb"\xE8", text):
            i = mm.start()
            if i + 5 <= len(text) and im.text0 + i + 5 + struct.unpack_from("<i", text, i + 1)[0] == f:
                sites.append(im.text0 + i)
        handed = 0
        for site in sites:
            nxt = next((x for x in im.md.disasm(im.img[site + 5:site + 20], im.ib + site + 5)), None)
            if nxt is not None and nxt.mnemonic == "mov" and nxt.op_str == "rcx, rax":
                handed += 1
        if handed >= 2:
            greet.append(f)
    if len(greet) != 1:
        fail(f"expected exactly one greeting sound function, found {[hex(f) for f in greet]}")
    greeting = greet[0]

    if im.timestamp != LAYOUT_VERIFIED_FOR:
        print(f"WARNING: the GUI object layout constants were verified for exe {LAYOUT_VERIFIED_FOR:#010x}, this is {im.timestamp:#010x}; "
              "re-check them against docs/engine-notes.md", file=sys.stderr)
    print(f"exe timestamp {im.timestamp:#010x}")
    print(f"CPortraitObject::UpdatePortrait   rva {fn:#x}  ({len(ins)} instructions, this in {this_reg})")
    print(f"CPortraitObject width             +{width:#x} (uint16)")
    print(f"CPortraitObject height            +{height:#x} (uint16)")
    print(f"CPortraitObject render target     +{rt:#x} (TextureGFX*)")
    print(f"CPortraitObject needs-render flag +{dirty:#x} (uint8)")
    print(f"CPortraitObject key (CString)     +{key:#x}  (database table at +{table:#x})")
    print(f"CPortraitObject::Render           rva {render:#x}")
    print(f"CPortraitObject::GetGreetingSoundEffect rva {greeting:#x}")
    print(f"CSettings global                  rva {settings_global:#x}; volume fields " + ", ".join(f"{k} +{v:#x}" for k, v in vol.items()) + f" (read in {audio_fn:#x})")
    print(f"UpdatePortraits (controller)      rva {ctrl:#x}")
    print(f"portrait array                    data pointer rva {data:#x}, count rva {count:#x}")

    out = ROOT / "sdk" / "stellaris_sdk.hpp"
    out.parent.mkdir(exist_ok=True)
    text = f"""// GENERATED by tools/locate.py from the installed stellaris.exe. DO NOT EDIT.
// Every value is found by a fingerprint taken from the engine's own code (see the script); rerun it after a game patch.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sdk {{
inline constexpr uint32_t kExeTimestamp = {im.timestamp:#010x};  // PE TimeDateStamp this SDK was located in

namespace fn {{
    // void (*)(void* portrait, void* graphics, void* context): renders one visible portrait into its render target
    inline constexpr uintptr_t CPortraitObject_UpdatePortrait = {fn:#x};
    // void (*)(void* self, void* guiGraphics, void* ctx, const float* matrix16, float alpha, uint16_t state, void* texture): the GUI
    // draws the portrait's render target; the matrix holds the absolute position, guiGraphics is the engine's CGuiGraphics
    inline constexpr uintptr_t CPortraitObject_Render = {render:#x};
    // void* (*)(void* portrait): the sound effect of the portrait's greeting (null if there is none); its callers play it
    inline constexpr uintptr_t CPortraitObject_GetGreetingSoundEffect = {greeting:#x};
}}  // namespace fn

namespace glob {{
    // pointer to the engine's settings object (settings.txt as the game uses it right now)
    inline constexpr uintptr_t CSettings = {settings_global:#x};
    // The engine's array of every portrait object (CPdxArray<CPortraitObject*>): RVA of its data pointer and of its int count
    inline constexpr uintptr_t CPortraitObjectController_PortraitObjects_data = {data:#x};
    inline constexpr uintptr_t CPortraitObjectController_PortraitObjects_count = {count:#x};
}}  // namespace glob

namespace rt {{
    inline constexpr std::ptrdiff_t CPortraitObject_width = {width:#x};         // uint16_t, render target width
    inline constexpr std::ptrdiff_t CPortraitObject_height = {height:#x};        // uint16_t, render target height
    inline constexpr std::ptrdiff_t CPortraitObject_needs_render = {dirty:#x};  // uint8_t, the portrait is re-rendered while set
    inline constexpr std::ptrdiff_t CPortraitObject_render_target = {rt:#x};  // TextureGFX*, null until first rendered
    // engine CString: the key of the `portraits = {{}}` entry this object shows (empty or "debug" until a setter ran)
    inline constexpr std::ptrdiff_t CPortraitObject_key = {key:#x};
    // int32_t: 0 character, 1 character_large, 2 room, 3 empty_room, 4 character_without_room, 5 planet (read from SetPortrait and the constructor)
    inline constexpr std::ptrdiff_t CPortraitObject_kind = 0xA48;
    inline constexpr std::ptrdiff_t CPortraitObject_scope_type = 0x248;  // u64: 0x100 leader, 0x800 species, 0x20 pop group
    inline constexpr std::ptrdiff_t CPortraitObject_scope_id = 0x250;    // u32
    inline constexpr std::ptrdiff_t CPortraitObject_group = 0x878;       // CString: the group or selector name the portrait was set from (`human`)
    // the object is a GUI sprite and keeps where the GUI drew it last frame (GUI units, not pixels)
    // float x, y: the lower-left corner, in GUI units around the middle of the screen with y up (checked against screenshots)
    inline constexpr std::ptrdiff_t CPortraitObject_pos = 0x68;
    inline constexpr std::ptrdiff_t CPortraitObject_scale = 0xC4;      // float
    inline constexpr std::ptrdiff_t CPortraitObject_mirrored = 0xC8;   // uint8_t, 1 = drawn flipped left to right
    inline constexpr std::ptrdiff_t CPortraitObject_scissor = 0xA8;    // int x0, y0, x1, y1: the clip area in framebuffer pixels
    inline constexpr std::ptrdiff_t CPortraitObject_has_scissor = 0xBE;  // uint8_t, 1 = the scissor is valid
    // the CGuiGraphics passed to UpdatePortrait: the mouse pointer in the same GUI units
    inline constexpr std::ptrdiff_t CGuiGraphics_mouse_x = 0x350;      // float
    inline constexpr std::ptrdiff_t CGuiGraphics_mouse_y = 0x354;      // float
    inline constexpr std::ptrdiff_t CGuiGraphics_width = 0x28;         // int, size of the window in pixels (the origin of the GUI's world is its middle)
    inline constexpr std::ptrdiff_t CGuiGraphics_height = 0x2C;        // int
    inline constexpr std::ptrdiff_t CGuiGraphics_gui_width = 0x30;     // int, size of the GUI in GUI units: the pixels divided by the UI scale
    inline constexpr std::ptrdiff_t CGuiGraphics_gui_height = 0x34;    // int
}}  // namespace rt

namespace rt_settings {{
    // floats in the settings object, 0..100 as on the sliders; the effective volume of a sound is master/100 * dev_master/100 * its category/100
    inline constexpr std::ptrdiff_t master = {vol["master"]:#x};
    inline constexpr std::ptrdiff_t dev_master = {vol["dev_master"]:#x};  // a hidden multiplier, 75 by default
    inline constexpr std::ptrdiff_t music = {vol["music"]:#x};
    inline constexpr std::ptrdiff_t sfx = {vol["sfx"]:#x};  // the "Effects" category, where the portraits' greeting sounds are
    inline constexpr std::ptrdiff_t ambient = {vol["ambient"]:#x};
    inline constexpr std::ptrdiff_t voice = {vol["voice"]:#x};  // the "Voice" category: advisor and event speech
    inline constexpr std::ptrdiff_t tts = {vol["tts"]:#x};
}}  // namespace rt_settings

namespace vt {{
    // virtual void GetSize(this, int out[2]): the width and height the GUI draws the object at (sprite size times scale)
    inline constexpr int C2dObject_GetSize = 54;
}}  // namespace vt

// Layout of the engine's CString, read from the constructor of CPortraitObject (it initialises the key at +{key:#x} to "debug"):
// 0x30 bytes, characters inline in the first 16 bytes after +0x10 while the capacity (+0x28) is below 16, else a pointer there.
namespace cstring {{
    inline constexpr std::ptrdiff_t kInline = 0x10;
    inline constexpr std::ptrdiff_t kLength = 0x20;
    inline constexpr std::ptrdiff_t kCapacity = 0x28;
    inline constexpr size_t kInlineCapacity = 16;
}}  // namespace cstring
}}  // namespace sdk
"""
    if "--check" in sys.argv:
        current = out.read_text(encoding="utf-8").replace("\r\n", "\n") if out.exists() else ""
        if current != text:
            print("STALE: sdk/stellaris_sdk.hpp is not what the locator finds in this executable; run tools/locate.py", file=sys.stderr)
            sys.exit(1)
        print("sdk/stellaris_sdk.hpp is what the locator finds in this executable")
        return
    out.write_text(text, encoding="utf-8", newline="\n")
    print("wrote", out)


if __name__ == "__main__":
    main()
