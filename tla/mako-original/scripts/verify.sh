#!/usr/bin/env bash
# Verify every module of the Mako model with the pinned Verus release.
# Usage: VERUS_PATH=/path/to/verus scripts/verify.sh [extra verus args]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
verus="${VERUS_PATH:-verus}"
cd "$root"
mods=()
for f in src/*.rs; do
    m="${f##*/}"; m="${m%.rs}"
    if [[ "$m" != lib ]]; then mods+=(--verify-only-module "$m"); fi
done
exec "$verus" --crate-type=lib src/lib.rs "${mods[@]}" --num-threads 8 "$@"
