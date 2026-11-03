"""Rewrite the video AssetBundles uncompressed: Android's VideoPlayer can only open
a clip that sits uncompressed inside a bundle file. Writes work/patched/Raw/... (game data).
usage: python -I tools/repack_videos.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_assets as ua  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "work", "raw-android", "AssetBundles", "android", "videos")
OUT = os.path.join(ROOT, "work", "patched", "Raw", "AssetBundles", "android", "videos")
for dp, _, fs in os.walk(SRC):
    for f in fs:
        if f.endswith(".manifest"):
            continue
        src = os.path.join(dp, f)
        dst = os.path.join(OUT, os.path.relpath(src, SRC))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        ua.write_bundle(dst, ua.read_bundle(src), compress=False)
        print(os.path.relpath(dst, ROOT), os.path.getsize(dst) >> 20, "MB")
