#!/usr/bin/env bash
# usage: tools/thor-apk.sh [seconds]   (takes the thor lock)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ops="$USERPROFILE/Data/10_Projects/tailsmachine-ops"
export LOCK_SESSION="${LOCK_SESSION:-marble-knights}" LOCK_MODEL="${LOCK_MODEL:-claude-opus-5-5}"
exec "$ops/tools/lock/with-lock.sh" thor 10 "Marble Knights APK test" -- bash "$here/tools/thor-apk-inner.sh" "$@"
