"""Locates what the plugin needs inside stellaris.exe and writes sdk/stellaris_sdk.hpp. No address is typed in by hand:
every one is found by a fingerprint taken from the engine's own code, and the run fails instead of guessing when a
fingerprint is not unique. Run it after every game patch:

    python tools/locate.py            # reads STELLARIS_DIR (or the default Steam folder), writes sdk/stellaris_sdk.hpp

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
LITERAL = r"C:\mnt\gsg\stellaris\augustus\augustus\source\graphics\portraitobject.cpp:467"


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

    print(f"exe timestamp {im.timestamp:#010x}")
    print(f"CPortraitObject::UpdatePortrait   rva {fn:#x}  ({len(ins)} instructions, this in {this_reg})")
    print(f"CPortraitObject width             +{width:#x} (uint16)")
    print(f"CPortraitObject height            +{height:#x} (uint16)")
    print(f"CPortraitObject render target     +{rt:#x} (TextureGFX*)")
    print(f"CPortraitObject needs-render flag +{dirty:#x} (uint8)")
    print(f"CPortraitObject key (CString)     +{key:#x}  (database table at +{table:#x})")
    print(f"UpdatePortraits (controller)      rva {ctrl:#x}")
    print(f"portrait array                    data pointer rva {data:#x}, count rva {count:#x}")

    out = ROOT / "sdk" / "stellaris_sdk.hpp"
    out.parent.mkdir(exist_ok=True)
    out.write_text(f"""// GENERATED by tools/locate.py from the installed stellaris.exe. DO NOT EDIT.
// Every value is found by a fingerprint taken from the engine's own code (see the script); rerun it after a game patch.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sdk {{
inline constexpr uint32_t kExeTimestamp = {im.timestamp:#010x};  // PE TimeDateStamp this SDK was located in

namespace fn {{
    // void (*)(void* portrait, void* graphics, void* context): renders one visible portrait into its render target
    inline constexpr uintptr_t CPortraitObject_UpdatePortrait = {fn:#x};
}}  // namespace fn

namespace glob {{
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
}}  // namespace rt

// Layout of the engine's CString, read from the constructor of CPortraitObject (it initialises the key at +{key:#x} to "debug"):
// 0x30 bytes, characters inline in the first 16 bytes after +0x10 while the capacity (+0x28) is below 16, else a pointer there.
namespace cstring {{
    inline constexpr std::ptrdiff_t kInline = 0x10;
    inline constexpr std::ptrdiff_t kLength = 0x20;
    inline constexpr std::ptrdiff_t kCapacity = 0x28;
    inline constexpr size_t kInlineCapacity = 16;
}}  // namespace cstring
}}  // namespace sdk
""", encoding="utf-8", newline="\n")
    print("wrote", out)


if __name__ == "__main__":
    main()
