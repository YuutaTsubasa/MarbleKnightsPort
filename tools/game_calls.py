"""List the calls made by a game method in UnityFramework, naming targets with the
runtime method table (work/an/methods.txt, written by the proxy on device).
usage: python -E tools/game_calls.py <Class::Method substring>   (needs capstone; run from a neutral cwd)"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone  # noqa: E402
from macho import MachO  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UF = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Frameworks", "UnityFramework.framework",
                  "UnityFramework")
rows = sorted((int(l.split()[0], 16), l.split(" ", 1)[1].strip()) for l in open(os.path.join(ROOT, "work", "an", "methods.txt")))
addrs = [a for a, _ in rows]
m = MachO(UF)
starts = sorted(m.function_starts())
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for addr, name in rows:
    if sys.argv[1] not in name:
        continue
    end = starts[bisect.bisect_right(starts, addr)]
    print(f"== {name} {addr:#x}-{end:#x}")
    for i in md.disasm(m.read(addr, end - addr), addr):
        if i.mnemonic in ("bl", "b") and i.op_str.startswith("#"):
            t = int(i.op_str[1:], 16)
            if addr <= t < end:
                continue
            k = bisect.bisect_right(addrs, t) - 1
            tag = rows[k][1] if k >= 0 and rows[k][0] == t else f"{t:#x}"
            print(f"   {i.address:#x} {i.mnemonic} {tag}")
