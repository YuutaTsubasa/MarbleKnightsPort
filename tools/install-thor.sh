#!/usr/bin/env bash
# One-shot install on Thor: build the proxy and APK, install it, and make sure the game data
# (StreamingAssets: shader-patched AssetBundles + FMOD banks, 3.4 GB) is in /sdcard/MarbleKnights/Raw,
# where it survives uninstall/reinstall. Existing data in the app's private folder is moved there.
# Takes the thor lock for the device part. Game data never leaves this PC except to Thor.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd -W)"
ops="$USERPROFILE/Data/10_Projects/tailsmachine-ops"
cmake --build "$here/work/build-android" >/dev/null
python -I "$here/tools/build_apk.py" | grep -E "wrote"
export LOCK_SESSION="${LOCK_SESSION:-marble-knights}" LOCK_MODEL="${LOCK_MODEL:-claude-opus-5-5}"
exec "$ops/tools/lock/with-lock.sh" thor 30 "Marble Knights install" -- bash -c '
set -e
export MSYS_NO_PATHCONV=1
dev="adb -s cc6a97fe"
pkg=com.yuutatsubasa.marbleknights
shared=/sdcard/MarbleKnights/Raw
private=/sdcard/Android/data/$pkg/files/Raw
$dev install -r -g "$0/out/marbleknights-private.apk" | tail -1
if ! $dev shell test -e $shared/AssetBundles/ios/ios; then
  if $dev shell test -e $private/AssetBundles/ios/ios; then
    echo "moving data from the app folder to $shared"
    $dev shell "mkdir -p /sdcard/MarbleKnights && mv $private $shared"
  else
    echo "pushing data to $shared (first time, a few minutes)"
    src="$0/work/raw-android"; patched="$0/work/patched/Raw"
    mapname() { echo "$1" | sed -e "s|AssetBundles/android/android|AssetBundles/ios/ios|" -e "s|AssetBundles/android/|AssetBundles/ios/|"; }
    cd "$src"
    dirs=$(find . -mindepth 1 -type d | sed "s|^\./||" | while read -r x; do echo "$shared/$(mapname "$x")"; done | tr "\n" " ")
    $dev shell mkdir -p $shared $dirs
    find . -type f | sed "s|^\./||" | while read -r f; do
      [ -e "$patched/$f" ] && from="$patched/$f" || from="$f"
      $dev push "$from" "$shared/$(mapname "$f")" > /dev/null || echo "FAILED $f"
    done
  fi
fi
$dev shell du -sh $shared' "$here"
