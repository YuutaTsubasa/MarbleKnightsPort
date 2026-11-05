"""Assemble the Windows release folder (no game data; LEGAL.md):

  MarbleKnightsLauncher.exe   our launcher window (loader/win/mk_launcher.c)
  MarbleKnights.exe           Unity's WindowsPlayer.exe with -force-gles30 (loader/win/launcher.c), no icon
  GameAssembly.dll            the recompiled game code (tools/build_win_ga.sh)
  UnityPlayer.dll, UnityCrashHandler64.exe, MarbleKnights_Data/Resources/unity default resources
                              the official Unity 2019.4.17f1 Windows player
  python/                     the embeddable Python 3.12 from python.org
  tools/                      prepare_win.py and the modules it uses
  bin/mk_native.dll           our PVRTC decoder and LZ4
  README.md, README.zh-TW.md, LICENSE, LEGAL.md, THIRD_PARTY.md

The player's IPA becomes MarbleKnights_Data when they press Prepare in the launcher.
usage: python -I tools/package_win.py [out dir]   (default out/MarbleKnightsRecompiled-win64)
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, "work", "win")
PLAYER = os.path.join(ROOT, "work", "unity", "winil2cpp", "$INSTDIR$_59_", "Variations", "win64_nondevelopment_il2cpp")
PYTHON = os.path.join(ROOT, "work", "third_party", "python-3.12.10")
TOOLS = ["prepare_win.py", "build_win.py", "prelink_win.py", "win_icon.py", "win_textures.py", "unity_assets.py",
         "patch_shaders.py", "shader_gles.py", "msl2glsl.py", "video_decode_android.py", "macho.py"]
DOCS = ["README.md", "README.zh-TW.md", "LICENSE", "LEGAL.md", "THIRD_PARTY.md"]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "MarbleKnightsRecompiled-win64")
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "MarbleKnights_Data", "Resources"))
    copies = [
        (os.path.join(W, "MarbleKnightsLauncher.exe"), "MarbleKnightsLauncher.exe"),
        (os.path.join(W, "launcher-plain.exe"), "MarbleKnights.exe"),
        (os.path.join(W, "GameAssembly.dll"), "GameAssembly.dll"),
        (os.path.join(PLAYER, "UnityPlayer.dll"), "UnityPlayer.dll"),
        (os.path.join(PLAYER, "UnityCrashHandler64.exe"), "UnityCrashHandler64.exe"),
        (os.path.join(PLAYER, "Data", "Resources", "unity default resources"),
         os.path.join("MarbleKnights_Data", "Resources", "unity default resources")),
        (os.path.join(W, "mk_native.dll"), os.path.join("bin", "mk_native.dll")),
    ]
    copies += [(os.path.join(ROOT, "tools", t), os.path.join("tools", t)) for t in TOOLS]
    copies += [(os.path.join(ROOT, d), d) for d in DOCS]
    for src, rel in copies:
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    shutil.copytree(PYTHON, os.path.join(out, "python"))
    check(out)
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(out) for f in fs)
    print(f"{out}: {size / 2**20:.0f} MB")


def check(out):
    """Nothing from the game: no IPA content names, no game data files, no icon in the player exe."""
    bad = []
    for d, _, fs in os.walk(out):
        for f in fs:
            low = f.lower()
            if low.endswith((".ipa", ".dat", ".assets", ".ress", ".bank", ".ico", ".png")) or low in ("globalgamemanagers", "level0", "image.bin"):
                bad.append(os.path.join(d, f))
    exe = open(os.path.join(out, "MarbleKnights.exe"), "rb").read()
    if b"\x89PNG" in exe:
        bad.append("MarbleKnights.exe carries an icon image")
    if bad:
        raise SystemExit("game data in the release folder: " + ", ".join(bad))


if __name__ == "__main__":
    main()
