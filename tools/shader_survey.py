"""Survey every Shader in the iOS data: names, platforms, subprogram counts,
unique Metal programs, and which MSL constructs they use.

Writes work/shaders/survey.json and dumps each unique Metal program to
work/shaders/msl/<hash>.metal (game data: stays in work/).
usage: python -I tools/shader_survey.py
"""
import collections
import hashlib
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_assets as ua  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Data")
RAW = os.path.join(ROOT, "work", "raw-android")
OUT = os.path.join(ROOT, "work", "shaders")

FEATURES = {
    "sample": r"\.sample\(", "sample_bias": r"bias\(", "sample_level": r"level\(", "sample_grad": r"gradient2d\(",
    "sample_compare": r"sample_compare\(", "read": r"\.read\(", "texturecube": r"texturecube<",
    "texture3d": r"texture3d<", "depth2d": r"depth2d<", "texture2d_array": r"texture2d_array<",
    "half": r"\bhalf[234]?\b", "int": r"\bint[234]?\b", "uint": r"\buint[234]?\b", "as_type": r"as_type<",
    "dfdx": r"\bdfd[xy]\(", "discard": r"discard_fragment", "front_facing": r"front_facing",
    "vertex_id": r"vertex_id", "instance_id": r"instance_id", "array_of_structs": r"\[\[ buffer\(\d+\) \]\],?\n.*\*",
    "fb_fetch": r"\[\[ color\(\d+\) \]\]\s*,", "precise": r"precise::", "fast": r"fast::", "bool": r"\bbool[234]?\b",
    "loops": r"\bfor \(|\bwhile \(", "switch": r"\bswitch\s*\(", "struct_buffers": r"device ",
}


def shaders_in_serialized(sf, shader_tree):
    for pid, st, sz, tree in sf.objects_of(48):
        tree = tree or shader_tree
        if not tree:
            continue
        yield sf.read(st, sz, tree)


def main():
    os.makedirs(os.path.join(OUT, "msl"), exist_ok=True)
    sources = []
    for name in ("globalgamemanagers", "globalgamemanagers.assets", "level0", "resources.assets", "sharedassets0.assets",
                 "unity default resources", os.path.join("Resources", "unity_builtin_extra")):
        sources.append(("Data/" + name.replace("\\", "/"), open(os.path.join(APP, name), "rb").read()))
    bundle_files = []
    for dp, _, fs in os.walk(os.path.join(RAW, "AssetBundles")):
        for f in fs:
            if not f.endswith(".manifest"):
                bundle_files.append(os.path.join(dp, f))

    shader_tree = None
    shaders = {}       # name -> info (first occurrence)
    occurrences = collections.Counter()
    programs = {}      # sha1 -> {"stage", "bytes", "shaders"}
    feats = collections.Counter()
    gpu_types = collections.Counter()

    def visit(origin, sf):
        nonlocal shader_tree
        for cid, tree in sf.types:
            if cid == 48 and tree and not shader_tree:
                shader_tree = tree
        for o in shaders_in_serialized(sf, shader_tree):
            pf = o["m_ParsedForm"]
            name = pf["m_Name"]
            occurrences[name] += 1
            if name in shaders:
                continue
            nsub = 0
            for ss in pf["m_SubShaders"]:
                for ps in ss["m_Passes"]:
                    for stage in ("progVertex", "progFragment", "progGeometry", "progHull", "progDomain"):
                        for sp in ps[stage]["m_SubPrograms"]:
                            nsub += 1
                            gpu_types[sp["m_GpuProgramType"]] += 1
            info = {"origin": origin, "platforms": o["platforms"], "subprograms": nsub, "unique_programs": 0}
            shaders[name] = info
            for pi, plat in enumerate(o["platforms"]):
                if plat != 14:
                    continue
                for tier, (off, clen, dlen) in enumerate(zip(o["offsets"][pi], o["compressedLengths"][pi],
                                                             o["decompressedLengths"][pi])):
                    if not dlen:
                        continue
                    blob = ua.lz4_block(o["compressedBlob"][off:off + clen], dlen)
                    n = struct.unpack_from("<i", blob, 0)[0]
                    for k in range(n):
                        so, sl = struct.unpack_from("<ii", blob, 4 + 8 * k)
                        seg = blob[so:so + sl]
                        i = seg.find(b"#include <metal_stdlib>")
                        if i < 0:
                            continue
                        code = seg[i:seg.index(b"\0", i)] if b"\0" in seg[i:] else seg[i:]
                        h = hashlib.sha1(code).hexdigest()[:16]
                        if h not in programs:
                            text = code.decode("utf-8", "replace")
                            stage = "vertex" if "vertex Mtl_VertexOut" in text or "\nvertex " in text else "fragment"
                            programs[h] = {"stage": stage, "bytes": len(code), "shaders": [name]}
                            with open(os.path.join(OUT, "msl", h + ".metal"), "w", encoding="utf-8") as f:
                                f.write(text)
                            for fk, rx in FEATURES.items():
                                if re.search(rx, text):
                                    feats[fk] += 1
                            info["unique_programs"] += 1
                        elif name not in programs[h]["shaders"]:
                            programs[h]["shaders"].append(name)

    for origin, data in sources:
        try:
            visit(origin, ua.SerializedFile(data, origin))
        except Exception as e:  # Data files without type trees are revisited after a tree is known
            print("skip", origin, e)
    for path in sorted(bundle_files):
        try:
            for node, data in ua.read_bundle(path).items():
                if node.endswith((".resS", ".resource")):
                    continue
                visit(os.path.relpath(path, RAW).replace("\\", "/"), ua.SerializedFile(data, node))
        except Exception as e:
            print("skip", path, e)
    # second pass for Data files now that a Shader type tree is known
    for origin, data in sources:
        try:
            visit(origin, ua.SerializedFile(data, origin))
        except Exception as e:
            print("skip again", origin, e)

    summary = {
        "shaders": len(shaders),
        "shader_occurrences": sum(occurrences.values()),
        "subprograms": sum(s["subprograms"] for s in shaders.values()),
        "unique_metal_programs": len(programs),
        "unique_vertex": sum(1 for p in programs.values() if p["stage"] == "vertex"),
        "unique_fragment": sum(1 for p in programs.values() if p["stage"] == "fragment"),
        "metal_source_bytes": sum(p["bytes"] for p in programs.values()),
        "gpu_program_types": dict(gpu_types),
        "msl_features (programs using)": dict(feats.most_common()),
        "largest_shaders": sorted(((s["unique_programs"], n) for n, s in shaders.items()), reverse=True)[:25],
    }
    with open(os.path.join(OUT, "survey.json"), "w", encoding="utf-8") as f:
        json.dump({"summary": summary, "shaders": shaders, "programs": programs}, f, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
