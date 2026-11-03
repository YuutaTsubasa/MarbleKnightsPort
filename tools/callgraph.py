"""Approximate static call graph of UnityFramework.

Finds which functions (and through them, which imported symbols) are reachable
from a set of roots: by default the exported il2cpp_* API plus everything in the
`__TEXT,il2cpp` section (IL2CPP generated game code). Edges are BL/B targets
and ADRP+ADD/LDR references to function starts (function pointers, vtables are
only caught when referenced from code, so this under-approximates; data
references from __DATA pointer tables are added as an option).

usage: python -I tools/callgraph.py <UnityFramework> <outdir>
"""
import bisect
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def main():
    path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    m = MachO(path)
    text = m.section("__TEXT", "__text")
    il2 = m.section("__TEXT", "il2cpp")
    stubs = m.section("__TEXT", "__stubs")
    starts = sorted(m.function_starts())
    exports = m.exports()
    imports = m.import_map()
    code_lo, code_hi = text.addr, il2.addr + il2.size

    # stub address -> imported symbol
    stub_sym = {}
    for a in range(stubs.addr, stubs.addr + stubs.size, 12):
        w0, w1 = struct.unpack("<II", m.read(a, 8))
        page = ((a >> 12) + sx(((w0 >> 29) & 3) | (((w0 >> 5) & 0x7FFFF) << 2), 21)) << 12
        slot = page + ((w1 >> 10) & 0xFFF) * 8
        stub_sym[a] = imports.get(slot, ("?", f"slot{slot:#x}"))

    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    startset = set(starts)
    callees = collections.defaultdict(set)   # func -> funcs
    uses = collections.defaultdict(set)      # func -> imported symbols (lib, sym)

    for sect in (text, il2):
        raw = m.read(sect.addr, sect.size)
        words = struct.unpack(f"<{len(raw) // 4}I", raw[: len(raw) // 4 * 4])
        fi = bisect.bisect_right(starts, sect.addr) - 1
        nxt = starts[fi + 1] if fi + 1 < len(starts) else 1 << 64
        cur = starts[fi]
        adrp = {}
        for k, w in enumerate(words):
            pc = sect.addr + 4 * k
            while pc >= nxt:
                fi += 1
                cur = starts[fi]
                nxt = starts[fi + 1] if fi + 1 < len(starts) else 1 << 64
                adrp.clear()
            top = w & 0xFC000000
            if top in (0x94000000, 0x14000000):  # BL / B
                t = pc + sx(w & 0x3FFFFFF, 26) * 4
                if stubs.addr <= t < stubs.addr + stubs.size:
                    uses[cur].add(stub_sym.get(t - (t - stubs.addr) % 12, ("?", "?")))
                elif code_lo <= t < code_hi and (top == 0x94000000 or t in startset):
                    f = func_of(t)
                    if f != cur:
                        callees[cur].add(f)
            elif (w & 0x9F000000) == 0x90000000:  # ADRP
                rd = w & 31
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
                adrp[rd] = ((pc >> 12) + sx(imm, 21)) << 12
            elif (w & 0xFF800000) == 0x91000000:  # ADD imm (64-bit)
                rn, rd = (w >> 5) & 31, w & 31
                if rn in adrp:
                    t = adrp[rn] + ((w >> 10) & 0xFFF)
                    if t in startset:
                        callees[cur].add(t)
                    if rd != rn:
                        adrp.pop(rd, None)
            elif (w & 0xFFC00000) == 0xF9400000:  # LDR Xt, [Xn, #imm]
                rn, rt = (w >> 5) & 31, w & 31
                if rn in adrp:
                    slot = adrp[rn] + ((w >> 10) & 0xFFF) * 8
                    if slot in imports:
                        uses[cur].add(imports[slot])
                adrp.pop(rt, None)

    il2_funcs = [f for f in starts if il2.addr <= f < il2.addr + il2.size]
    api = {n: a for n, a in exports.items() if n.startswith("_il2cpp_")}
    fmod = {n: a for n, a in exports.items() if "FMOD" in n}

    def reach(roots):
        seen, todo = set(roots), list(roots)
        while todo:
            f = todo.pop()
            for c in callees.get(f, ()):
                if c not in seen:
                    seen.add(c)
                    todo.append(c)
        return seen

    r_api = reach(api.values())
    r_game = reach(il2_funcs)
    r_fmod = reach(fmod.values())
    r_all = r_api | r_game

    def import_summary(funcs):
        c = collections.defaultdict(set)
        for f in funcs:
            for lib, sym in uses.get(f, ()):
                c[lib].add(sym)
        return {k: sorted(v) for k, v in sorted(c.items())}

    # direct calls from game code into __text that are NOT reachable from the il2cpp API:
    # P/Invoke("__Internal") plugins or engine functions called without an icall
    direct = collections.Counter()
    for f in il2_funcs:
        for c in callees.get(f, ()):
            if text.addr <= c < text.addr + text.size and c not in r_api:
                direct[c] += 1
    names = {a: n for n, a in exports.items()}
    for name, ntype, nsect, ndesc, value in m.symbols():
        if value and ntype & 0x0E == 0x0E:
            names.setdefault(value, name)

    report = {
        "functions": len(starts),
        "il2cpp_section_functions": len(il2_funcs),
        "il2cpp_api_exports": len(api),
        "reachable_from_api": len(r_api),
        "reachable_from_game": len(r_game),
        "reachable_union": len(r_all),
        "reachable_from_fmod_exports": len(r_fmod),
        "imports_reachable_from_api": import_summary(r_api),
        "imports_reachable_from_api_or_game": import_summary(r_all),
        "imports_reachable_from_fmod": import_summary(r_fmod),
        "game_direct_calls_outside_api": [
            {"addr": hex(a), "name": names.get(a, ""), "callers": n,
             "reach_imports": import_summary(reach([a]))}
            for a, n in direct.most_common()],
    }
    with open(os.path.join(outdir, "callgraph.json"), "w") as fp:
        json.dump(report, fp, indent=1)
    for k, v in report.items():
        if isinstance(v, int):
            print(f"{k}: {v}")
    for key in ("imports_reachable_from_api", "imports_reachable_from_api_or_game", "imports_reachable_from_fmod"):
        print(key, {lib: len(s) for lib, s in report[key].items()})
    print("game_direct_calls_outside_api:", len(report["game_direct_calls_outside_api"]))


if __name__ == "__main__":
    main()
