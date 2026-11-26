#!/usr/bin/env bash
# Independent compilation, local tests and wrapper quality; no application build.
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run ./docker_build.sh ci masstreeTx [jobs].' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build="${MASSTREE_TX_BUILD_DIR:-$root/build_masstree_tx}"
jobs="${CI_BUILD_JOBS:-32}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'CI_BUILD_JOBS must be positive' >&2; exit 2; }
export CARGO_BUILD_JOBS="$jobs"
# Own the target directory even when the caller normally uses another Cargo tree.
# This keeps feature variants and native test executables independent of runtime builds.
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMAKO_BUILD_PROFILE=MASSTREE_TX -DMAKO_NATIVE_CARGO_PROFILE=dev \
    -DMAKO_NATIVE_CARGO_TARGET="$build/cargo"
cmake --build "$build" --parallel "$jobs" --target masstree_tx_build \
    2>&1 | tee "$build/build.log"
cmake --build "$build" --parallel "$jobs" --target masstree_tx_tests masstree_tx_quality \
    2>&1 | tee "$build/tests.log"
