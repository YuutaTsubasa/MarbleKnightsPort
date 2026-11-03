#!/usr/bin/env bash
# Push the probe (and, with --sync, the game files it needs) to Thor and run it.
# Takes the `thor` lock. Game files go to /data/local/tmp/mk only (never into Git).
# usage: tools/thor-run.sh [--sync] [ENV=VALUE ...]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ops="$USERPROFILE/Data/10_Projects/tailsmachine-ops"
export LOCK_SESSION="${LOCK_SESSION:-marble-knights}" LOCK_MODEL="${LOCK_MODEL:-claude-opus-5-5}"
exec "$ops/tools/lock/with-lock.sh" thor 10 "Marble Knights il2cpp probe" -- bash "$here/tools/thor-run-inner.sh" "$@"
