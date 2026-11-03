"""Add the Hidden/VideoDecodeAndroid shader that the Android video decoder needs.

The editor puts it into unity_builtin_extra only for Android builds, so the iOS data has no copy
(ScriptMapper lists the name with a null pointer, and the intro video stays black:
"Could not find material Hidden/VideoDecodeAndroid"). We build it from the iOS Hidden/VideoDecode
shader (already given GLES3 programs by patch_shaders.py): the Flip_RGBA_To_RGBA and
Flip_RGBASplit_To_RGBA passes, renamed to the Android pass names, with _MainTex sampled as the
SurfaceTexture's external OES image. Then the ScriptMapper entry in globalgamemanagers is pointed
at the new object.

Works on work/patched (run after patch_shaders.py; it calls this at the end). Game data: stays in work/.
usage: python -I tools/video_decode_android.py
"""
import copy
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_gles as sg  # noqa: E402
import unity_assets as ua  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Data")
OUT = os.path.join(ROOT, "work", "patched", "Data")
SRC_ID, NEW_ID = 16001, 16002  # Hidden/VideoDecode, our new object (unused id in unity_builtin_extra)
EXTRA_FILE_ID = 1  # globalgamemanagers' external #1 is Resources/unity_builtin_extra
PASSES = {"Flip_RGBA_To_RGBA": "RGBAExternal_To_RGBA", "Flip_RGBASplit_To_RGBA": "RGBASplitExternal_To_RGBA"}


def external_oes(code):
    """Sample _MainTex as an external image in the fragment stage. The Flip_ passes flip V for the
    iOS decoder output; the SurfaceTexture image is already the right way up, so drop the flip."""
    head, frag = code.split("#ifdef FRAGMENT\n", 1)
    flip = "fma(in_TEXCOORD0.xy, vec2(1.0, -1.0), vec2(0.0, 1.0))"
    assert flip in head
    head = head.replace(flip, "in_TEXCOORD0.xy")
    frag = frag.replace("#version 300 es\n", "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n", 1)
    assert "uniform highp sampler2D _MainTex;" in frag
    frag = frag.replace("uniform highp sampler2D _MainTex;", "uniform highp samplerExternalOES _MainTex;")
    return head + "#ifdef FRAGMENT\n" + frag


def build_shader(o):
    pi = o["platforms"].index(9)
    off, cl, dl = o["offsets"][pi][0], o["compressedLengths"][pi][0], o["decompressedLengths"][pi][0]
    segs = sg.read_blob(ua.lz4_block(bytes(o["compressedBlob"][off:off + cl]), dl))
    n = copy.deepcopy(o)
    pf = n["m_ParsedForm"]
    pf["m_Name"] = "Hidden/VideoDecodeAndroid"
    pf["m_PropInfo"]["m_Props"] = [p for p in pf["m_PropInfo"]["m_Props"] if p["m_Name"] == "_MainTex"]
    ss = pf["m_SubShaders"][0]
    new_segs, passes = [], []
    for ps in ss["m_Passes"]:
        name = ps["m_State"]["m_Name"]
        if name not in PASSES:
            continue
        ps["m_State"]["m_Name"] = PASSES[name]
        remap = {}
        for stage in ("progVertex", "progFragment"):
            subs = [sp for sp in ps[stage]["m_SubPrograms"] if sp["m_GpuProgramType"] == sg.GLES3]
            for sp in subs:
                b = sp["m_BlobIndex"]
                if b not in remap:
                    seg = segs[b]
                    if stage == "progVertex":
                        p = sg.parse_segment(seg)
                        code = external_oes(p["code"].decode()).encode()
                        cb = [c for c in p["cbs"] if c["name"] == "$Globals"]
                        seg = sg.build_segment(sg.GLES3, p["global_kw"], p["local_kw"], code, p["source_map"],
                                               [{"name": "", "size": 0, "params": []}] + cb,
                                               [r for r in p["res"] if r[0] == "_MainTex"])
                    remap[b] = len(new_segs)
                    new_segs.append(seg)
                sp["m_BlobIndex"] = remap[b]
                sp["m_TextureParams"] = [t for t in sp["m_TextureParams"] if t["m_Index"] == 0]
            ps[stage]["m_SubPrograms"] = subs
        passes.append(ps)
    assert len(passes) == 2, [p["m_State"]["m_Name"] for p in ss["m_Passes"]]
    ss["m_Passes"] = passes
    blob = sg.write_blob(new_segs)
    comp = ua.lz4_compress(blob)
    n["platforms"] = [9]
    n["offsets"], n["compressedLengths"], n["decompressedLengths"] = [[0]], [[len(comp)]], [[len(blob)]]
    n["compressedBlob"] = comp
    for k in ("m_Dependencies",):
        if k in n:
            n[k] = []
    return n


def patch_script_mapper(data):
    """Point ScriptMapper's 'Hidden/VideoDecodeAndroid' entry at (unity_builtin_extra, NEW_ID)."""
    sf = ua.SerializedFile(data)
    out = bytearray(data)
    for pid, st, sz, tidx in sf.objects:
        if sf.types[tidx][0] != 94:
            continue
        n = struct.unpack_from("<i", data, st)[0]
        p = st + 4
        for _ in range(n):
            pptr = p
            p += 12
            ln = struct.unpack_from("<i", data, p)[0]
            name = data[p + 4:p + 4 + ln].decode()
            p = (p + 4 + ln + 3) & ~3
            if name == "Hidden/VideoDecodeAndroid":
                struct.pack_into("<iq", out, pptr, EXTRA_FILE_ID, NEW_ID)
                return bytes(out)
    raise SystemExit("ScriptMapper entry not found")


def main(tree=None):
    if tree is None:
        import patch_shaders
        tree = patch_shaders.shader_tree()
    path = os.path.join(OUT, "Resources", "unity_builtin_extra")
    sf = ua.SerializedFile(open(path, "rb").read())
    src = [x for x in sf.objects_of(48) if x[0] == SRC_ID]
    if not src or 9 not in sf.read(src[0][1], src[0][2], src[0][3] or tree)["platforms"]:
        raise SystemExit("run patch_shaders.py first (Hidden/VideoDecode needs its GLES3 programs)")
    _, st, sz, t = src[0]
    new = ua.write_object(t or tree, build_shader(sf.read(st, sz, t or tree)))
    tidx = next(i for i, (c, _) in enumerate(sf.types) if c == 48)
    if any(x[0] == NEW_ID for x in sf.objects):
        data = ua.rebuild_serialized(sf, {NEW_ID: new})
    else:
        data = ua.rebuild_serialized(sf, {}, {NEW_ID: (tidx, new)})
    check = ua.SerializedFile(data)
    o = [check.read(s, z, tt or tree) for p, s, z, tt in check.objects_of(48) if p == NEW_ID][0]
    assert o["m_ParsedForm"]["m_Name"] == "Hidden/VideoDecodeAndroid"
    open(path, "wb").write(data)
    ggm = patch_script_mapper(open(os.path.join(DATA, "globalgamemanagers"), "rb").read())
    open(os.path.join(OUT, "globalgamemanagers"), "wb").write(ggm)
    print("added Hidden/VideoDecodeAndroid to unity_builtin_extra and ScriptMapper")


if __name__ == "__main__":
    main()
