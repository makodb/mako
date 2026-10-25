#!/usr/bin/env bash
# Verify the complete MakoV2 crate, including constructive executions.
# Usage: VERUS_PATH=/path/to/verus scripts/verify.sh [extra verus args]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
verus="${VERUS_PATH:-verus}"
sysroot_args=()
if [[ -n "${VERUS_SYSROOT:-}" ]]; then
    sysroot_args=(--sysroot "$VERUS_SYSROOT")
fi
cd "$root"
exec "$verus" "${sysroot_args[@]}" --crate-type=lib src/lib.rs --no-cheating \
    --num-threads 8 --triggers-mode silent "$@"
