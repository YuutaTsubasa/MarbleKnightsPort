#!/usr/bin/env bash
# Push the shader-patched AssetBundles (work/patched/Raw) over the copies on Thor. Takes the thor lock.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ops="$USERPROFILE/Data/10_Projects/tailsmachine-ops"
export LOCK_SESSION="${LOCK_SESSION:-marble-knights}" LOCK_MODEL="${LOCK_MODEL:-claude-opus-5-5}"
exec "$ops/tools/lock/with-lock.sh" thor 15 "Marble Knights push patched bundles" -- bash -c '
export MSYS_NO_PATHCONV=1
dst=/sdcard/Android/data/com.yuutatsubasa.marbleknights/files/Raw
cd "$0/work/patched/Raw"
find . -type f | sed "s|^\./||" | while read -r f; do
  adb -s cc6a97fe push "$f" "$dst/$f" > /dev/null || echo "FAILED $f"
done
echo pushed $(find . -type f | wc -l) files' "$(cd "$here" && pwd -W)"
