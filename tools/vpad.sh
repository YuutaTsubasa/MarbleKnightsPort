#!/usr/bin/env bash
# A virtual second gamepad on Thor (Android's `uinput` command, no root), for multi-pad tests.
# It copies the built-in pad's identity (usb 2020:0112, same evdev codes) so Android and Unity
# treat it the same way. Run while holding the `thor` lock.
# usage: [VPAD=n] tools/vpad.sh start | press <a|b|x|y|lb|rb|start|select> [n] | axis <x|y|hatx|haty> <value> | stop
set -euo pipefail
export MSYS_NO_PATHCONV=1
dev=(adb -s cc6a97fe)
n=${VPAD:-2}
d=/data/local/tmp/mk/vpad$n
send() { "${dev[@]}" shell "echo '$1' > $d/in"; }
syn='0,0,0'
case "${1:-}" in
  start)
    "${dev[@]}" shell "mkdir -p $d; rm -f $d/in; mkfifo $d/in"
    keys='[158,114,115,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,544,545,546,547]'
    absinfo=''
    for c in 0 1 2 5; do absinfo="$absinfo{\"code\":$c,\"info\":{\"value\":0,\"minimum\":-32767,\"maximum\":32767,\"fuzz\":0,\"flat\":15,\"resolution\":0}},"; done
    for c in 9 10; do absinfo="$absinfo{\"code\":$c,\"info\":{\"value\":0,\"minimum\":0,\"maximum\":32767,\"fuzz\":0,\"flat\":0,\"resolution\":0}},"; done
    absinfo="$absinfo{\"code\":16,\"info\":{\"value\":0,\"minimum\":-1,\"maximum\":1,\"fuzz\":0,\"flat\":0,\"resolution\":0}},{\"code\":17,\"info\":{\"value\":0,\"minimum\":-1,\"maximum\":1,\"fuzz\":0,\"flat\":0,\"resolution\":0}}"
    reg="{\"id\":1,\"command\":\"register\",\"name\":\"Virtual Pad $n\",\"vid\":8224,\"pid\":274,\"bus\":\"usb\",\"configuration\":[{\"type\":100,\"data\":[1,3]},{\"type\":101,\"data\":$keys},{\"type\":103,\"data\":[0,1,2,5,9,10,16,17]}],\"abs_info\":[$absinfo]}"
    "${dev[@]}" shell "cd $d; nohup sh -c 'sleep 86400 > in & echo \$! > holder; uinput - < in > log 2>&1' > /dev/null 2>&1 &"
    sleep 1
    send "$reg"
    sleep 1
    "${dev[@]}" shell "getevent -pl 2>/dev/null | grep -A1 'add device' | grep -c 'Virtual Pad $n'; cat $d/log"
    ;;
  press)
    declare -A code=([a]=304 [b]=305 [x]=307 [y]=308 [lb]=310 [rb]=311 [select]=314 [start]=315)
    c=${code[$2]}
    for _ in $(seq "${3:-1}"); do
      send "{\"id\":1,\"command\":\"inject\",\"events\":[1,$c,1,$syn]}"
      send "{\"id\":1,\"command\":\"delay\",\"duration\":120}"
      send "{\"id\":1,\"command\":\"inject\",\"events\":[1,$c,0,$syn]}"
      sleep 0.4
    done ;;
  axis)
    declare -A code=([x]=0 [y]=1 [hatx]=16 [haty]=17)
    send "{\"id\":1,\"command\":\"inject\",\"events\":[3,${code[$2]},$3,$syn]}" ;;
  stop)
    "${dev[@]}" shell "kill \$(cat $d/holder) 2>/dev/null; sleep 1; rm -rf $d" || true ;;
  *) sed -n 2,6p "$0"; exit 2 ;;
esac
