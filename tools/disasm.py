"""Disassemble UnityFramework around an image offset, annotating ADRP+ADD/LDR targets
(C strings, import slots) and BL targets (exports, imports via stubs).

usage: python -E tools/disasm.py <UnityFramework> <hex addr> [before] [after]
needs capstone (user site-packages), so run from a neutral cwd with -E rather than -I.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
from macho import MachO  # noqa: E402

m = MachO(sys.argv[1])
addr = int(sys.argv[2], 16)
before = int(sys.argv[3]) if len(sys.argv) > 3 else 24
after = int(sys.argv[4]) if len(sys.argv) > 4 else 12
imports = m.import_map()
exports = {a: n for n, a in m.exports().items()}
stubs = m.section("__TEXT", "__stubs")
starts = sorted(m.function_starts())
import bisect  # noqa: E402

fi = bisect.bisect_right(starts, addr) - 1
print(f"function start {starts[fi]:#x} ({exports.get(starts[fi], '')}), next {starts[fi + 1]:#x}")


def stub_target(t):
    w0, w1 = struct.unpack("<II", m.read(t, 8))
    imm = ((w0 >> 29) & 3) | (((w0 >> 5) & 0x7FFFF) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    page = ((t >> 12) + imm) << 12
    return imports.get(page + ((w1 >> 10) & 0xFFF) * 8)


md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True
start = max(starts[fi], addr - 4 * before)
regs = {}
for i in md.disasm(m.read(start, 4 * (before + after + 1)), start):
    note = ""
    if i.mnemonic == "adrp":
        regs[i.operands[0].reg] = i.operands[1].imm
    elif i.mnemonic in ("add", "ldr") and len(i.operands) >= 2:
        op = i.operands[1]
        base = op.mem.base if op.type == capstone.arm64.ARM64_OP_MEM else op.reg
        if base in regs:
            t = regs[base] + (op.mem.disp if op.type == capstone.arm64.ARM64_OP_MEM else i.operands[2].imm)
            if t in imports:
                note = f"; {imports[t]}"
            else:
                sec = m.section_at(t)
                if sec and sec.sectname in ("__cstring", "__const"):
                    s = m.cstr(t, 120)
                    note = f"; {sec.sectname} {t:#x} {s!r}"
                elif sec:
                    note = f"; {sec.segname},{sec.sectname} {t:#x}"
    elif i.mnemonic in ("bl", "b") and i.operands[0].type == capstone.arm64.ARM64_OP_IMM:
        t = i.operands[0].imm
        if stubs.addr <= t < stubs.addr + stubs.size:
            note = f"; -> {stub_target(t)}"
        elif t in exports:
            note = f"; -> {exports[t]}"
    mark = "=>" if i.address == addr else "  "
    print(f"{mark} {i.address:#09x} {i.mnemonic:8} {i.op_str} {note}")
