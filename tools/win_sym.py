"""Find things in the official Windows UnityPlayer.dll (x64) with its PDB publics
(work/an/win/unityplayer_publics.txt, from `llvm-pdbutil dump -publics`).

usage: python -I tools/win_sym.py name <regex>          symbols matching, with their addresses
       python -I tools/win_sym.py addr <hex VA> [...]   which function each address is in
       python -I tools/win_sym.py xref "<string>"       functions with a RIP-relative lea of that string
       python -I tools/win_sym.py dis <hex VA> [len]    disassembly (llvm-objdump) with call targets named
Unity's binaries are not in Git; this only reads them under work/.
"""
import bisect
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(ROOT, "work", "unity", "winil2cpp", "$INSTDIR$_59_", "Variations", "win64_nondevelopment_il2cpp", "UnityPlayer.dll")
PUB = os.path.join(ROOT, "work", "an", "win", "unityplayer_publics.txt")
OBJDUMP = r"C:\Program Files\LLVM\bin\llvm-objdump.exe"


def sections():
    d = open(DLL, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
    out = []
    o = pe + 24 + opt
    for k in range(nsec):
        name = d[o:o + 8].rstrip(b"\0").decode()
        vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, base + va, vsize, raw, rsize))
        o += 40
    return d, base, out


def publics():
    secs = sections()[2]
    syms = []
    name = None
    for line in open(PUB, encoding="utf-8", errors="replace"):
        m = re.search(r"`(.*)`", line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r"addr = (\d+):(\d+)", line)
        if m and name:
            seg, off = int(m.group(1)), int(m.group(2))
            if 1 <= seg <= len(secs):
                syms.append((secs[seg - 1][1] + off, name, "function" in line))
            name = None
    syms.sort()
    return syms


def owner(syms, addrs, va):
    k = bisect.bisect_right(addrs, va) - 1
    return syms[k] if k >= 0 else None


def main():
    cmd = sys.argv[1]
    if cmd == "name":
        rx = re.compile(sys.argv[2])
        for va, n, fn in publics():
            if rx.search(n):
                print(f"{va:#x} {'F' if fn else 'D'} {n}")
        return
    syms = [s for s in publics() if s[2]]
    addrs = [s[0] for s in syms]
    if cmd == "addr":
        for a in sys.argv[2:]:
            va = int(a, 16)
            s = owner(syms, addrs, va)
            print(f"{va:#x} = {s[1]}+{va - s[0]:#x}" if s else f"{va:#x} ?")
        return
    d, base, secs = sections()
    if cmd == "xref":
        needle = sys.argv[2].encode()
        targets = []
        for name, va, vsize, raw, rsize in secs:
            blob = d[raw:raw + rsize]
            for m in re.finditer(re.escape(needle), blob):
                st = m.start()
                while st > 0 and blob[st - 1] != 0:  # the code points at the start of the string
                    st -= 1
                targets.append(va + st)
        text = next(s for s in secs if s[0] == ".text")
        blob = d[text[3]:text[3] + text[4]]
        found = set()
        for t in targets:
            # find the start of the string's run too (the reference may point at its beginning)
            for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", blob):
                i = m.start()
                disp = struct.unpack_from("<i", blob, i + 3)[0]
                if text[1] + i + 7 + disp == t:
                    s = owner(syms, addrs, text[1] + i)
                    found.add((text[1] + i, s[1] if s else "?"))
            print(f"string at {t:#x}")
        for va, n in sorted(found):
            print(f"  {va:#x} in {n}")
        return
    if cmd == "dis":
        va = int(sys.argv[2], 16)
        ln = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x200
        out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", f"--start-address={va:#x}", f"--stop-address={va + ln:#x}", DLL],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.search(r"(call|jmp)q?\s+0x([0-9a-f]+)", line)
            if m:
                s = owner(syms, addrs, int(m.group(2), 16))
                if s:
                    line = re.sub(r"<.*>", f"<{s[1]}+{int(m.group(2), 16) - s[0]:#x}>", line)
            if ":" in line:
                print(line)


if __name__ == "__main__":
    main()
