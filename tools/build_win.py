"""Assemble the private Windows test build in work/win/MarbleKnights/:
official Unity 2019.4.17f1 Windows player (IL2CPP, x64) + our GameAssembly.dll + the iOS Data folder.

Layout the Windows IL2CPP player expects:
  MarbleKnights.exe, UnityPlayer.dll, GameAssembly.dll
  MarbleKnights_Data/  (globalgamemanagers, level0, *.assets, Resources/unity default resources,
                        il2cpp_data/Metadata/global-metadata.dat, StreamingAssets -> junction)
Serialized files get the Windows build target (StandaloneWindows64 = 19) instead of iOS (9).
"unity default resources" (engine built-ins: shaders, fonts) comes from the Windows player, since
the iOS copy only has Metal programs. StreamingAssets (3.4 GB) is linked, not copied.

Everything here is game/Unity data: work/ only, never in Git, never uploaded.
usage: python -I tools/build_win.py [--gameassembly path\\to\\GameAssembly.dll]
"""
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work")
APP = os.path.join(WORK, "ipa", "Payload", "reprovisioned.app")
PLAYER = os.path.join(WORK, "unity", "winil2cpp", "$INSTDIR$_59_", "Variations", "win64_nondevelopment_il2cpp")
OUT = os.path.join(WORK, "win", "MarbleKnights")
DATA = os.path.join(OUT, "MarbleKnights_Data")
WIN64 = 19  # BuildTarget.StandaloneWindows64


def patch_platform(path, target):
    with open(path, "r+b") as f:
        head = f.read(64)
        if head[0x14:0x20].startswith(b"2019."):
            off = 0x14 + head[0x14:].index(b"\0") + 1
            if struct.unpack_from("<i", head, off)[0] == 9:
                f.seek(off)
                f.write(struct.pack("<i", target))
                return True
    return False


def main():
    os.makedirs(DATA, exist_ok=True)
    shutil.copy2(os.path.join(PLAYER, "WindowsPlayer.exe"), os.path.join(OUT, "MarbleKnights.exe"))
    for f in ("UnityPlayer.dll", "UnityCrashHandler64.exe"):
        shutil.copy2(os.path.join(PLAYER, f), os.path.join(OUT, f))
    src = os.path.join(APP, "Data")
    patched = 0
    for dirpath, dirs, files in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        if rel.split(os.sep)[0] in ("Raw", "Managed"):
            continue
        os.makedirs(os.path.join(DATA, rel), exist_ok=True)
        for name in files:
            s = os.path.join(dirpath, name)
            p = os.path.join(WORK, "patched", "Data", rel, name)  # GLES3-patched shaders etc.
            d = os.path.join(DATA, rel, name)
            shutil.copy2(p if os.path.exists(p) else s, d)
            if patch_platform(d, WIN64):
                patched += 1
    # engine built-ins from the Windows player
    shutil.copy2(os.path.join(PLAYER, "Data", "Resources", "unity default resources"),
                 os.path.join(DATA, "Resources", "unity default resources"))
    md = os.path.join(DATA, "il2cpp_data", "Metadata")
    os.makedirs(md, exist_ok=True)
    shutil.copy2(os.path.join(src, "Managed", "Metadata", "global-metadata.dat"), md)
    res = os.path.join(src, "Managed", "Resources")
    if os.path.isdir(res):
        shutil.copytree(res, os.path.join(DATA, "il2cpp_data", "Resources"), dirs_exist_ok=True)
    sa = os.path.join(DATA, "StreamingAssets")
    raw = os.path.join(WORK, "raw-android")
    if not os.path.exists(sa) and os.path.isdir(raw):
        subprocess.run(["cmd", "/c", "mklink", "/J", sa, raw], check=True, stdout=subprocess.DEVNULL)
    ga = sys.argv[sys.argv.index("--gameassembly") + 1] if "--gameassembly" in sys.argv else None
    if ga:
        shutil.copy2(ga, os.path.join(OUT, "GameAssembly.dll"))
    print(f"assembled {OUT}: platform patched in {patched} files" + (", GameAssembly.dll from " + ga if ga else ""))


if __name__ == "__main__":
    main()
