#!/usr/bin/env bash
# Push work/raw-android (StreamingAssets: AssetBundles + FMOD banks, iOS -> android names)
# to the app's external files dir on Thor. Takes the thor lock. Game data: never commit.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ops="$USERPROFILE/Data/10_Projects/tailsmachine-ops"
export LOCK_SESSION="${LOCK_SESSION:-marble-knights}" LOCK_MODEL="${LOCK_MODEL:-claude-opus-5-5}"
exec "$ops/tools/lock/with-lock.sh" thor 20 "Marble Knights push StreamingAssets" -- bash -c '
export MSYS_NO_PATHCONV=1
dst=/sdcard/Android/data/com.yuutatsubasa.marbleknights/files/Raw
adb -s cc6a97fe shell mkdir -p $dst
# adbd may not create nested dirs here ("secure_mkdirs failed"), and one huge directory
# push dies with "failed to read copy response: EOF": make the dirs with the shell, push file by file.
cd "$0/work/raw-android"
dirs=$(find . -mindepth 1 -type d | sed "s|^\./||" | sed "s|^|$dst/|" | tr "
" " ")
adb -s cc6a97fe shell mkdir -p $dst $dirs
find . -type f | sed "s|^\./||" | while read -r f; do
  adb -s cc6a97fe push --sync "$f" "$dst/$f" > /dev/null || echo "FAILED $f"
done
adb -s cc6a97fe shell du -sh $dst' "$(cd "$here" && pwd -W)"
