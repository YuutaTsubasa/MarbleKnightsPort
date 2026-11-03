#!/usr/bin/env bash
# Install out/marbleknights-private.apk on Thor, launch it, save logcat to work/logcat.txt.
set -euo pipefail
export MSYS_NO_PATHCONV=1
here="$(cd "$(dirname "$0")/.." && pwd -W)"
dev=(adb -s cc6a97fe)
pkg=com.yuutatsubasa.marbleknights
secs="${1:-25}"
top=$("${dev[@]}" shell dumpsys activity activities | grep -m1 -E "topResumedActivity|mResumedActivity" || true)
echo "top activity: ${top:-none}"
home=$("${dev[@]}" shell cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.HOME | tail -1 | cut -d/ -f1 | tr -d '
')
case "$top" in *"$pkg"*|*"$home"*|*launcher*|*Launcher*|"") ;; *) echo "something else is in the foreground ($top); not launching"; exit 3;; esac
"${dev[@]}" install -r -g "$here/out/marbleknights-private.apk" | tail -1
"${dev[@]}" shell am force-stop $pkg
"${dev[@]}" shell input keyevent KEYCODE_WAKEUP; "${dev[@]}" shell wm dismiss-keyguard; sleep 1
"${dev[@]}" logcat -c
"${dev[@]}" shell am start -n $pkg/com.unity3d.player.UnityPlayerActivity | tail -1
# optional scripted input: TAPS="40:960,540 55:960,540" (seconds after launch : x,y), screenshots after each
elapsed=0
for t in ${TAPS:-}; do
  at=${t%%:*}; xy=${t#*:}
  sleep $((at - elapsed)); elapsed=$at
  "${dev[@]}" exec-out screencap -p > "$here/work/screen_${at}s.png" || true
  case "$xy" in
    key=*) "${dev[@]}" shell input -d 0 keyevent ${xy#key=}; echo "key ${xy#key=} at ${at}s" ;;
    hold=*) p=${xy#hold=}; "${dev[@]}" shell input -d 0 swipe ${p%,*} ${p#*,} ${p%,*} ${p#*,} 400; echo "hold $p at ${at}s" ;;
    *) "${dev[@]}" shell input -d 0 tap ${xy%,*} ${xy#*,}; echo "tapped $xy at ${at}s" ;;
  esac
done
sleep $((secs > elapsed ? secs - elapsed : 1))
"${dev[@]}" exec-out screencap -p > "$here/work/screen.png" || true
pid=$("${dev[@]}" shell pidof $pkg || true)
if [ -n "$pid" ]; then
  echo "pid $pid alive"
  "${dev[@]}" shell "grep -E 'libil2cpp|libunity|libmain' /proc/$pid/maps | awk '{print \$6}' | sort -u; ls /proc/$pid/task | wc -l" > "$here/work/procinfo.txt" 2>&1 || true
  "${dev[@]}" shell "su -c 'debuggerd -b $pid'" > "$here/work/stacks.txt" 2>&1 || "${dev[@]}" shell "debuggerd -b $pid" > "$here/work/stacks.txt" 2>&1 || true
else
  echo "process is gone"
fi
"${dev[@]}" logcat -d -b all > "$here/work/logcat.txt"
"${dev[@]}" shell am force-stop $pkg
"${dev[@]}" shell input keyevent KEYCODE_WAKEUP; "${dev[@]}" shell wm dismiss-keyguard; sleep 1
echo "logcat lines: $(wc -l < "$here/work/logcat.txt")"
