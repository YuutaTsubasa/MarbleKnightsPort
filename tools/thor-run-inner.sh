#!/usr/bin/env bash
set -euo pipefail
export MSYS_NO_PATHCONV=1
here="$(cd "$(dirname "$0")/.." && pwd -W)"
app="$here/work/ipa/Payload/reprovisioned.app"
dev=(adb -s cc6a97fe)
sync=0
envs=()
for a in "$@"; do
  if [ "$a" = --sync ]; then sync=1; else envs+=("$a"); fi
done
# Never disturb a game the user is playing.
top=$("${dev[@]}" shell dumpsys activity activities | grep -m1 -E "topResumedActivity|mResumedActivity" || true)
echo "top activity: ${top:-none}"
"${dev[@]}" shell mkdir -p /data/local/tmp/mk/Data/Managed/Metadata
if [ $sync = 1 ]; then
  "${dev[@]}" push "$app/Frameworks/UnityFramework.framework/UnityFramework" /data/local/tmp/mk/UnityFramework >/dev/null
  "${dev[@]}" push "$app/Data/Managed/Metadata/global-metadata.dat" /data/local/tmp/mk/Data/Managed/Metadata/ >/dev/null
  "${dev[@]}" push "$app/Data/Managed/mono" /data/local/tmp/mk/Data/Managed/ >/dev/null
fi
"${dev[@]}" push "$here/work/build-android/il2cpp_probe" /data/local/tmp/mk/il2cpp_probe >/dev/null
"${dev[@]}" shell "cd /data/local/tmp/mk && chmod 755 il2cpp_probe && ${envs[*]:-} timeout 120 ./il2cpp_probe /data/local/tmp/mk 2>&1" || true
