"""Windows build: decode the PVRTC textures of the AssetBundles to RGBA32.

The iOS data keeps most textures as PVRTC 4bpp, which desktop GPUs cannot sample. The Windows
player decompresses them on the CPU at load time, but mipmapped ones come out black (the 3D
scenes are black with only specular highlights left), so the bundles of the Windows build get
RGBA32 textures with every mip level decoded here (tools/pvrtc_decode.c).

usage: python -I tools/win_textures.py [StreamingAssets dir]
Rewrites bundles in place under work/win/MarbleKnights (they start as hard links to the Android
data: the new file replaces the link, the original stays). Already converted bundles are skipped.
Reads and writes game data, so everything stays in work/.
"""
import ctypes
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_assets as u  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SA = os.path.join(ROOT, "work", "win", "MarbleKnights", "MarbleKnights_Data", "StreamingAssets")
DLL = os.path.join(ROOT, "work", "win", "tools", "pvrtc_decode.dll")
SRC = os.path.join(ROOT, "tools", "pvrtc_decode.c")
CLANG = r"C:\Program Files\LLVM\bin\clang.exe"
PVRTC_RGB4, PVRTC_RGBA4, RGBA32 = 32, 33, 4


def decoder():
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


def convert_bundle(lib, path):
    entries = u.read_bundle(path)
    changed, n = {}, 0
    for name, data in entries.items():
        if name.endswith((".resS", ".resource")):
            continue
        try:
            sf = u.SerializedFile(data)
        except ValueError:
            continue
        replaced = {}
        for pid, st, sz, tree in sf.objects_of(28):
            v = sf.read(st, sz, tree)
            if v["m_TextureFormat"] not in (PVRTC_RGB4, PVRTC_RGBA4) or v.get("m_ImageCount", 1) != 1:
                continue
            sd = v.get("m_StreamData") or {}
            if sd.get("size"):
                res = sd["path"].split("/")[-1]
                blob = changed.get(res, entries[res])
                raw = blob[sd["offset"]:sd["offset"] + sd["size"]]
            else:
                raw = bytes(v["image data"])
            try:
                rgba = convert_texture(lib, v, raw)
            except ValueError:
                continue
            if sd.get("size"):
                blob = bytearray(changed.get(res, entries[res]))
                while len(blob) % 16:
                    blob.append(0)
                sd["offset"], sd["size"] = len(blob), len(rgba)
                changed[res] = bytes(blob + rgba)
            else:
                v["image data"] = rgba
            v["m_TextureFormat"] = RGBA32
            v["m_CompleteImageSize"] = len(rgba)
            replaced[pid] = u.write_object(tree, v, sf.big)
            n += 1
        if replaced:
            changed[name] = u.rebuild_serialized(sf, replaced)
    if not n:
        return 0
    files = {k: changed.get(k, d) for k, d in entries.items()}
    tmp = path + ".tmp"
    u.write_bundle(tmp, files)
    os.replace(tmp, path)  # replaces the hard link; the Android original is untouched
    return n


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


if __name__ == "__main__":
    main()
