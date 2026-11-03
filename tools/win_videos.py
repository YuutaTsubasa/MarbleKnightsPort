"""Windows build: re-encode the H.264 VideoClips (Apple Arcade / WayForward logos, intro) as VP8 WebM.

The Windows player decodes H.264 with Media Foundation, which it only uses under Direct3D; this
build runs on OpenGL ES 3 (the only shaders the data has), so the videos stayed black ("Microsoft
Media Foundation video decoding to texture disabled" in Player.log). Unity's own VP8 decoder works
with any graphics API, so each clip is transcoded with ffmpeg (VP8 without audio, at most 1080 lines)
and its VideoClip object updated (size, path, dimensions).

usage: python -I tools/win_videos.py [MarbleKnights_Data dir]
Bundles are rewritten in place (the new file replaces the hard link to the Android data); the
player data files go to work/win/patched-data too, like tools/win_textures.py. Already converted
clips (WebM data) are skipped. Reads and writes game data, so everything stays in work/.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_assets as u  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "work", "win", "MarbleKnights", "MarbleKnights_Data")
OVERLAY = os.path.join(ROOT, "work", "win", "patched-data")
VIDEOCLIP = 329
EBML = b"\x1a\x45\xdf\xa3"
MAX_H = 1080


def transcode(raw, v):
    """mp4 bytes -> (webm bytes, width, height)"""
    w, h = v["Width"], v["Height"]
    if h > MAX_H:
        w, h = (w * MAX_H // h) & ~1, MAX_H
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "in.mp4"), os.path.join(tmp, "out.webm")
        with open(src, "wb") as f:
            f.write(raw)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", f"scale={w}:{h}", "-c:v", "libvpx",
                        "-b:v", "6M", "-crf", "8", "-deadline", "good", "-cpu-used", "2", "-auto-alt-ref", "0",
                        "-an", dst], check=True)
        return open(dst, "rb").read(), w, h


def convert_serialized(sf, res, tree=None):
    """res: {resource name: bytes}, updated in place. Returns (new file bytes or None, count)."""
    replaced = {}
    for pid, st, sz, t in sf.objects_of(VIDEOCLIP):
        t = t or tree
        v = sf.read(st, sz, t)
        er = v["m_ExternalResources"]
        name = er["m_Source"].split("/")[-1]
        raw = res[name][er["m_Offset"]:er["m_Offset"] + er["m_Size"]]
        if raw[:4] == EBML:
            continue
        webm, w, h = transcode(raw, v)
        blob = bytearray(res[name])
        if er["m_Offset"] == 0 and er["m_Size"] == len(blob):
            blob = bytearray()  # the resource holds only this clip
        while len(blob) % 16:
            blob.append(0)
        er["m_Offset"], er["m_Size"] = len(blob), len(webm)
        res[name] = bytes(blob + webm)
        v["m_OriginalPath"] = os.path.splitext(v["m_OriginalPath"])[0] + ".webm"
        v["Width"], v["Height"], v["m_ProxyWidth"], v["m_ProxyHeight"] = w, h, w, h
        # no audio track: the game plays with Unity audio off (FMOD), and a clip with audio never
        # advances (VideoPlayer.time stays 0, the boot sequence waits forever)
        v["m_AudioChannelCount"], v["m_AudioSampleRate"], v["m_AudioLanguage"] = [], [], []
        replaced[pid] = u.write_object(t, v, sf.big)
        print(f"  {v['m_Name']}: {len(raw) // 1024} KiB H.264 -> {len(webm) // 1024} KiB VP8 {w}x{h}")
    return (u.rebuild_serialized(sf, replaced) if replaced else None), len(replaced)


def clip_tree(sa):
    for dirpath, _, names in os.walk(sa):
        for nm in names:
            try:
                entries = u.read_bundle(os.path.join(dirpath, nm))
            except (ValueError, OSError):
                continue
            for name, data in entries.items():
                if name.endswith((".resS", ".resource")):
                    continue
                for _, _, _, t in u.SerializedFile(data).objects_of(VIDEOCLIP):
                    if t:
                        return t
    raise SystemExit("no VideoClip type tree found")


def convert_bundle(path):
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
        new, k = convert_serialized(sf, res)
        if new:
            out[name] = new
            n += k
    if n:
        out.update(res)
        u.write_bundle(path + ".tmp", out, compress=False)  # the player streams video from the archive: no LZ4
        os.replace(path + ".tmp", path)  # replaces the hard link; the Android original is untouched
    return n


def convert_loose(data_dir, tree):
    os.makedirs(OVERLAY, exist_ok=True)
    total = 0
    for nm in sorted(os.listdir(data_dir)):
        p = os.path.join(data_dir, nm)
        if not nm.endswith(".assets") or not os.path.isfile(p):
            continue
        sf = u.SerializedFile(open(p, "rb").read())
        if not any(True for _ in sf.objects_of(VIDEOCLIP)):
            continue
        r = os.path.splitext(nm)[0] + ".resource"
        res = {r: open(os.path.join(data_dir, r), "rb").read()}
        print(nm)
        new, k = convert_serialized(sf, res, tree)
        if not new:
            continue
        for name, blob in ((nm, new), (r, res[r])):
            for d in (OVERLAY, data_dir):
                with open(os.path.join(d, name + ".tmp"), "wb") as f:
                    f.write(blob)
                os.replace(os.path.join(d, name + ".tmp"), os.path.join(d, name))
        total += k
    return total


def main():
    data_dir = sys.argv[1] if len(sys.argv) > 1 else DATA
    sa = os.path.join(data_dir, "StreamingAssets")
    tree = clip_tree(sa)  # before the bundles change (any clip carries the same tree)
    total = 0
    for dirpath, _, names in os.walk(sa):
        for nm in sorted(names):
            path = os.path.join(dirpath, nm)
            if nm.endswith((".manifest", ".tmp")):
                continue
            with open(path, "rb") as f:
                if f.read(8) != b"UnityFS\0":
                    continue
            entries = u.read_bundle(path)
            if not any(k.endswith(".resource") for k in entries):
                continue
            print(os.path.relpath(path, sa))
            k = convert_bundle(path)
            if not k and os.path.getsize(path) < sum(len(d) for d in entries.values()):
                u.write_bundle(path + ".tmp", entries, compress=False)  # converted by an earlier version (LZ4)
                os.replace(path + ".tmp", path)
                print("  rewritten uncompressed")
            total += k
    total += convert_loose(data_dir, tree)
    print(f"{total} video clips converted to VP8")


if __name__ == "__main__":
    main()
