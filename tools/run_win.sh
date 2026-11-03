#!/usr/bin/env bash
# Run the Windows test build (work/win/MarbleKnights) windowed for N seconds, then close it and show
# the logs: mk_ga.log (our GameAssembly.dll) and player.log (Unity). Our own test build only.
# usage: tools/run_win.sh [seconds]
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
d="$here/work/win/MarbleKnights"
secs="${1:-20}"
cd "$d"
MSYS_NO_PATHCONV=1 ./MarbleKnights.exe -logFile "$(cygpath -w "$d/player.log")" -screen-fullscreen 0 \
  -screen-width 960 -screen-height 540 &
pid=$!
for _ in $(seq "$secs"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
if kill -0 $pid 2>/dev/null; then
  echo "still running after ${secs}s: closing it"
  taskkill //F //PID "$(cat /proc/$pid/winpid 2>/dev/null || echo 0)" >/dev/null 2>&1 || taskkill //F //IM MarbleKnights.exe >/dev/null 2>&1
fi
wait $pid 2>/dev/null
echo "--- mk_ga.log (tail)"; tail -25 mk_ga.log 2>/dev/null
echo "--- player.log (tail)"; tail -25 player.log 2>/dev/null
