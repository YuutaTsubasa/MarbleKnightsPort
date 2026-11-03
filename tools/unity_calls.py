"""List the calls (and immediate compares) inside one libunity.so function, named
from Unity's libunity.sym.so. usage: python -I tools/unity_calls.py <hex start> <hex end>"""
import bisect
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL = os.path.join(ROOT, "work", "unity", "AndroidPlayer", "Variations", "il2cpp", "Release")
LU = os.path.join(REL, "Libs", "arm64-v8a", "libunity.so")
SYMS = os.path.join(ROOT, "work", "an", "unity_syms.txt")
OBJDUMP = r"C:\Program Files\LLVM\bin\llvm-objdump.exe"

syms = []
for l in open(SYMS, encoding="utf-8", errors="replace"):
    a, _, n = l.rstrip("\n").split(" ", 2)
    syms.append((int(a, 16), n))
syms.sort()
addrs = [a for a, _ in syms]


def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return hex(a)
    return syms[i][1][:110] + ("" if syms[i][0] == a else f"+{a - syms[i][0]:#x}")


out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", f"--start-address={sys.argv[1]}",
                      f"--stop-address={sys.argv[2]}", LU], capture_output=True, text=True).stdout
for l in out.splitlines():
    m = re.match(r"\s+([0-9a-f]+):\s+(\S+)\s*(.*)", l)
    if not m:
        continue
    pc, op, args = m.groups()
    if op in ("bl", "b") and re.match(r"[0-9a-f]+", args):
        t = int(args.split()[0], 16)
        if op == "bl" or not (int(sys.argv[1], 16) <= t < int(sys.argv[2], 16)):
            print(pc, op, name(t))
    elif op in ("cmp", "mov") and "#" in args:
        print(pc, op, args)
