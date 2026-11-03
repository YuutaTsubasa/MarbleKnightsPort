"""Print the parameter table that follows the code in a shader blob segment as
ints / strings (for reverse engineering the 2019.4 layout).
usage: python -I tools/blob_tail.py <Data file or bundle> <shader name> <platform> <blob index>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_assets as ua  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def shader_tree():
    b = ua.read_bundle(os.path.join(ROOT, "work/raw-android/AssetBundles/android/materials/catsci/m_hero_catsci_01"))
    return [t for c, t in ua.SerializedFile(list(b.values())[0]).types if c == 48][0]


def segments(o, platform):
    pi = o["platforms"].index(platform)
    off, cl, dl = o["offsets"][pi][0], o["compressedLengths"][pi][0], o["decompressedLengths"][pi][0]
    blob = ua.lz4_block(o["compressedBlob"][off:off + cl], dl)
    n = struct.unpack_from("<i", blob, 0)[0]
    out = []
    for k in range(n):
        so, sl, sg = struct.unpack_from("<iii", blob, 4 + 12 * k)
        out.append(blob[so:so + sl])
    return out


def split_segment(seg):
    magic, ptype = struct.unpack_from("<ii", seg, 0)
    p = 24
    kws = []
    for _ in range(2):
        n = struct.unpack_from("<i", seg, p)[0]; p += 4
        lst = []
        for _ in range(n):
            ln = struct.unpack_from("<i", seg, p)[0]; p += 4
            lst.append(seg[p:p + ln].decode()); p = (p + ln + 3) & ~3
        kws.append(lst)
    cl = struct.unpack_from("<i", seg, p)[0]; p += 4
    code = seg[p:p + cl]; p = (p + cl + 3) & ~3
    return ptype, kws, code, seg[p:]


def dump_ints(t):
    p, out = 0, []
    while p + 4 <= len(t):
        v = struct.unpack_from("<i", t, p)[0]
        if 0 < v < 128 and p + 4 + v <= len(t) and all(32 <= c < 127 for c in t[p + 4:p + 4 + v]):
            out.append(repr(t[p + 4:p + 4 + v].decode()))
            p = (p + 4 + v + 3) & ~3
        else:
            out.append(str(v))
            p += 4
    return " ".join(out)


if __name__ == "__main__":
    src, name, plat, idx = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    tree = shader_tree()
    files = ua.read_bundle(src) if open(src, "rb").read(7) == b"UnityFS" else {src: open(src, "rb").read()}
    for node, data in files.items():
        if node.endswith((".resS", ".resource")):
            continue
        sf = ua.SerializedFile(data)
        for _, st, sz, t in sf.objects_of(48):
            o = sf.read(st, sz, t or tree)
            if o["m_ParsedForm"]["m_Name"] != name:
                continue
            ptype, kws, code, tail = split_segment(segments(o, plat)[idx])
            print("type", ptype, "keywords", kws, "code", len(code))
            print(dump_ints(tail))
