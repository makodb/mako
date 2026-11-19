#!/usr/bin/env bash
# Real-engine Docker smoke: two-process/colocated native cutover and STO epoch RCU.
#   ./docker_build.sh ci nativeShardingSmoke
# Existing Docker binaries: ./docker_build.sh ci-quick nativeShardingSmoke
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Use ./docker_build.sh ci nativeShardingSmoke; tests require Docker.' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
python3 examples/native_sharding_smoke.py
exec ctest --test-dir "${BUILD_DIR:-build_docker}" --output-on-failure \
    --no-tests=error -R '^(test_mako_native_mixed_scan|STOEpochRCUTests)$'
