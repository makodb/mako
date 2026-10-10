#!/usr/bin/env bash
# core_check.sh -- what a raft-core commit must pass before it is made:
# clippy -D warnings over the Raft workspace with and without the lab and
# disk features (the build's source gate runs the first), the ledger lint (every
# core line ghost, moved or labelled, plan A.3), the correspondence doc's
# freshness (the build checks it), and verify_core.sh.
# Exits non-zero on the first failure, so `core_check.sh && git commit`
# cannot commit a crate that plain cargo or Verus rejects.
set -uo pipefail
: "${VERUS_PIN:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT/src/deptran/raft" || exit 2
# Cargo's output stays out of the source tree (and the NFS home).
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-/var/tmp/raft-cargo-${USER:-$(id -un)}}"
for features in "" "--features raft_test" "--features raft_disk" "--features raft_test,raft_disk"; do
  if ! out=$(cargo clippy --quiet --workspace $features -- -D warnings 2>&1); then
    echo "core_check: clippy ${features:-(default)} FAILED"; echo "$out" | head -40; exit 1
  fi
done
echo "core_check: clippy ok (default, raft_test, raft_disk, both)"
# The tests the trusted note exactness rests on (host contract §1 item 5).
if ! out=$(cargo test --quiet -p raft-core -p raft-replay 2>&1); then
  echo "core_check: raft-core / raft-replay tests FAILED"; echo "$out" | tail -40; exit 1
fi
echo "core_check: raft-core and raft-replay tests ok"
if ! out=$(python3 "$REPO_ROOT/scripts/verus/ledger_lint.py" 2>&1); then
  echo "core_check: ledger lint FAILED"; echo "$out" | tail -40; exit 1
fi
echo "core_check: $(echo "$out" | tail -1)"
# From Phase 8 on the core changes only in ghost code (plan Phase 8's
# diff-2 lint): against the commit recorded in ghost_only_base.txt (Phase
# 6's last), every changed line of core/src must be ghost.
BASE_FILE="$REPO_ROOT/scripts/verus/ghost_only_base.txt"
if [ -s "$BASE_FILE" ]; then
  if ! out=$(python3 "$REPO_ROOT/scripts/verus/ledger_lint.py" --ghost-only --base "$(cat "$BASE_FILE")" 2>&1); then
    echo "core_check: ghost-only lint FAILED"; echo "$out" | tail -40; exit 1
  fi
  echo "core_check: $(echo "$out" | tail -1)"
fi
# the build fails on a stale correspondence doc (line counts of the shell)
if ! python3 "$REPO_ROOT/scripts/gen_correspondence.py" --check; then
  echo "core_check: run scripts/gen_correspondence.py"; exit 1
fi
"$REPO_ROOT/scripts/verus/verify_core.sh" "$@" || exit 1
