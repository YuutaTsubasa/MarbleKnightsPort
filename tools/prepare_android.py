"""Turn the player's own Marble Knights IPA into an Android APK and the data folder to copy to the phone.

The release ships android/base.apk (the Unity Android player, our libil2cpp.so and start activity,
no game data; tools/build_apk.py --base). Everything from the game is added here, on the player's PC:

  android/MarbleKnights.apk      base.apk + UnityFramework (as libmkframework.so) + the iOS Data
                                 folder with GLES3 shader programs and Hidden/VideoDecodeAndroid
  android/MarbleKnights/Raw/     the StreamingAssets (AssetBundles with GLES3 shaders, FMOD banks);
                                 the player copies the MarbleKnights folder to the top of the phone's
                                 storage, where the game looks for it (loader/proxy/proxy.c)
  android/signing-key.json       the player's own signing key, made once and kept so later APKs
                                 install over the first one (tools/apk_sign.py)

Stages and the progress lines for the launcher work as in tools/prepare_win.py; finished stages are
recorded in android/work/prepare.json.

usage: python -I tools/prepare_android.py <ipa> <release folder> [--jobs N] [--lz4 dll]
"""
import collections
import concurrent.futures
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apk_sign  # noqa: E402
import prepare_win as pw  # noqa: E402
from prepare_win import say  # noqa: E402

STAGES = ["check", "extract", "player data", "asset bundles", "apk"]


def dirs(out):
    a = os.path.join(out, "android")
    return {"base": os.path.join(a, "base.apk"), "apk": os.path.join(a, "MarbleKnights.apk"),
            "raw": os.path.join(a, "MarbleKnights", "Raw"), "key": os.path.join(a, "signing-key.json"),
            "work": os.path.join(a, "work"), "data": os.path.join(a, "work", "Data")}


def stage_extract(ipa, d, tmp):
    """The StreamingAssets, with their iOS names (the game asks for AssetBundles/ios)."""
    todo = [(n, os.path.join(d["raw"], *n[len(ipa.app + "Data/Raw/"):].split("/"))) for n in ipa.members("Data/Raw/")]
    total = sum(ipa.zip.getinfo(n).file_size for n, _ in todo)
    done = 0
    for n, dst in todo:
        size = ipa.zip.getinfo(n).file_size
        if not (os.path.exists(dst) and os.path.getsize(dst) == size):
            ipa.extract(n, dst)
        done += size
        say(f"@progress {done} {total}")


def shader_tree(raw):
    import unity_assets as ua
    b = ua.read_bundle(os.path.join(raw, *pw.SHADER_BUNDLE.split("/")))
    return [t for c, t in ua.SerializedFile(list(b.values())[0]).types if c == 48][0]


def stage_player_data(ipa, d, tmp, opts):
    """Data/ from the IPA afresh (60 MB), shaders given GLES3 programs, plus the Android video shader."""
    import patch_shaders
    import unity_assets as ua
    import video_decode_android
    shutil.rmtree(d["data"], ignore_errors=True)
    tree = shader_tree(d["raw"])
    names = [n for n in ipa.members("Data/") if not n.startswith(ipa.app + "Data/Raw/")
             and n != ipa.app + "Data/unity default resources"]  # the base APK has the Android player's copy
    for k, n in enumerate(names):
        rel = n[len(ipa.app + "Data/"):]
        dst = os.path.join(d["data"], *rel.split("/"))
        ipa.extract(n, dst)
        if not rel.startswith("Managed/") and not rel.endswith((".resS", ".resource", ".config")):
            blob = open(dst, "rb").read()
            try:
                ua.SerializedFile(blob)
            except Exception:
                blob = None
            nd = blob and patch_shaders.patch_serialized(blob, tree, collections.Counter(), None)
            if nd:
                open(dst, "wb").write(nd)
        say(f"@progress {k + 1} {len(names) + 1}")
    video_decode_android.main(tree, d["data"], d["data"])
    say(f"@progress {len(names) + 1} {len(names) + 1}")


_tree = {}


def worker_init(lz4, tree):
    pw.use_lz4(lz4)
    _tree["shader"] = tree


