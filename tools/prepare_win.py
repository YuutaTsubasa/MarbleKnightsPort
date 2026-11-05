"""Turn the player's own Marble Knights IPA into the game data of a Windows release folder.

A release carries our launcher and runtime, the recompiled GameAssembly.dll and the official Unity
Windows player; everything from the game is made here, on the player's machine (LEGAL.md):

  MarbleKnights_Data/        the iOS Data folder: serialized files set to the Windows build target,
                             shaders given GLES3 programs, PVRTC textures decoded to RGBA32
  MarbleKnights_Data/StreamingAssets/   the AssetBundles and FMOD banks, converted the same way
  MarbleKnights_Data/il2cpp_data/       global-metadata.dat and the managed resources
  MarbleKnights_Data/mk/     UnityFramework's memory image for GameAssembly.dll (tools/prelink_win.py)
                             and the game's icon

The work is split into stages; finished ones are recorded in MarbleKnights_Data/mk/prepare.json, so
an interrupted run carries on where it stopped. Progress goes to stdout as lines the launcher reads:
  @stage <n> <count> <name>       @progress <done> <total>       @done       @error <message>
Other lines are log text.

usage: python -I tools/prepare_win.py <ipa> <release folder> [--jobs N] [--lz4 dll] [--pvrtc dll]
"""
import collections
import concurrent.futures
import hashlib
import json
import os
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_win  # noqa: E402
import prelink_win  # noqa: E402
import win_icon  # noqa: E402
import win_textures  # noqa: E402

# GameAssembly.dll is translated from exactly this UnityFramework (v2.3.1, build 760642)
UNITYFRAMEWORK_SHA256 = "499cbca42ac355a1896d0723932a246f3126f64e4a21b66c41ee05be3fc354f0"
STAGES = ["check", "extract", "player data", "asset bundles", "image", "icon"]
SHADER_BUNDLE = "AssetBundles/ios/materials/catsci/m_hero_catsci_01"  # any bundle with a Shader type tree


def say(*parts):
    print(*parts, flush=True)


