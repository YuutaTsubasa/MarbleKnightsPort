"""Add GLES3 (shader compiler platform 9) programs to Unity 2019.4 Shader
objects that only carry Metal (platform 14), by translating the Metal code with
msl2glsl and rebuilding Unity's blob and parsed-form parameter tables.

Blob segment layout (from LoadGpuProgramFromData in libunity.so):
  int magic(201806140), int gpuProgramType, int stats[4],
  strlist globalKeywords, strlist localKeywords, bytes code (aligned),
  int sourceMap, int nChannels, {int source, int target}*,
  int nCB, {str name, int size, int nParams, {str name, int type, rows, cols, isMatrix, arraySize, offset}*,
            int nStructs, ...}*,
  int nResources, {str name, int type, int index, int extra, [int dim if type == 0]}*
A GLES program keeps both stages in the vertex segment (#ifdef VERTEX / #ifdef FRAGMENT);
the fragment segment is empty.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msl2glsl  # noqa: E402
import unity_assets as ua  # noqa: E402

MAGIC = 201806140
GLES3 = 4
METAL_VS, METAL_FS = 23, 24
FATAL = ("device buffer", "struct member", "unknown parameter", "texture kind", "output attribute",
         "clip distance", "function constants", "no entry point")


# ---- segment (de)serialization ------------------------------------------------
class Reader:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def i(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def s(self):
        n = self.i()
        v = self.b[self.p:self.p + n].decode("utf-8")
        self.p = (self.p + n + 3) & ~3
        return v

    def raw(self):
        n = self.i()
        v = self.b[self.p:self.p + n]
        self.p = (self.p + n + 3) & ~3
        return v


def parse_segment(seg):
    r = Reader(seg)
    out = {"magic": r.i(), "type": r.i(), "stats": [r.i() for _ in range(4)]}
    out["global_kw"] = [r.s() for _ in range(r.i())]
    out["local_kw"] = [r.s() for _ in range(r.i())]
    out["code"] = r.raw()
    out["source_map"] = r.i()
    out["channels"] = [(r.i(), r.i()) for _ in range(r.i())]
    cbs = []
    for _ in range(r.i()):
        name, size = r.s(), r.i()
        params = [(r.s(), r.i(), r.i(), r.i(), r.i(), r.i(), r.i()) for _ in range(r.i())]
        nstruct = r.i()
        if nstruct:
            raise ValueError("struct params")
        cbs.append({"name": name, "size": size, "params": params})
    out["cbs"] = cbs
    res = []
    for _ in range(r.i()):
        name, typ, index, extra = r.s(), r.i(), r.i(), r.i()
        dim = r.i() if typ == 0 else None
        res.append((name, typ, index, extra, dim))
    out["res"] = res
    if r.p != len(seg):
        raise ValueError(f"segment parse ended at {r.p}/{len(seg)}")
    return out


class Writer:
    def __init__(self):
        self.b = bytearray()

    def i(self, v):
        self.b += struct.pack("<i", v)

    def s(self, v):
        raw = v.encode("utf-8") if isinstance(v, str) else v
        self.i(len(raw))
        self.b += raw
        while len(self.b) % 4:
            self.b.append(0)


def build_segment(ptype, global_kw, local_kw, code, source_map, cbs, res):
    w = Writer()
    w.i(MAGIC); w.i(ptype)
    for _ in range(4):
        w.i(0)
    w.i(len(global_kw))
    for k in global_kw:
        w.s(k)
    w.i(len(local_kw))
    for k in local_kw:
        w.s(k)
    w.s(code)
    w.i(source_map)
    w.i(0)  # GLES binds attributes by name
    w.i(len(cbs))
    for cb in cbs:
        w.s(cb["name"]); w.i(cb["size"]); w.i(len(cb["params"]))
        for p in cb["params"]:
            w.s(p[0])
            for v in p[1:]:
                w.i(v)
        w.i(0)
    w.i(len(res))
    for name, typ, index, extra, dim in res:
        w.s(name); w.i(typ); w.i(index); w.i(extra)
        if typ == 0:
            w.i(dim)
    return bytes(w.b)


def read_blob(blob):
    n = struct.unpack_from("<i", blob, 0)[0]
    segs = []
    for k in range(n):
        so, sl, _ = struct.unpack_from("<iii", blob, 4 + 12 * k)
        segs.append(blob[so:so + sl])
    return segs


def write_blob(segs):
    head = 4 + 12 * len(segs)
    out = bytearray(struct.pack("<i", len(segs)))
    off = head
    for s in segs:
        out += struct.pack("<iii", off, len(s), 0)
        off += len(s)
    for s in segs:
        out += s
    return bytes(out)


# ---- parameter layout for GLES $Globals -----------------------------------------
def gles_globals(param_lists):
    """Merge Metal cbuffer params (all stages) into one float-typed $Globals layout."""
    seen, params, off = set(), [], 0
    for plist in param_lists:
        for name, typ, rows, cols, is_mat, arr, _ in plist:
            if name in seen:
                continue
            seen.add(name)
            t = 0 if typ == 3 else typ  # half -> float
            count = max(arr, 1)
            size = (rows * 16 if is_mat else 16) * count
            params.append((name, t, rows, cols, is_mat, arr, off))
            off += size
    return {"name": "$Globals", "size": off, "params": params}


def translate_pair(vs_code, fs_code):
    vs = msl2glsl.translate(vs_code.decode("utf-8", "replace"))
    fs = msl2glsl.translate(fs_code.decode("utf-8", "replace"))
    bad = [p for p in vs.problems + fs.problems if p.startswith(FATAL)]
    if bad or vs.stage != "vertex" or fs.stage != "fragment":
        return None, bad or ["stage mismatch"]
    code = "#ifdef VERTEX\n" + vs.glsl + "\n#endif\n#ifdef FRAGMENT\n" + fs.glsl + "\n#endif\n"
    return code, []


def metal_code(seg):
    """The MSL text inside a Metal segment's code bytes."""
    i = seg["code"].find(b"#include <metal_stdlib>")
    return seg["code"][i:].split(b"\0")[0] if i >= 0 else b""


