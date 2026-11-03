"""Package a private test APK: official Unity 2019.4.17f1 Android player +
our proxy libil2cpp.so + the iOS UnityFramework + the iOS Data folder.

Everything read and written here is game/Unity data and stays under work/ and
out/ (gitignored). Never upload the result anywhere.

usage: python -I tools/build_apk.py [--with-raw] [--patch-platform]
"""
import os
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work")
APP = os.path.join(WORK, "ipa", "Payload", "reprovisioned.app")
PLAYER = os.path.join(WORK, "unity", "AndroidPlayer")
VAR = os.path.join(PLAYER, "Variations", "il2cpp", "Release")
SDK = os.environ.get("ANDROID_HOME") or os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk")
BT = os.path.join(SDK, "build-tools", "36.0.0")
PLATFORM = os.path.join(SDK, "platforms", "android-35", "android.jar")
PACKAGE = "com.yuutatsubasa.marbleknights"
OUT = os.path.join(ROOT, "out")
STAGE = os.path.join(WORK, "apkstage")

MANIFEST = f"""<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{PACKAGE}"
    android:versionCode="1" android:versionName="0.0.1" android:installLocation="preferExternal">
  <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="29" />
  <uses-feature android:glEsVersion="0x00030000" />
  <supports-screens android:smallScreens="true" android:normalScreens="true" android:largeScreens="true"
      android:xlargeScreens="true" android:anyDensity="true" />
  <application android:label="@string/app_name" android:icon="@mipmap/app_icon" android:extractNativeLibs="true"
      android:isGame="true" android:hardwareAccelerated="false">
    <activity android:name="com.unity3d.player.UnityPlayerActivity" android:theme="@style/UnityThemeSelector"
        android:screenOrientation="sensorLandscape" android:launchMode="singleTask" android:resizeableActivity="false"
        android:configChanges="mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density"
        android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
      <meta-data android:name="unityplayer.UnityActivity" android:value="true" />
    </activity>
  </application>
</manifest>
"""

# Serialized-file target platform (BuildTarget): iOS = 9, Android = 13.
def patch_platform(path):
    with open(path, "r+b") as f:
        head = f.read(64)
        if head[0x14:0x20].startswith(b"2019."):
            off = 0x14 + head[0x14:].index(b"\0") + 1
            plat = struct.unpack_from("<i", head, off)[0]
            if plat == 9:
                f.seek(off)
                f.write(struct.pack("<i", 13))
                return True
    return False


def run(*cmd):
    print(">", " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
    subprocess.run(cmd, check=True)


def main():
    with_raw = "--with-raw" in sys.argv
    shutil.rmtree(STAGE, ignore_errors=True)
    os.makedirs(os.path.join(STAGE, "res"))
    os.makedirs(OUT, exist_ok=True)
    shutil.copytree(os.path.join(PLAYER, "Apk", "res"), os.path.join(STAGE, "res"), dirs_exist_ok=True)
    with open(os.path.join(STAGE, "res", "values", "strings.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                '  <string name="app_name">Marble Knights</string>\n'
                '  <string name="game_view_content_description">Game view</string>\n</resources>\n')
    with open(os.path.join(STAGE, "AndroidManifest.xml"), "w", encoding="utf-8") as f:
        f.write(MANIFEST)

    aapt2 = os.path.join(BT, "aapt2.exe")
    run(aapt2, "compile", "--dir", os.path.join(STAGE, "res"), "-o", os.path.join(STAGE, "res.zip"))
    base = os.path.join(STAGE, "base.apk")
    run(aapt2, "link", "-o", base, "-I", PLATFORM, "--manifest", os.path.join(STAGE, "AndroidManifest.xml"),
        os.path.join(STAGE, "res.zip"))

    # UnityPlayerActivity ships as source in 2019.4; compile it into classes2.dex.
    jdir = os.path.join(STAGE, "java")
    os.makedirs(jdir)
    classes_jar = os.path.join(VAR, "Classes", "classes.jar")
    run("javac", "-source", "8", "-target", "8", "-nowarn", "-cp", PLATFORM + os.pathsep + classes_jar, "-d", jdir,
        os.path.join(PLAYER, "Source", "com", "unity3d", "player", "UnityPlayerActivity.java"))
    classes = [os.path.join(dp, f) for dp, _, fs in os.walk(jdir) for f in fs if f.endswith(".class")]
    dexdir = os.path.join(STAGE, "dex")
    os.makedirs(dexdir)
    run(os.path.join(BT, "d8.bat"), "--min-api", "24", "--lib", PLATFORM, "--classpath", classes_jar,
        "--output", dexdir, *classes)

    unsigned = os.path.join(STAGE, "unsigned.apk")
    shutil.copy(base, unsigned)
    patched = 0
    with zipfile.ZipFile(unsigned, "a") as z:
        z.write(os.path.join(VAR, "Classes", "classes.dex"), "classes.dex", zipfile.ZIP_DEFLATED)
        z.write(os.path.join(dexdir, "classes.dex"), "classes2.dex", zipfile.ZIP_DEFLATED)
        libs = os.path.join(VAR, "Libs", "arm64-v8a")
        for so in ("libmain.so", "libunity.so"):
            z.write(os.path.join(libs, so), f"lib/arm64-v8a/{so}", zipfile.ZIP_DEFLATED)
        z.write(os.path.join(WORK, "build-android", "libil2cpp.so"), "lib/arm64-v8a/libil2cpp.so", zipfile.ZIP_DEFLATED)
        z.write(os.path.join(APP, "Frameworks", "UnityFramework.framework", "UnityFramework"),
                "lib/arm64-v8a/libmkframework.so", zipfile.ZIP_DEFLATED)
        data = os.path.join(APP, "Data")
        for dirpath, _, files in os.walk(data):
            rel = os.path.relpath(dirpath, data).replace("\\", "/")
            if rel.startswith("Raw") and not with_raw:
                continue
            for name in files:
                src = os.path.join(dirpath, name)
                arc = "assets/bin/Data/" + ("" if rel == "." else rel + "/") + name
                if rel.startswith("Raw"):
                    arc = "assets/" + ("" if rel == "Raw" else rel[4:] + "/") + name  # StreamingAssets
                if "--patch-platform" in sys.argv and not rel.startswith(("Managed", "Raw")):
                    tmp = os.path.join(STAGE, "patched_" + name.replace(" ", "_"))
                    shutil.copy(src, tmp)
                    if patch_platform(tmp):
                        patched += 1
                        src = tmp
                z.write(src, arc, zipfile.ZIP_STORED)  # Unity reads assets by offset; keep them uncompressed
    print("patched platform in", patched, "files")

    aligned = os.path.join(STAGE, "aligned.apk")
    run(os.path.join(BT, "zipalign.exe"), "-f", "-p", "4", unsigned, aligned)
    final = os.path.join(OUT, "marbleknights-private.apk")
    keystore = os.path.join(os.environ["USERPROFILE"], ".android", "debug.keystore")
    run(os.path.join(BT, "apksigner.bat"), "sign", "--ks", keystore, "--ks-pass", "pass:android",
        "--key-pass", "pass:android", "--out", final, aligned)
    print("wrote", final, os.path.getsize(final) // (1 << 20), "MB")


if __name__ == "__main__":
    main()
