"""Survey the IL2CPP-generated game code in UnityFramework for a static ARM64 -> C recompilation
(Windows/Linux build): size, instruction mix, indirect branches, which non-managed functions the
game methods call (IL2CPP runtime helpers), and how many methods have C++ exception regions (LSDA).

Managed methods come from work/an/methods.txt (written by the proxy on device); everything else in
__text is the IL2CPP runtime, plugins and libraries. Results go to stdout; game data stays in work/.
usage: python -E tools/recomp_survey.py [--top N]   (needs capstone; run from a neutral cwd)
"""
import bisect
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UF = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Frameworks", "UnityFramework.framework", "UnityFramework")
TOP = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 40


def lsda_functions(m):
    """Function offsets that have an LSDA (C++ landing pads) in __unwind_info."""
    s = m.section("__TEXT", "__unwind_info")
    d = m.read(s.addr, s.size)
    _, _, _, _, _, idx_off, idx_cnt = struct.unpack_from("<7I", d, 0)
    first = struct.unpack_from("<III", d, idx_off)[2]
    last = struct.unpack_from("<III", d, idx_off + 12 * (idx_cnt - 1))[2]
    return {struct.unpack_from("<II", d, o)[0] for o in range(first, last, 8)}


def main():
    m = MachO(UF)
    eng = m.section("__TEXT", "__text")      # Unity engine, IL2CPP runtime, plugins, libraries
    text = m.section("__TEXT", "il2cpp")     # IL2CPP-generated code (the game's C# and the BCL)
    base = 0  # __TEXT is at vmaddr 0
    starts = sorted(m.function_starts())
    names = {}
    for line in open(os.path.join(ROOT, "work", "an", "methods.txt"), encoding="utf-8", errors="replace"):
        a, _, n = line.strip().partition(" ")
        try:
            names.setdefault(int(a, 16), n)
        except ValueError:
            pass
    managed = sorted(a for a in names if text.addr <= a < text.addr + text.size)
    mset = set(managed)
    lsda = lsda_functions(m)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.skipdata = True
    mnem = collections.Counter()
    callees = collections.Counter()
    callee_users = collections.defaultdict(set)
    n_insn = n_blr = n_br = n_simd = 0
    sizes = []
    end_of = {}
    for i, a in enumerate(starts):
        end_of[a] = starts[i + 1] if i + 1 < len(starts) else text.addr + text.size
    blob = m.read(text.addr, text.size)
    for a in managed:
        e = end_of.get(a)
        if e is None:
            k = bisect.bisect_right(starts, a)
            e = starts[k] if k < len(starts) else text.addr + text.size
        code = blob[a - text.addr:e - text.addr]
        sizes.append(len(code) // 4)
        for ins in md.disasm(code, a):
            n_insn += 1
            mn = ins.mnemonic
            mnem[mn] += 1
            if mn == "blr":
                n_blr += 1
            elif mn == "br":
                n_br += 1
            elif mn in ("bl", "b") and ins.op_str.startswith("#"):
                t = int(ins.op_str[1:], 16)
                if mn == "b" and a <= t < e:
                    continue
                if t not in mset:
                    callees[t] += 1
                    callee_users[t].add(a)
            if any(r in ins.op_str for r in ("v0", "v1", "q0", ".4s", ".2d", ".16b", ".8h")) or mn.startswith(("ld1", "st1", "fmla", "dup", "ins", "umov")):
                n_simd += 1
    total = text.size // 4
    print(f"__text (engine + runtime) {eng.size / 2**20:.1f} MB; il2cpp (generated) {text.size / 2**20:.1f} MB, {total / 1e6:.2f}M instructions")
    print(f"functions in il2cpp section: {sum(1 for a in starts if text.addr <= a < text.addr + text.size)}")
    print(f"managed methods: {len(managed)} ({n_insn / 1e6:.2f}M instructions, {100 * n_insn / total:.0f}% of the il2cpp section)")
    sizes.sort()
    print(f"  size (instructions) median {sizes[len(sizes) // 2]}, p90 {sizes[len(sizes) * 9 // 10]}, max {sizes[-1]}")
    lsda_managed = sum(1 for a in managed if (a - base) in lsda)
    print(f"  with exception regions (LSDA): {lsda_managed} ({100 * lsda_managed / len(managed):.1f}%); LSDA functions overall {len(lsda)}")
    print(f"  indirect calls (blr) {n_blr}, indirect jumps (br) {n_br}, SIMD-looking instructions {n_simd}")
    print(f"  distinct mnemonics {len(mnem)}; top {TOP}:")
    for mn, c in mnem.most_common(TOP):
        print(f"    {mn:10} {c:9} {100 * c / n_insn:5.1f}%")
    print(f"  distinct non-managed call targets: {len(callees)}; top {TOP} by number of calling methods:")
    for t, c in sorted(callees.items(), key=lambda kv: -len(callee_users[kv[0]]))[:TOP]:
        print(f"    {t:#x} calls {c:7} from {len(callee_users[t]):6} methods")
    ranked = sorted(callees, key=lambda t: -len(callee_users[t]))
    per_method = collections.defaultdict(set)
    for t, us in callee_users.items():
        for u in us:
            per_method[u].add(t)
    for k in (10, 30, 100, 300, 1000):
        top = set(ranked[:k])
        ok = sum(1 for a in managed if per_method[a] <= top)
        print(f"  methods whose external calls all go to the top {k} targets: {ok} ({100 * ok / len(managed):.1f}%)")

if __name__ == "__main__":
    main()