# ---- Shader object rewrite ----------------------------------------------------------
def add_gles3(o, stats):
    """Mutate a decoded Shader object in place. Returns False if nothing could be added."""
    if 9 in o["platforms"] or 14 not in o["platforms"]:
        return False
    pi = o["platforms"].index(14)
    if len(o["offsets"][pi]) != 1:
        stats["multi-segment blob"] += 1
        return False
    off, cl, dl = o["offsets"][pi][0], o["compressedLengths"][pi][0], o["decompressedLengths"][pi][0]
    metal = read_blob(ua.lz4_block(bytes(o["compressedBlob"][off:off + cl]), dl))
    parsed = {}

    def mseg(i):
        if i not in parsed:
            parsed[i] = parse_segment(metal[i])
        return parsed[i]

    segs, seg_index = [], {}
    added = 0
    for ss in o["m_ParsedForm"]["m_SubShaders"]:
        for ps in ss["m_Passes"]:
            names = ps["m_NameIndices"]
            name_ix = {d["first"]: d["second"] for d in names}

            def nix(n):
                if n not in name_ix:
                    name_ix[n] = max([d["second"] for d in names] + [-1]) + 1
                    names.append({"first": n, "second": name_ix[n]})
                return name_ix[n]

            vsl = ps["progVertex"]["m_SubPrograms"]
            fsl = ps["progFragment"]["m_SubPrograms"]
            vss = [sp for sp in vsl if sp["m_GpuProgramType"] == METAL_VS]
            fss = [sp for sp in fsl if sp["m_GpuProgramType"] == METAL_FS]
            new_v, new_f = [], []
            for v in vss:
                vk = set(v["m_GlobalKeywordIndices"]) | {("l", x) for x in v["m_LocalKeywordIndices"]}
                best = None
                for f in fss:
                    if f["m_ShaderHardwareTier"] != v["m_ShaderHardwareTier"]:
                        continue
                    fk = set(f["m_GlobalKeywordIndices"]) | {("l", x) for x in f["m_LocalKeywordIndices"]}
                    if fk <= vk and (best is None or len(fk) > best[0]):
                        best = (len(fk), f)
                if not best:
                    stats["no matching fragment"] += 1
                    continue
                f = best[1]
                key = (v["m_BlobIndex"], f["m_BlobIndex"])
                if key not in seg_index:
                    try:
                        vseg, fseg = mseg(v["m_BlobIndex"]), mseg(f["m_BlobIndex"])
                        code, bad = translate_pair(metal_code(vseg), metal_code(fseg))
                    except ValueError as e:  # struct params (GPU instancing) are not handled yet
                        code, bad = None, [str(e)]
                    if code is None:
                        stats["untranslated: " + bad[0].split(":")[0]] += 1
                        seg_index[key] = None
                    else:
                        cb = gles_globals([c["params"] for c in vseg["cbs"]] + [c["params"] for c in fseg["cbs"]])
                        tex, units = [], {}
                        for r in vseg["res"] + fseg["res"]:
                            if r[1] == 0 and r[0] not in units:
                                units[r[0]] = len(units)
                                tex.append((r[0], 0, units[r[0]], units[r[0]], r[4]))
                        vblob = build_segment(GLES3, vseg["global_kw"], vseg["local_kw"], code.encode(),
                                              vseg["source_map"], [{"name": "", "size": 0, "params": []}, cb], tex)
                        fblob = build_segment(GLES3, vseg["global_kw"], vseg["local_kw"], b"", 0,
                                              [{"name": "", "size": 0, "params": []}], [])
                        segs.append(vblob)
                        segs.append(fblob)
                        seg_index[key] = (len(segs) - 2, len(segs) - 1, cb, tex, vseg, fseg)
                if seg_index[key] is None:
                    continue
                vi, fi, cb, tex, vseg, fseg = seg_index[key]
                req = v["m_ShaderRequirements"] | f["m_ShaderRequirements"]
                ventry = dict(v)
                ventry.update({
                    "m_BlobIndex": vi, "m_GpuProgramType": GLES3,
                    "m_Channels": {"m_Channels": [], "m_SourceMap": vseg["source_map"]},
                    "m_VectorParams": [], "m_MatrixParams": [], "m_BufferParams": [], "m_UAVParams": [],
                    "m_Samplers": [], "m_ConstantBufferBindings": [], "m_ShaderRequirements": req,
                    "m_TextureParams": [{"m_NameIndex": nix(t[0]), "m_Index": t[2], "m_SamplerIndex": t[3],
                                         "m_MultiSampled": False, "m_Dim": t[4] >> 1} for t in tex],
                    "m_ConstantBuffers": [{
                        "m_NameIndex": nix("$Globals"),
                        "m_MatrixParams": [{"m_NameIndex": nix(p[0]), "m_Index": p[6], "m_ArraySize": p[5],
                                            "m_Type": p[1], "m_RowCount": p[2]} for p in cb["params"] if p[4]],
                        "m_VectorParams": [{"m_NameIndex": nix(p[0]), "m_Index": p[6], "m_ArraySize": p[5],
                                            "m_Type": p[1], "m_Dim": p[3]} for p in cb["params"] if not p[4]],
                        "m_StructParams": [], "m_Size": cb["size"]}],
                })
                fentry = dict(v)
                fentry.update({
                    "m_BlobIndex": fi, "m_GpuProgramType": GLES3,
                    "m_Channels": {"m_Channels": [], "m_SourceMap": 0},
                    "m_VectorParams": [], "m_MatrixParams": [], "m_TextureParams": [], "m_BufferParams": [],
                    "m_ConstantBuffers": [], "m_ConstantBufferBindings": [], "m_UAVParams": [], "m_Samplers": [],
                    "m_ShaderRequirements": req,
                })
                new_v.append(ventry)
                new_f.append(fentry)
                added += 1
            vsl.extend(new_v)
            fsl.extend(new_f)
    if not added:
        return False
    blob = write_blob(segs)
    comp = ua.lz4_compress(blob)
    o["platforms"].append(9)
    o["offsets"].append([len(o["compressedBlob"])])
    o["compressedLengths"].append([len(comp)])
    o["decompressedLengths"].append([len(blob)])
    o["compressedBlob"] = bytes(o["compressedBlob"]) + comp
    stats["programs added"] += added
    stats["shaders patched"] += 1
    return True
