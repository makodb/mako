#!/usr/bin/env bash
# verify_core.sh [--log FILE] [extra verus args...]
#
# Verus over the Raft core crate (src/deptran/raft/core, plan Phase 6) with
# the pinned binary ($VERUS_PIN, docs/verus/modification-plan.md §0.3). Exits
# 1 on any verification error, or if any function in the crate is trusted
# (#[verifier::external_body]) without being listed in
# scripts/verus/core_trusted.txt -- the crate's trusted surface, which must
# only shrink. (Verus's "verified" count is reported but not gated: it counts
# verification conditions, not functions, and moves under refactoring.) The
# run imports spec v1 (the manifest's tag; plan §4.5, spike (d) choice (i))
# for the proof side (core/src/coupling.rs, Verus only).
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
  --import glr="$EXPORT/glr.vir" "$@" > "$LOG" 2>&1
rc=$?
summary=$(grep -E "verification results::" "$LOG" | tail -1)
echo "verify_core: ${summary:-no summary} ($((SECONDS - t0)) s, log $LOG)"
if [ $rc -ne 0 ] || [ -z "$summary" ]; then
  grep -E "^error" "$LOG" | sort | uniq -c | sort -rn | head -20
  echo "verify_core: FAILED (verus exit $rc)"
  exit 1
fi
errors=$(echo "$summary" | sed -E 's/.* ([0-9]+) errors.*/\1/')
if [ "$errors" != "0" ]; then
  echo "verify_core: FAILED ($errors errors)"; exit 1
fi
# The trusted surface: every external_body function, by name.
trusted=$(grep -h -A4 '#\[verifier::external_body\]' "$REPO_ROOT"/src/deptran/raft/core/src/*.rs \
          | grep -oE 'fn [a-z_][a-z0-9_]*' | sed 's/^fn //' | sort -u)
allowed=$(grep -vE '^\s*(#|$)' "$TRUSTED_FILE" 2>/dev/null | awk '{print $1}' | sort -u)
extra=$(comm -23 <(echo "$trusted") <(echo "$allowed") | grep -v '^$' || true)
if [ -n "$extra" ]; then
  echo "verify_core: FAILED (trusted but not in core_trusted.txt: $(echo $extra))"; exit 1
fi
echo "verify_core: ok ($summary; trusted: $(echo $trusted))"
