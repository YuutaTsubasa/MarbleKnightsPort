"""Package the Android APK: official Unity 2019.4.17f1 Android player + our proxy libil2cpp.so +
the iOS UnityFramework + the iOS Data folder.

The base APK (--base) has only the Unity player, our code and our start activity; the release ships
it and the launcher adds the player's own game data to it (tools/prepare_android.py). Without
--base this makes the private test APK from work/, signed with the local debug key.

Game data read and written here stays under work/ and out/ (gitignored). Never upload the result.

usage: python -I tools/build_apk.py [--with-raw] [--patch-platform] [--instance NAME] [--resizeable]
       python -I tools/build_apk.py --base OUT.apk [--lib libil2cpp.so] [--instance NAME]
  --instance b: a second copy (package ...marbleknights.b, "Marble Knights B") for testing online
                rooms on one device in split screen; --resizeable allows split screen
"""
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apk_sign  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work")
APP = os.path.join(WORK, "ipa", "Payload", "reprovisioned.app")
PLAYER = os.path.join(WORK, "unity", "AndroidPlayer")
VAR = os.path.join(PLAYER, "Variations", "il2cpp", "Release")
SDK = os.environ.get("ANDROID_HOME") or os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk")
BT = os.path.join(SDK, "build-tools", "36.0.0")
PLATFORM = os.path.join(SDK, "platforms", "android-35", "android.jar")
def arg(name):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None
INSTANCE = arg("--instance")
RESIZEABLE = "--resizeable" in sys.argv or INSTANCE is not None
PACKAGE = "com.yuutatsubasa.marbleknights" + ("." + INSTANCE if INSTANCE else "")
LABEL = "Marble Knights" + (" " + INSTANCE.upper() if INSTANCE else "")
OUT = os.path.join(ROOT, "out")
STAGE = os.path.join(WORK, "apkstage")
# the proxy; a release uses a build without the validation harness (MK_RECOMP_BATCH=OFF)
LIB = os.path.abspath(arg("--lib") or os.path.join(WORK, "build-android", "libil2cpp.so"))

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}"
    android:versionCode="1" android:versionName="0.1.0" android:installLocation="preferExternal">
  <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="29" />
  <uses-feature android:glEsVersion="0x00030000" />
  <uses-permission android:name="android.permission.VIBRATE" />
  <!-- online rooms over the LAN (loader/proxy/gamecenter_net.c) -->
  <uses-permission android:name="android.permission.INTERNET" />
  <!-- game data (3.4 GB) lives in /sdcard/MarbleKnights so it survives reinstalls; needs legacy storage -->
  <uses-permission android:name="android.permission.READ_EXTERNAL_STORAGE" />
  <supports-screens android:smallScreens="true" android:normalScreens="true" android:largeScreens="true"
      android:xlargeScreens="true" android:anyDensity="true" />
  <application android:label="@string/app_name" android:icon="@mipmap/app_icon" android:extractNativeLibs="true"
      android:isGame="true" android:hardwareAccelerated="false" android:requestLegacyExternalStorage="true">
    <!-- asks for storage access and checks the game data, then starts Unity (loader/android) -->
    <activity android:name="com.yuutatsubasa.marbleknights.MkStartActivity" android:screenOrientation="sensorLandscape"
        android:theme="@android:style/Theme.DeviceDefault.NoActionBar" android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
    <activity android:name="com.unity3d.player.UnityPlayerActivity" android:theme="@style/UnityThemeSelector"
        android:screenOrientation="sensorLandscape" android:launchMode="singleTask" android:resizeableActivity="{resizeable}"
        android:configChanges="mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density"
        android:exported="true">
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


