#!/usr/bin/env bash
set -euo pipefail
source /var/tmp/mako-w1-implementation.VtilJG/build-candidate.sh "$@"
cargo build --locked --release --manifest-path "$manifest" \
    -p mako-cache-bench --no-default-features --features "engine-$arm" --bin gc_soak
check_native
