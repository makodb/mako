#!/usr/bin/env bash
# verify_core.sh [--log FILE] [extra verus args...]
#
# Verus over the Raft core crate (src/deptran/raft/core, plan Phase 6) with
# the pinned binary ($VERUS_PIN, docs/verus/modification-plan.md §0.3). Exits
# 1 on any verification error, or if the verified count drops below the floor
# recorded in scripts/verus/core_verified_floor.txt (so a function that
# quietly leaves verus! -- or becomes external_body -- is noticed). From
# Phase 8 the same run imports spec v1 (§4.5, spike (d) choice (i)).
#
# The crate also builds with plain cargo, ghost code erased (spike (a)):
# this script is the proof half, not the build.
set -uo pipefail
: "${VERUS_PIN:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORE="$REPO_ROOT/src/deptran/raft/core/src/lib.rs"
FLOOR_FILE="$REPO_ROOT/scripts/verus/core_verified_floor.txt"
LOG=""
if [ "${1:-}" = "--log" ]; then LOG="$2"; shift 2; fi
LOG="${LOG:-$(mktemp /tmp/verify_core_XXXX.log)}"

t0=$SECONDS
"$VERUS_PIN" --crate-type=lib "$CORE" "$@" > "$LOG" 2>&1
rc=$?
summary=$(grep -E "verification results::" "$LOG" | tail -1)
echo "verify_core: ${summary:-no summary} ($((SECONDS - t0)) s, log $LOG)"
if [ $rc -ne 0 ] || [ -z "$summary" ]; then
  grep -E "^error" "$LOG" | sort | uniq -c | sort -rn | head -20
  echo "verify_core: FAILED (verus exit $rc)"
  exit 1
fi
verified=$(echo "$summary" | sed -E 's/.*:: ([0-9]+) verified.*/\1/')
errors=$(echo "$summary" | sed -E 's/.* ([0-9]+) errors.*/\1/')
floor=0
[ -f "$FLOOR_FILE" ] && floor=$(grep -E '^[0-9]+$' "$FLOOR_FILE" | head -1)
if [ "$errors" != "0" ]; then
  echo "verify_core: FAILED ($errors errors)"; exit 1
fi
if [ "$verified" -lt "${floor:-0}" ]; then
  echo "verify_core: FAILED (verified $verified < floor $floor)"; exit 1
fi
echo "verify_core: ok (verified $verified, floor ${floor:-0})"