def build_base(dst):
    """Everything but the game data: manifest and resources, Unity's classes.dex, our activities
    (classes2.dex), libmain/libunity from the Unity player, our libil2cpp.so, and the Android
    player's own 'unity default resources'."""
    shutil.rmtree(STAGE, ignore_errors=True)
    os.makedirs(os.path.join(STAGE, "res"))
    shutil.copytree(os.path.join(PLAYER, "Apk", "res"), os.path.join(STAGE, "res"), dirs_exist_ok=True)
    with open(os.path.join(STAGE, "res", "values", "strings.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                f'  <string name="app_name">{LABEL}</string>\n'
                '  <string name="game_view_content_description">Game view</string>\n</resources>\n')
    with open(os.path.join(STAGE, "AndroidManifest.xml"), "w", encoding="utf-8") as f:
        f.write(MANIFEST.replace("{package}", PACKAGE).replace("{resizeable}", "true" if RESIZEABLE else "false"))

    aapt2 = os.path.join(BT, "aapt2.exe")
    run(aapt2, "compile", "--dir", os.path.join(STAGE, "res"), "-o", os.path.join(STAGE, "res.zip"))
    res = os.path.join(STAGE, "res.apk")
    run(aapt2, "link", "-o", res, "-I", PLATFORM, "--manifest", os.path.join(STAGE, "AndroidManifest.xml"),
        os.path.join(STAGE, "res.zip"))

    # UnityPlayerActivity ships as source in 2019.4; compile it with our start activity into classes2.dex.
    jdir = os.path.join(STAGE, "java")
    os.makedirs(jdir)
    classes_jar = os.path.join(VAR, "Classes", "classes.jar")
    # The iOS BuildSettings only list Metal, so the Android player would fall back to GLES 2.0;
    # our patched shaders carry GLES3 programs, so ask for GLES 3.0 explicitly.
    src_dir = os.path.join(STAGE, "src", "com", "unity3d", "player")
    os.makedirs(src_dir)
    activity = open(os.path.join(PLAYER, "Source", "com", "unity3d", "player", "UnityPlayerActivity.java")).read()
    activity = activity.replace("return cmdLine;", 'return (cmdLine == null ? "" : cmdLine + " ") + "-force-gles30";', 1)
    open(os.path.join(src_dir, "UnityPlayerActivity.java"), "w").write(activity)
    run("javac", "-encoding", "UTF-8", "-source", "8", "-target", "8", "-nowarn", "-cp", PLATFORM + os.pathsep + classes_jar, "-d", jdir,
        os.path.join(src_dir, "UnityPlayerActivity.java"), os.path.join(ROOT, "loader", "android", "MkStartActivity.java"))
    classes = [os.path.join(dp, f) for dp, _, fs in os.walk(jdir) for f in fs if f.endswith(".class")]
    dexdir = os.path.join(STAGE, "dex")
    os.makedirs(dexdir)
    run(os.path.join(BT, "d8.bat"), "--min-api", "24", "--lib", PLATFORM, "--classpath", classes_jar,
        "--output", dexdir, *classes)

    libs = os.path.join(VAR, "Libs", "arm64-v8a")
    apk_sign.write_apk(dst, res, [
        ("classes.dex", os.path.join(VAR, "Classes", "classes.dex"), False),
        ("classes2.dex", os.path.join(dexdir, "classes.dex"), False),
        ("lib/arm64-v8a/libmain.so", os.path.join(libs, "libmain.so"), False),
        ("lib/arm64-v8a/libunity.so", os.path.join(libs, "libunity.so"), False),
        ("lib/arm64-v8a/libil2cpp.so", LIB, False),
        ("assets/bin/Data/unity default resources", os.path.join(PLAYER, "Data", "unity default resources"), True),
    ])
    return dst


def main():
    if "--base" in sys.argv:
        dst = os.path.abspath(arg("--base"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        build_base(dst)
        print("wrote", dst, os.path.getsize(dst) // (1 << 20), "MB")
        return
    with_raw = "--with-raw" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    base = build_base(os.path.join(WORK, "apkstage-base.apk"))
    add, patched = [], 0
    add.append(("lib/arm64-v8a/libmkframework.so", os.path.join(APP, "Frameworks", "UnityFramework.framework", "UnityFramework"), False))
    data = os.path.join(APP, "Data")
    for dirpath, _, files in os.walk(data):
        rel = os.path.relpath(dirpath, data).replace("\\", "/")
        if rel.startswith("Raw") and not with_raw:
            continue
        for name in files:
            if rel == "." and name == "unity default resources":  # the base has the Android player's copy
                continue
            src = os.path.join(dirpath, name)
            patched_src = os.path.join(WORK, "patched", "Data", os.path.relpath(src, data))
            if os.path.exists(patched_src):  # shaders with GLES3 programs added (tools/patch_shaders.py)
                src = patched_src
            arc = "assets/bin/Data/" + ("" if rel == "." else rel + "/") + name
            if rel.startswith("Raw"):
                arc = "assets/" + ("" if rel == "Raw" else rel[4:] + "/") + name  # StreamingAssets
            if "--patch-platform" in sys.argv and not rel.startswith(("Managed", "Raw")):
                tmp = os.path.join(STAGE, "patched_" + name.replace(" ", "_"))
                shutil.copy(src, tmp)
                if patch_platform(tmp):
                    patched += 1
                    src = tmp
            add.append((arc, src, True))  # Unity reads assets by offset; keep them uncompressed
    print("patched platform in", patched, "files")

    aligned = os.path.join(STAGE, "aligned.apk")
    apk_sign.write_apk(aligned, base, add)
    final = os.path.join(OUT, "marbleknights-private" + ("-" + INSTANCE if INSTANCE else "") + ".apk")
    keystore = os.path.join(os.environ["USERPROFILE"], ".android", "debug.keystore")
    run(os.path.join(BT, "apksigner.bat"), "sign", "--ks", keystore, "--ks-pass", "pass:android",
        "--key-pass", "pass:android", "--out", final, aligned)
    print("wrote", final, os.path.getsize(final) // (1 << 20), "MB")


if __name__ == "__main__":
    main()
