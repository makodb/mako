#!/usr/bin/env bash
# Run the Rust-lane microbenchmark (bench/) on the current tree, or A/B it
# across two commits.
#
#   scripts/run_microbench.sh                     # current working tree
#   scripts/run_microbench.sh --compare A B       # two commits, same sitting
#
# The compare mode is the one that answers questions. Absolute ns/op is
# machine-, thermal- and compiler-dependent and means very little on its own; a
# delta taken back-to-back on one box means something. It builds each commit
# from `git archive` (plus the Lion submodule at the commit that ref records,
# when it has one -- the extraction scripts/run_rpc_echo_bench.sh does), leaves
# that ref's own bench/ out and puts today's in its place (so the HARNESS is held
# constant and only the measured code varies), checks that the bench/ it is
# about to build is today's, then runs the builds alternately to spread thermal
# drift across both sides rather than loading it onto whichever ran second.
#
# Compare builds are offline: the Cargo registry and, for Lion-era refs, the
# Verus git checkout must be warm (see CLAUDE.md). bench/ is workspace-excluded,
# so none of this touches the source gate.
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
trap 'rm -rf "$WORK"' EXIT INT TERM

# What a build takes from today's bench/: everything Cargo reads for the
# srpc-bench package, and nothing else (not bench/target/).
HARNESS=(Cargo.toml Cargo.lock src)

harness_digest() {
    # harness_digest <bench dir>: one digest over the HARNESS files' names and
    # contents.
    (cd "$1" && find "${HARNESS[@]}" -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) |
        sha256sum | cut -c1-16
}

TODAY="$(harness_digest "$REPO/bench")"

# Indexed by side, not by ref, so an A/A run (the same ref twice, to read the
# noise floor) still gets two independent builds.
REFS=("$REF_A" "$REF_B")
BIN=()
for side in 0 1; do
    ref="${REFS[$side]}"
    dir="$WORK/$((side + 1))-$(git -C "$REPO" rev-parse --short "$ref^{commit}")"
    echo "### building $ref in $dir" >&2
    mkdir -p "$dir"
    # The ref's own bench/ stays out of the extraction rather than being copied
    # over: `cp -r bench <tree>/bench` into a tree that already has bench/ lands
    # in bench/bench and builds the ref's harness instead of today's.
    git -C "$REPO" archive "$ref" | tar -x -C "$dir" --anchored --exclude=bench
    if lion="$(git -C "$REPO" rev-parse --verify --quiet "$ref:third-party/lion")"; then
        if ! git -C "$REPO/third-party/lion" cat-file -e "$lion^{commit}" 2>/dev/null; then
            echo "$ref records third-party/lion at $lion, which $REPO/third-party/lion" \
                "does not have; run: git submodule update --init third-party/lion" >&2
            exit 1
        fi
        mkdir -p "$dir/third-party/lion"
        git -C "$REPO/third-party/lion" archive "$lion" | tar -x -C "$dir/third-party/lion"
    fi
    # Today's harness into that tree.
    mkdir "$dir/bench"
    for f in "${HARNESS[@]}"; do
        cp -r "$REPO/bench/$f" "$dir/bench/"
    done
    built="$(harness_digest "$dir/bench")"
    if [[ "$built" != "$TODAY" ]]; then
        echo "$ref: bench/ in $dir is harness $built, not today's $TODAY" >&2
        exit 1
    fi
    if [[ -z "$(git -C "$REPO" ls-tree "$ref" bench)" ]]; then
        own="has no bench/"
    elif git -C "$REPO" diff --quiet "$ref" -- "${HARNESS[@]/#/bench/}"; then
        own="has the same bench/"
    else
        own="has a different bench/"
    fi
    echo "### $ref: building today's harness $TODAY (the ref itself $own)" >&2
    # Only the codec harness: today's bench/ may carry binaries (rpc_echo) that
    # need a newer srpc API than an older ref provides.
    cargo build --release --quiet --offline --manifest-path "$dir/bench/Cargo.toml" \
        --target-dir "$dir/bench/target" --bin srpc-bench >&2
    BIN[$side]="$dir/bench/target/release/srpc-bench"
done

for round in 1 2; do
    for side in 0 1; do
        echo "=== ${REFS[$side]}  (round $round) ==="
        "${BIN[$side]}"
        echo
    done
done
