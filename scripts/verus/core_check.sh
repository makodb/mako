#!/usr/bin/env bash
# core_check.sh -- what a raft-core commit must pass before it is made:
# clippy -D warnings over the Raft workspace with and without the lab
# feature (the build's source gate runs the former), and verify_core.sh.
# Exits non-zero on the first failure, so `core_check.sh && git commit`
# cannot commit a crate that plain cargo or Verus rejects.
set -uo pipefail
: "${VERUS_PIN:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT/src/deptran/raft" || exit 2
for features in "" "--features raft_test"; do
  if ! out=$(cargo clippy --quiet --workspace $features -- -D warnings 2>&1); then
    echo "core_check: clippy ${features:-(default)} FAILED"; echo "$out" | head -40; exit 1
  fi
done
echo "core_check: clippy ok (default, raft_test)"
"$REPO_ROOT/scripts/verus/verify_core.sh" "$@" || exit 1