class Ipa:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path)
        names = self.zip.namelist()
        apps = sorted({n.split("/")[1] for n in names if n.startswith("Payload/") and n.split("/")[1].endswith(".app")})
        uf = [a for a in apps if f"Payload/{a}/Frameworks/UnityFramework.framework/UnityFramework" in names]
        if not uf:
            raise SystemExit("@error not an IPA of a Unity game (no UnityFramework)")
        self.app = f"Payload/{uf[0]}/"
        self.names = [n for n in names if n.startswith(self.app) and not n.endswith("/")]

    def members(self, prefix):
        return [n for n in self.names if n.startswith(self.app + prefix)]

    def extract(self, name, dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with self.zip.open(name) as src, open(dst + ".tmp", "wb") as out:
            shutil.copyfileobj(src, out, 1 << 22)
        os.replace(dst + ".tmp", dst)


def sha256_member(ipa, name):
    h = hashlib.sha256()
    with ipa.zip.open(name) as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def stage_check(ipa, out, tmp):
    uf = ipa.app + "Frameworks/UnityFramework.framework/UnityFramework"
    got = sha256_member(ipa, uf)
    if got != UNITYFRAMEWORK_SHA256:
        raise SystemExit("@error this IPA is a different version of the game: Marble Knights 2.3.1 (build 760642) is needed")
    say("UnityFramework matches the version GameAssembly.dll was built from")


def target_of(rel):
    """IPA path under the .app -> path in the release folder (None: not needed)"""
    if rel.startswith("Data/Raw/"):
        return os.path.join("MarbleKnights_Data", "StreamingAssets", *rel[len("Data/Raw/"):].split("/"))
    if rel.startswith("Data/Managed/Metadata/"):
        return os.path.join("MarbleKnights_Data", "il2cpp_data", "Metadata", *rel[len("Data/Managed/Metadata/"):].split("/"))
    if rel.startswith("Data/Managed/Resources/"):
        return os.path.join("MarbleKnights_Data", "il2cpp_data", "Resources", *rel[len("Data/Managed/Resources/"):].split("/"))
    if rel.startswith("Data/Managed/"):
        return None
    if rel.startswith("Data/"):
        if rel == "Data/Resources/unity default resources":
            return None  # the Windows player's own copy (in the release) has the PC shaders
        return os.path.join("MarbleKnights_Data", *rel[len("Data/"):].split("/"))
    return None


def stage_extract(ipa, out, tmp):
    todo = [(n, target_of(n[len(ipa.app):])) for n in ipa.members("Data/")]
    todo = [(n, t) for n, t in todo if t]
    total = sum(ipa.zip.getinfo(n).file_size for n, _ in todo)
    done = 0
    for n, t in todo:
        dst = os.path.join(out, t)
        size = ipa.zip.getinfo(n).file_size
        if not (os.path.exists(dst) and os.path.getsize(dst) == size):
            ipa.extract(n, dst)
        done += size
        say(f"@progress {done} {total}")


def convert_serialized_bytes(data, shader_tree, texture_tree, pvrtc, res):
    """Shaders get GLES3 programs, PVRTC textures become RGBA32. res: the file's .resS/.resource
    data, updated in place. Returns new bytes or None."""
    import patch_shaders
    import unity_assets as ua
    nd = patch_shaders.patch_serialized(data, shader_tree, collections.Counter(), None) if shader_tree else None
    try:
        sf = ua.SerializedFile(nd or data)
    except (ValueError, Exception):  # not a serialized file
        return nd
    new, _ = win_textures.convert_serialized(pvrtc, sf, res, texture_tree)
    return new or nd


_worker = {}


def use_lz4(dll):
    import unity_assets as ua
    if dll:
        ua._LZ4_DLL = dll


def worker_init(lz4, pvrtc, shader_tree, texture_tree):
    use_lz4(lz4)
    _worker.update(pvrtc=win_textures.decoder(pvrtc), shader_tree=shader_tree, texture_tree=texture_tree)


def convert_bundle(path):
    import unity_assets as ua
    entries = ua.read_bundle(path)
    res = {k: d for k, d in entries.items() if k.endswith((".resS", ".resource"))}
    out, touched = dict(entries), False
    for name, data in entries.items():
        if name in res:
            continue
        nd = convert_serialized_bytes(data, _worker["shader_tree"], None, _worker["pvrtc"], res)
        if nd:
            out[name], touched = nd, True
    if not touched:
        return 0
    out.update(res)
    ua.write_bundle(path + ".tmp", out)
    os.replace(path + ".tmp", path)
    return 1


def bundles(sa):
    for dp, _, fs in os.walk(sa):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            if f.endswith((".manifest", ".tmp")):
                continue
            with open(p, "rb") as fh:
                if fh.read(8) == b"UnityFS\0":
                    yield p


def type_trees(sa):
    import unity_assets as ua
    b = ua.read_bundle(os.path.join(sa, *SHADER_BUNDLE.split("/")))
    shader = [t for c, t in ua.SerializedFile(list(b.values())[0]).types if c == 48][0]
    return shader, win_textures.texture_tree(sa)


def stage_player_data(ipa, out, tmp, opts):
    import unity_assets as ua
    data = os.path.join(out, "MarbleKnights_Data")
    shader_tree, texture_tree = type_trees(os.path.join(data, "StreamingAssets"))
    pvrtc = win_textures.decoder(opts["pvrtc"])
    files = []
    for dp, dirs, fs in os.walk(data):
        dirs[:] = [d for d in dirs if d not in ("StreamingAssets", "il2cpp_data", "mk")]
        files += [os.path.join(dp, f) for f in fs if not f.endswith((".resS", ".resource", ".config", ".tmp"))]
    for k, p in enumerate(sorted(files)):
        blob = open(p, "rb").read()
        try:
            ua.SerializedFile(blob)
        except Exception:
            say(f"@progress {k + 1} {len(files)}")
            continue
        res = {}
        for r in (p + ".resS", p + ".resource"):
            if os.path.exists(r):
                res[os.path.basename(r)] = open(r, "rb").read()
        before = dict(res)
        nd = convert_serialized_bytes(blob, shader_tree, texture_tree, pvrtc, res)
        if nd:
            for path, b in [(p, nd)] + [(os.path.join(os.path.dirname(p), r), b) for r, b in res.items() if b is not before[r]]:
                with open(path + ".tmp", "wb") as f:
                    f.write(b)
                os.replace(path + ".tmp", path)
        build_win.patch_platform(p, build_win.WIN64)
        say(f"@progress {k + 1} {len(files)}")


def stage_asset_bundles(ipa, out, tmp, opts):
    sa = os.path.join(out, "MarbleKnights_Data", "StreamingAssets")
    shader_tree, _ = type_trees(sa)
    paths = list(bundles(sa))
    done_list = os.path.join(out, "MarbleKnights_Data", "mk", "bundles-done.txt")
    done = set(open(done_list, encoding="utf-8").read().splitlines()) if os.path.exists(done_list) else set()
    todo = [p for p in paths if os.path.relpath(p, sa) not in done]
    say(f"{len(paths)} bundles, {len(todo)} to convert")
    with open(done_list, "a", encoding="utf-8") as log, concurrent.futures.ProcessPoolExecutor(
            opts["jobs"], initializer=worker_init, initargs=(opts["lz4"], opts["pvrtc"], shader_tree, None)) as ex:
        n = len(paths) - len(todo)
        for p, _ in zip(todo, ex.map(convert_bundle, todo)):
            log.write(os.path.relpath(p, sa) + "\n")
            log.flush()
            n += 1
            say(f"@progress {n} {len(paths)}")


def stage_image(ipa, out, tmp, opts):
    uf = os.path.join(tmp, "UnityFramework")
    ipa.extract(ipa.app + "Frameworks/UnityFramework.framework/UnityFramework", uf)
    prelink_win.main(uf, os.path.join(out, "MarbleKnights_Data", "mk"))
    os.remove(uf)


def stage_icon(ipa, out, tmp, opts):
    app = os.path.join(tmp, "app")
    for n in ipa.names:
        rel = n[len(ipa.app):]
        if "/" not in rel and rel.startswith("AppIcon") and rel.endswith(".png"):
            ipa.extract(n, os.path.join(app, rel))
    ico = os.path.join(out, "MarbleKnights_Data", "mk", "icon.ico")
    win_icon.main(app, ico)
    set_exe_icon(os.path.join(out, "MarbleKnights.exe"), ico)


def set_exe_icon(exe, ico):
    """Put the icon into the exe as icon group 103, the one Unity's player gives its window."""
    import ctypes
    import struct
    from ctypes import wintypes
    data = open(ico, "rb").read()
    _, _, count = struct.unpack_from("<HHH", data)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.BeginUpdateResourceW.restype = wintypes.HANDLE
    k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
    k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    h = k32.BeginUpdateResourceW(exe, False)
    if not h:
        raise OSError(ctypes.get_last_error(), "BeginUpdateResource")
    group = struct.pack("<HHH", 0, 1, count)
    for i in range(count):
        w, hh, colors, _, planes, bits, size, off = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        img = data[off:off + size]
        k32.UpdateResourceW(h, 3, 200 + i, 0, img, len(img))  # RT_ICON
        group += struct.pack("<BBBBHHIH", w, hh, colors, 0, planes, bits, size, 200 + i)
    k32.UpdateResourceW(h, 14, 103, 0, group, len(group))  # RT_GROUP_ICON
    if not k32.EndUpdateResourceW(h, False):
        raise OSError(ctypes.get_last_error(), "EndUpdateResource")


def main():
    args = [a for a in sys.argv[1:]]
    opts = {"jobs": max(1, (os.cpu_count() or 2) - 1), "lz4": None, "pvrtc": None}
    for key in ("--jobs", "--lz4", "--pvrtc"):
        if key in args:
            i = args.index(key)
            opts[key[2:]] = int(args[i + 1]) if key == "--jobs" else args[i + 1]
            del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    use_lz4(opts["lz4"])
    ipa_path, out = args
    ipa = Ipa(ipa_path)
    mk = os.path.join(out, "MarbleKnights_Data", "mk")
    os.makedirs(mk, exist_ok=True)
    state_path = os.path.join(mk, "prepare.json")
    state = json.load(open(state_path)) if os.path.exists(state_path) else {"done": []}
    funcs = {"check": lambda i, o, t, _: stage_check(i, o, t), "extract": lambda i, o, t, _: stage_extract(i, o, t),
             "player data": stage_player_data, "asset bundles": stage_asset_bundles, "image": stage_image, "icon": stage_icon}
    tmp = tempfile.mkdtemp(prefix="mk-prepare-", dir=mk)
    try:
        for k, name in enumerate(STAGES):
            say(f"@stage {k + 1} {len(STAGES)} {name}")
            if name in state["done"] and name != "check":
                say("already done")
                continue
            funcs[name](ipa, out, tmp, opts)
            state["done"].append(name)
            with open(state_path, "w") as f:
                json.dump(state, f)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    say("@done")


if __name__ == "__main__":
    try:
        main()
    except SystemExit as e:
        if isinstance(e.code, str) and e.code.startswith("@error"):
            say(e.code)
            sys.exit(1)
        raise
    except Exception as e:  # the launcher shows the message; the traceback goes to stderr
        say(f"@error {type(e).__name__}: {e}")
        raise
