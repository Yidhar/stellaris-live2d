"""A small read-only view of stellaris.exe for locating engine code: the mapped image, the function table from
.pdata, a linear disassembler that follows forward branches (MSVC splits functions into hot and cold parts), and an
index of rip-relative lea targets (string literals, vtables).

Needs `pip install pefile capstone`. The game directory comes from STELLARIS_DIR (default: the usual Steam folder).
"""
import bisect
import collections
import os
import re
import struct
from pathlib import Path

import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

GAME_DIR = Path(os.environ.get("STELLARIS_DIR", r"E:\Program Files (x86)\Steam\steamapps\common\Stellaris"))
EXE = GAME_DIR / "stellaris.exe"


class Image:
    def __init__(self, path=EXE):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.ib = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = bytes(self.pe.get_memory_mapped_image())  # indexed by RVA
        self.timestamp = self.pe.FILE_HEADER.TimeDateStamp
        secs = {s.Name.rstrip(b"\0").decode(): s for s in self.pe.sections}
        t, r = secs[".text"], secs[".rdata"]
        self.text0, self.text1 = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
        self.rdata0, self.rdata1 = r.VirtualAddress, r.VirtualAddress + r.Misc_VirtualSize
        p = secs[".pdata"]
        raw = self.img[p.VirtualAddress:p.VirtualAddress + p.Misc_VirtualSize]
        entries = {struct.unpack_from("<III", raw, i) for i in range(0, len(raw) - 11, 12)}
        rf = sorted({(a, b) for a, b, _ in entries} - {(0, 0)})
        self.fstarts = [a for a, _ in rf]
        self.fend = dict(rf)
        self.unwind = {a: u for a, _, u in entries}
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True
        self._lea = None

    def fn_of(self, rva):
        """Start of the .pdata entry containing rva."""
        i = bisect.bisect_right(self.fstarts, rva) - 1
        return self.fstarts[i] if i >= 0 else None

    def primary(self, f):
        """The function a .pdata fragment belongs to: MSVC splits a function into chained entries
        (UNW_FLAG_CHAININFO) whose unwind info ends with the parent's RUNTIME_FUNCTION."""
        for _ in range(8):
            ui = self.unwind.get(f)
            if not ui or ui + 4 > len(self.img):
                return f
            flags, count = self.img[ui] >> 3, self.img[ui + 2]
            if not flags & 0x4:
                return f
            parent = struct.unpack_from("<I", self.img, ui + 4 + ((count + 1) & ~1) * 2)[0]
            if not parent or parent == f:
                return f
            f = parent
        return f

    def q(self, rva):
        return struct.unpack_from("<Q", self.img, rva)[0]

    def disasm_fn(self, start, limit=0x4000):
        """Linear sweep that follows forward branches, so a cold part does not end the function early."""
        out, reach = [], start
        for ins in self.md.disasm(self.img[start:start + limit], self.ib + start):
            rva = ins.address - self.ib
            if rva > start and ins.bytes[0] == 0xCC and rva >= reach:
                break
            out.append(ins)
            if ins.group(7) or ins.mnemonic.startswith("j"):
                op = ins.operands[0] if ins.operands else None
                if op is not None and op.type == X86_OP_IMM:
                    t = op.imm - self.ib
                    if start < t < start + limit:
                        reach = max(reach, t)
            if ins.mnemonic in ("ret", "jmp") and rva >= reach:
                break
        return out

    def rip_target(self, ins):
        """RVA a rip-relative memory operand points at, or None."""
        for op in ins.operands:
            if op.type == X86_OP_MEM and ins.reg_name(op.mem.base) == "rip":
                return ins.address - self.ib + ins.size + op.mem.disp
        return None

    def lea_index(self):
        """rip-relative lea target -> [instruction rva] (string literals, vtable starts)."""
        if self._lea is None:
            idx = collections.defaultdict(list)
            text = self.img[self.text0:self.text1]
            for m in re.finditer(rb"[\x48\x4C]\x8D[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]", text):
                i = m.start()
                disp = struct.unpack_from("<i", text, i + 3)[0]
                idx[self.text0 + i + 7 + disp].append(self.text0 + i)
            self._lea = idx
        return self._lea

    def find_string(self, text):
        """RVA of a NUL-terminated string literal (exact match), or None."""
        needle = b"\0" + text.encode() + b"\0"
        pos = self.img.find(needle)
        return pos + 1 if pos >= 0 else None

    def cstr(self, rva, n=128):
        return self.img[rva:rva + n].split(b"\0")[0].decode("latin1")

    def functions_referencing(self, string_rva):
        """Primary function starts whose code lea's the string."""
        return sorted({self.primary(self.fn_of(r)) for r in self.lea_index().get(string_rva, [])})
