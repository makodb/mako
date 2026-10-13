#!/usr/bin/env bash
# verify_core.sh [--log FILE] [extra verus args...]
#
# Verus over the Raft core crate (src/deptran/raft/core, plan Phase 6) with
# the pinned binary ($VERUS_PIN, docs/verus/modification-plan.md §0.3). The
# run uses --output-json --time-expanded, and scripts/verus/verus_gate.py
# decides (docs/verus/disk-persistence.md §8): it exits 1 on any
# verification error, if a function named in
# scripts/verus/verified_functions.txt is missing or failed, or if the core
# trusts anything (the spellings are in verus_gate.py's docstring) not listed
# in scripts/verus/core_trusted.txt -- the crate's trusted surface, which must
# only shrink. The run imports spec v1 (the manifest's tag; plan
# §4.5, spike (d) choice (i)) for the proof side (core/src/coupling.rs, Verus
# only).
#
# The crate also builds with plain cargo, ghost code erased (spike (a)):
# this script is the proof half, not the build.
set -uo pipefail
: "${VERUS_PIN:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORE="$REPO_ROOT/src/deptran/raft/core/src/lib.rs"
TRUSTED_FILE="$REPO_ROOT/scripts/verus/core_trusted.txt"
LOG=""
if [ "${1:-}" = "--log" ]; then LOG="$2"; shift 2; fi
LOG="${LOG:-$(mktemp /tmp/verify_core_XXXX.log)}"

# The spec the proof side imports (plan §4.5, Phase 0 spike (d) choice (i)):
# the group's crate exported by Verus from the frozen tag the manifest
# names, archived out of $GLR (so its working tree does not matter) and
# built once per tag into $RESULTS/spec/export/<tag>.
: "${GLR:?source ~/mako-verus-env.sh first}"
: "${RESULTS:?source ~/mako-verus-env.sh first}"
TAG=$(sed -n 's/^tag = "\(.*\)"$/\1/p' "$REPO_ROOT/src/deptran/raft/verus/spec/SPEC_VERSION.toml")
EXPORT="$RESULTS/spec/export/$TAG"
if [ ! -s "$EXPORT/glr.vir" ] || [ ! -s "$EXPORT/libglr.rlib" ]; then
  echo "verify_core: exporting the spec crate at $TAG (about 45 s, once per tag)"
  rm -rf "$EXPORT" && mkdir -p "$EXPORT"
  git -C "$GLR" archive "$TAG" src | tar -x -C "$EXPORT" || { echo "verify_core: cannot archive $TAG"; exit 2; }
  # this Verus takes --export's argument as the output path
  (cd "$EXPORT" && "$VERUS_PIN" --crate-type=lib --crate-name glr src/lib.rs --no-verify \
       --export glr.vir --compile -o libglr.rlib > export.log 2>&1) \
    || { echo "verify_core: export failed, see $EXPORT/export.log"; exit 2; }
fi

t0=$SECONDS
"$VERUS_PIN" --crate-type=lib "$CORE" --extern glr="$EXPORT/libglr.rlib" \
  --import glr="$EXPORT/glr.vir" --output-json --time-expanded "$@" > "$LOG" 2>&1
rc=$?
echo "verify_core: verus exit $rc ($((SECONDS - t0)) s, log $LOG)"
if [ $rc -ne 0 ]; then
  grep -E "^error" "$LOG" | sort | uniq -c | sort -rn | head -20
fi
python3 "$REPO_ROOT/scripts/verus/verus_gate.py" check "$LOG" \
  "$REPO_ROOT/src/deptran/raft/core/src" \
  "$REPO_ROOT/scripts/verus/verified_functions.txt" "$TRUSTED_FILE" || exit 1
[ $rc -eq 0 ] || { echo "verify_core: FAILED (verus exit $rc)"; exit 1; }
