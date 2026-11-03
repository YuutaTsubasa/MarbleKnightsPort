"""Add GLES3 programs to every Shader in the iOS data.

Inputs:  work/ipa/.../Data (player data) and work/raw-android (StreamingAssets).
Outputs: work/patched/Data/<file> and work/patched/Raw/<bundle> for every file that
changed, plus work/patched/changed.txt. Game data: stays in work/.

usage: python -I tools/patch_shaders.py [--validate] [--only SUBSTRING]
"""
import collections
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_gles as sg  # noqa: E402
import unity_assets as ua  # noqa: E402
import video_decode_android  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "work", "ipa", "Payload", "reprovisioned.app", "Data")
RAW = os.path.join(ROOT, "work", "raw-android")
OUT = os.path.join(ROOT, "work", "patched")
ANDROID_DEFAULT_RES = os.path.join(ROOT, "work", "unity", "AndroidPlayer", "Data", "unity default resources")
GLSLANG = shutil.which("glslangValidator") or r"C:\VulkanSDK\1.4.363.0\Bin\glslangValidator.exe"
DATA_FILES = ["globalgamemanagers.assets", "level0", "resources.assets", "sharedassets0.assets",
              os.path.join("Resources", "unity_builtin_extra")]


def shader_tree():
    b = ua.read_bundle(os.path.join(RAW, "AssetBundles", "android", "materials", "catsci", "m_hero_catsci_01"))
    return [t for c, t in ua.SerializedFile(list(b.values())[0]).types if c == 48][0]


def patch_serialized(data, tree, stats, programs):
    sf = ua.SerializedFile(data)
    rep = {}
    for pid, st, sz, t in sf.objects_of(48):
        t = t or tree
        o = sf.read(st, sz, t)
        if sg.add_gles3(o, stats):
            rep[pid] = ua.write_object(t, o)
            if programs is not None:
                pi = o["platforms"].index(9)
                blob = ua.lz4_block(bytes(o["compressedBlob"][o["offsets"][pi][0]:]), o["decompressedLengths"][pi][0])
                for seg in sg.read_blob(blob):
                    code = sg.parse_segment(seg)["code"]
                    if code:
                        programs[hashlib.sha1(code).hexdigest()[:16]] = code
    return ua.rebuild_serialized(sf, rep) if rep else None


def validate(programs):
    tmp = os.path.join(OUT, "validate")
    os.makedirs(tmp, exist_ok=True)

    def one(item):
        h, code = item
        text = code.decode()
        vs = text[text.index("#ifdef VERTEX\n") + 14:text.index("\n#endif\n#ifdef FRAGMENT")]
        fs = text[text.index("#ifdef FRAGMENT\n") + 16:text.rindex("\n#endif")]
        a, b = os.path.join(tmp, h + ".vert"), os.path.join(tmp, h + ".frag")
        open(a, "w").write(vs)
        open(b, "w").write(fs)
        r = subprocess.run([GLSLANG, "-l", a, b], capture_output=True, text=True)
        err = [l for l in r.stdout.splitlines() if l.startswith("ERROR")]
        return h, r.returncode == 0, err[0] if err else ""

    with concurrent.futures.ThreadPoolExecutor(16) as ex:
        res = list(ex.map(one, programs.items()))
    bad = [r for r in res if not r[1]]
    print(f"validated {len(res)} combined programs, {len(bad)} failed to compile/link")
    cats = collections.Counter(re.sub(r"'[^']*'", "'_'", re.sub(r"ERROR: [^:]*:\d+: ", "", r[2]))[:100] for r in bad)
    for k, n in cats.most_common(10):
        print(f"{n:5d} {k}")
    return bad


def main():
    tree = shader_tree()
    stats = collections.Counter()
    programs = {} if "--validate" in sys.argv else None
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    changed = []
    os.makedirs(os.path.join(OUT, "Data", "Resources"), exist_ok=True)
    for name in DATA_FILES:
        if only and only not in name:
            continue
        nd = patch_serialized(open(os.path.join(DATA, name), "rb").read(), tree, stats, programs)
        if nd:
            open(os.path.join(OUT, "Data", name), "wb").write(nd)
            changed.append("Data/" + name.replace("\\", "/"))
    if not only or only in "Resources/unity_builtin_extra":
        video_decode_android.main(tree)
        changed.append("Data/globalgamemanagers")
    shutil.copy(ANDROID_DEFAULT_RES, os.path.join(OUT, "Data", "unity default resources"))
    changed.append("Data/unity default resources")
    for dp, _, fs in os.walk(os.path.join(RAW, "AssetBundles")):
        for f in fs:
            if f.endswith(".manifest"):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, RAW).replace("\\", "/")
            if only and only not in rel:
                continue
            files = ua.read_bundle(path)
            out, touched = {}, False
            for node, data in files.items():
                nd = None
                if not node.endswith((".resS", ".resource")):
                    nd = patch_serialized(data, tree, stats, programs)
                out[node] = nd or data
                touched |= nd is not None
            if touched:
                dst = os.path.join(OUT, "Raw", rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                ua.write_bundle(dst, out)
                changed.append("Raw/" + rel)
    with open(os.path.join(OUT, "changed.txt"), "w") as fp:
        fp.write("\n".join(changed) + "\n")
    print(dict(stats))
    print(len(changed), "files changed")
    if programs is not None:
        validate(programs)


if __name__ == "__main__":
    main()