def convert_bundle(path):
    import patch_shaders
    import unity_assets as ua
    entries = ua.read_bundle(path)
    out, touched = dict(entries), False
    for name, data in entries.items():
        if name.endswith((".resS", ".resource")):
            continue
        nd = patch_shaders.patch_serialized(data, _tree["shader"], collections.Counter(), None)
        if nd:
            out[name], touched = nd, True
    # Android's VideoPlayer opens a clip only when it sits uncompressed in the bundle (tools/repack_videos.py)
    video = "/videos/" in path.replace("\\", "/")
    if touched or video:
        ua.write_bundle(path + ".tmp", out, compress=not video)
        os.replace(path + ".tmp", path)
    return touched


def stage_asset_bundles(ipa, d, tmp, opts):
    tree = shader_tree(d["raw"])
    paths = list(pw.bundles(d["raw"]))
    done_list = os.path.join(d["work"], "bundles-done.txt")
    done = set(open(done_list, encoding="utf-8").read().splitlines()) if os.path.exists(done_list) else set()
    todo = [p for p in paths if os.path.relpath(p, d["raw"]) not in done]
    say(f"{len(paths)} bundles, {len(todo)} to convert")
    with open(done_list, "a", encoding="utf-8") as log, concurrent.futures.ProcessPoolExecutor(
            opts["jobs"], initializer=worker_init, initargs=(opts["lz4"], tree)) as ex:
        n = len(paths) - len(todo)
        for p, _ in zip(todo, ex.map(convert_bundle, todo)):
            log.write(os.path.relpath(p, d["raw"]) + "\n")
            log.flush()
            n += 1
            say(f"@progress {n} {len(paths)}")


def stage_apk(ipa, d, tmp, opts):
    if not os.path.exists(d["base"]):
        raise SystemExit("@error android/base.apk is missing from the release folder")
    uf = os.path.join(tmp, "UnityFramework")
    ipa.extract(ipa.app + "Frameworks/UnityFramework.framework/UnityFramework", uf)
    add = [("lib/arm64-v8a/libmkframework.so", uf, False)]
    for dp, _, fs in os.walk(d["data"]):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            # Unity reads assets by offset from the APK: keep them uncompressed
            add.append(("assets/bin/Data/" + os.path.relpath(p, d["data"]).replace("\\", "/"), p, True))
    say(f"@progress 1 3")
    unsigned = os.path.join(tmp, "unsigned.apk")
    apk_sign.write_apk(unsigned, d["base"], add)
    say(f"@progress 2 3")
    key = apk_sign.load_key(d["key"])
    apk_sign.sign(unsigned, d["apk"], key)
    say(f"@progress 3 3")
    say(f"{d['apk']}: {os.path.getsize(d['apk']) / 2**20:.0f} MB")


def main():
    args = sys.argv[1:]
    opts = {"jobs": max(1, (os.cpu_count() or 2) - 1), "lz4": None}
    for key in ("--jobs", "--lz4"):
        if key in args:
            i = args.index(key)
            opts[key[2:]] = int(args[i + 1]) if key == "--jobs" else args[i + 1]
            del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    pw.use_lz4(opts["lz4"])
    ipa = pw.Ipa(args[0])
    d = dirs(args[1])
    os.makedirs(d["work"], exist_ok=True)
    state_path = os.path.join(d["work"], "prepare.json")
    state = json.load(open(state_path)) if os.path.exists(state_path) else {"done": []}
    funcs = {"check": lambda i, d, t, _: pw.stage_check(i, d, t), "extract": lambda i, d, t, _: stage_extract(i, d, t),
             "player data": stage_player_data, "asset bundles": stage_asset_bundles, "apk": stage_apk}
    tmp = tempfile.mkdtemp(prefix="mk-prepare-", dir=d["work"])
    try:
        for k, name in enumerate(STAGES):
            say(f"@stage {k + 1} {len(STAGES)} {name}")
            if name in state["done"] and name != "check":
                say("already done")
                continue
            funcs[name](ipa, d, tmp, opts)
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
