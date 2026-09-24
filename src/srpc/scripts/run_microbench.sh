#!/usr/bin/env bash
# Run the Rust-lane microbenchmark (bench/) on the current tree, or A/B it
# across two commits.
#
#   scripts/run_microbench.sh                     # current working tree
#   scripts/run_microbench.sh --compare A B       # two commits, same sitting
#
# The compare mode is the one that answers questions. Absolute ns/op is
# machine-, thermal- and compiler-dependent and means very little on its own; a
# delta taken back-to-back on one box means something. It builds each commit in
# a detached worktree, copies today's bench/ into both (so the HARNESS is held
# constant and only the measured code varies), then runs them alternately to
# spread thermal drift across both sides rather than loading it onto whichever
# ran second.
#
# bench/ is workspace-excluded, so none of this touches the source gate.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -eq 0 ]]; then
    exec cargo run --release --quiet --manifest-path "$REPO/bench/Cargo.toml"
fi

if [[ "${1:-}" != "--compare" || $# -ne 3 ]]; then
    echo "usage: $0 [--compare <ref-a> <ref-b>]" >&2
    exit 2
fi

REF_A="$2"
REF_B="$3"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/srpc-microbench-XXXXXX")"

cleanup() {
    for ref in "$REF_A" "$REF_B"; do
        git -C "$REPO" worktree remove --force "$WORK/$ref" 2>/dev/null || true
    done
    rm -rf "$WORK"
    git -C "$REPO" worktree prune 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for ref in "$REF_A" "$REF_B"; do
    echo "### building $ref" >&2
    git -C "$REPO" worktree add --detach "$WORK/$ref" "$ref" >/dev/null
    # Today's harness into yesterday's tree: if the benchmark itself differed
    # between the two sides, the delta would measure the harness, not the code.
    cp -r "$REPO/bench" "$WORK/$ref/bench"
    cargo build --release --quiet --manifest-path "$WORK/$ref/bench/Cargo.toml" >&2
done

for round in 1 2; do
    for ref in "$REF_A" "$REF_B"; do
        echo "=== $ref  (round $round) ==="
        "$WORK/$ref/bench/target/release/srpc-bench"
        echo
    done
done
