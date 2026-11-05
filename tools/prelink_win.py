"""Lay out UnityFramework for the Windows build: the runtime only has to copy one blob and patch
pointers, instead of parsing Mach-O (the code itself is recompiled to C, so only data matters).

work/win/image/ (game data: never in Git):
  image.bin     the memory image from vmaddr 0 to the end of the last segment (zero-fill included)
  rebases.bin   uint32 vm offsets of pointers to slide by the load address
  binds.txt     "offset symbol dylib addend weak" for every imported pointer slot (binds and lazy binds)
  inits.txt     vm offsets of the static initializers (__mod_init_func), in order
  exports.txt   "offset name" for every exported symbol (the il2cpp_* API, FMOD, plugins)
usage: python -I tools/prelink_win.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UF = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Frameworks", "UnityFramework.framework", "UnityFramework")
OUT = os.path.join(ROOT, "work", "win", "image")


def main(uf=UF, out=OUT):
    m = MachO(uf)
    segs = [s for s in m.segments if s["name"] != "__LINKEDIT" and s["vmsize"]]
    end = max(s["vmaddr"] + s["vmsize"] for s in segs)
    img = bytearray(end)
    for s in segs:
        img[s["vmaddr"]:s["vmaddr"] + s["filesize"]] = m.data[s["fileoff"]:s["fileoff"] + s["filesize"]]
    os.makedirs(out, exist_ok=True)
    rb = sorted(set(m.rebases()))
    with open(os.path.join(out, "rebases.bin"), "wb") as f:
        f.write(struct.pack(f"<{len(rb)}I", *rb))
    binds = m.binds("bind") + m.binds("lazy") + m.binds("weak")
    libs = ["self"] + m.dylibs
    seen = set()
    with open(os.path.join(out, "binds.txt"), "w", encoding="utf-8", newline="\n") as f:
        for addr, ordinal, sym, addend, weak in binds:
            if addr in seen:
                continue
            seen.add(addr)
            lib = os.path.basename(libs[ordinal]) if 0 <= ordinal < len(libs) else f"ord{ordinal}"
            f.write(f"{addr:x} {sym} {lib} {addend} {int(bool(weak))}\n")
    init = m.section("__DATA_CONST", "__mod_init_func") or m.section("__DATA", "__mod_init_func")
    inits = [struct.unpack_from("<Q", img, init.addr + 8 * k)[0] for k in range(init.size // 8)]
    with open(os.path.join(out, "inits.txt"), "w", newline="\n") as f:
        f.write("".join(f"{a:x}\n" for a in inits))
    with open(os.path.join(out, "exports.txt"), "w", newline="\n") as f:
        f.write("".join(f"{a:x} {n}\n" for n, a in sorted(m.exports().items(), key=lambda kv: kv[1])))
    with open(os.path.join(out, "image.bin"), "wb") as f:
        f.write(img)
    print(f"image {end / 2**20:.1f} MB, {len(rb)} rebases, {len(seen)} bound slots, {len(inits)} initializers -> {out}")


if __name__ == "__main__":
    main()
