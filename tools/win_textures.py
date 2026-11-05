"""Windows build: decode the PVRTC textures of the AssetBundles to RGBA32.

The iOS data keeps most textures as PVRTC 4bpp, which desktop GPUs cannot sample. The Windows
player decompresses them on the CPU at load time, but mipmapped ones come out black (the 3D
scenes are black with only specular highlights left), so the bundles of the Windows build get
RGBA32 textures with every mip level decoded here (tools/pvrtc_decode.c).

usage: python -I tools/win_textures.py [StreamingAssets dir]
Rewrites bundles in place under work/win/MarbleKnights (they start as hard links to the Android
data: the new file replaces the link, the original stays). Already converted bundles are skipped.
The player data files (resources.assets...) are converted too, with copies in work/win/patched-data
that tools/build_win.py copies over the ones it assembles.
Reads and writes game data, so everything stays in work/.
"""
import ctypes
import os
from struct import error as struct_error
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_assets as u  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SA = os.path.join(ROOT, "work", "win", "MarbleKnights", "MarbleKnights_Data", "StreamingAssets")
DLL = os.path.join(ROOT, "work", "win", "tools", "pvrtc_decode.dll")
SRC = os.path.join(ROOT, "tools", "pvrtc_decode.c")
OVERLAY = os.path.join(ROOT, "work", "win", "patched-data")  # converted player data files, used by build_win.py
CLANG = r"C:\Program Files\LLVM\bin\clang.exe"
PVRTC_RGB4, PVRTC_RGBA4, RGBA32 = 32, 33, 4


def decoder(dll=None):
    """dll: a prebuilt decoder (the launcher's); otherwise built from tools/pvrtc_decode.c"""
    if dll:
        lib = ctypes.CDLL(dll)
        lib.pvrtc4_decode.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        return lib
    if not os.path.exists(DLL) or os.path.getmtime(DLL) < os.path.getmtime(SRC):
        os.makedirs(os.path.dirname(DLL), exist_ok=True)
        subprocess.run([CLANG, "--target=x86_64-pc-windows-msvc", "-O2", "-shared", "-o", DLL, SRC], check=True)
    lib = ctypes.CDLL(DLL)
    lib.pvrtc4_decode.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
    return lib


def convert_texture(lib, v, raw):
    """raw: the PVRTC data of all mips -> RGBA32 data of all mips"""
    w, h, out, p = v["m_Width"], v["m_Height"], bytearray(), 0
    for _ in range(v["m_MipCount"]):
        size = max(w, 8) * max(h, 8) // 2
        buf = ctypes.create_string_buffer(w * h * 4)
        if lib.pvrtc4_decode(raw[p:p + size], w, h, buf) != 0:
            raise ValueError("not a power of two")
        out += buf.raw
        p += size
        w, h = max(w // 2, 1), max(h // 2, 1)
    return bytes(out)


def convert_serialized(lib, sf, res, tree=None):
    """Convert the PVRTC textures of one SerializedFile. res: {resS name: bytes}, updated in place
    (converted data is appended). tree: the Texture2D type tree when the file has none (player
    data). Returns (new file bytes or None, number converted)."""
    replaced = {}
    for pid, st, sz, t in sf.objects_of(28):
        t = t or tree
        v = sf.read(st, sz, t)
        if v["m_TextureFormat"] not in (PVRTC_RGB4, PVRTC_RGBA4) or v.get("m_ImageCount", 1) != 1:
            continue
        sd = v.get("m_StreamData") or {}
        name = sd["path"].split("/")[-1] if sd.get("size") else None
        raw = res[name][sd["offset"]:sd["offset"] + sd["size"]] if name else bytes(v["image data"])
        try:
            rgba = convert_texture(lib, v, raw)
        except ValueError:
            continue
        if name:
            blob = bytearray(res[name])
            while len(blob) % 16:
                blob.append(0)
            sd["offset"], sd["size"] = len(blob), len(rgba)
            res[name] = bytes(blob + rgba)
        else:
            v["image data"] = rgba
        v["m_TextureFormat"] = RGBA32
        v["m_CompleteImageSize"] = len(rgba)
        replaced[pid] = u.write_object(t, v, sf.big)
    return (u.rebuild_serialized(sf, replaced) if replaced else None), len(replaced)


def convert_bundle(lib, path):
    entries = u.read_bundle(path)
    res = {k: d for k, d in entries.items() if k.endswith((".resS", ".resource"))}
    out, n = dict(entries), 0
    for name, data in entries.items():
        if name in res:
            continue
        try:
            sf = u.SerializedFile(data)
        except ValueError:
            continue
        new, k = convert_serialized(lib, sf, res, None)
        if new:
            out[name] = new
            n += k
    if not n:
        return 0
    out.update(res)
    tmp = path + ".tmp"
    u.write_bundle(tmp, out)
    os.replace(tmp, path)  # replaces the hard link; the Android original is untouched
    return n


def texture_tree(sa):
    """Texture2D type tree from any bundle (the player data files carry none)."""
    for dirpath, _, names in os.walk(sa):
        for nm in names:
            try:
                entries = u.read_bundle(os.path.join(dirpath, nm))
            except (ValueError, OSError):
                continue
            for name, data in entries.items():
                if name.endswith((".resS", ".resource")):
                    continue
                for _, _, _, t in u.SerializedFile(data).objects_of(28):
                    if t:
                        return t
    raise SystemExit("no Texture2D type tree found")


def convert_loose(lib, data_dir, overlay, tree):
    """Player data files (resources.assets...): converted copies go to the overlay folder that
    tools/build_win.py prefers, and into data_dir."""
    os.makedirs(overlay, exist_ok=True)
    total = 0
    for nm in sorted(os.listdir(data_dir)):
        p = os.path.join(data_dir, nm)
        if not os.path.isfile(p) or nm.endswith((".resS", ".resource", ".config", ".tmp")):
            continue
        try:
            sf = u.SerializedFile(open(p, "rb").read())
        except (ValueError, struct_error):
            continue
        res = {}
        for r in (nm + ".resS", nm + ".resource"):
            if os.path.exists(os.path.join(data_dir, r)):
                res[r] = open(os.path.join(data_dir, r), "rb").read()
        before = dict(res)
        new, k = convert_serialized(lib, sf, res, tree)
        if not new:
            continue
        for name, blob in [(nm, new)] + [(r, b) for r, b in res.items() if b is not before[r]]:
            for d in (overlay, data_dir):
                with open(os.path.join(d, name + ".tmp"), "wb") as f:
                    f.write(blob)
                os.replace(os.path.join(d, name + ".tmp"), os.path.join(d, name))
        print(f"{nm}: {k} textures")
        total += k
    return total


def main():
    sa = sys.argv[1] if len(sys.argv) > 1 else SA
    lib = decoder()
    total = nb = 0
    for dirpath, _, names in os.walk(sa):
        for nm in sorted(names):
            path = os.path.join(dirpath, nm)
            if nm.endswith((".manifest", ".tmp")):
                continue
            with open(path, "rb") as f:
                if f.read(8) != b"UnityFS\0":
                    continue
            k = convert_bundle(lib, path)
            if k:
                nb += 1
                total += k
                print(f"{os.path.relpath(path, sa)}: {k} textures")
    print(f"{total} PVRTC textures in {nb} bundles converted to RGBA32")
    data_dir = os.path.dirname(sa)
    k = convert_loose(lib, data_dir, OVERLAY, texture_tree(sa))
    print(f"{k} PVRTC textures in the player data converted (overlay {OVERLAY})")


if __name__ == "__main__":
    main()
