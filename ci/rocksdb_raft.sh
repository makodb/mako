#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run this suite through ./docker_build.sh ci rocksdbRaft.' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
build_dir="${ROCKSDB_RAFT_BUILD_DIR:-$root/build_rocksdb_raft}"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$build_dir/cargo}"
export CARGO_BUILD_JOBS="${CI_BUILD_JOBS:-32}"

cmake -S "$root" -B "$build_dir" -G Ninja \
    -DMAKO_BUILD_PROFILE=ROCKSDB_RAFT -DCMAKE_BUILD_TYPE=Release \
    -DMAKO_RUST_TARGET_DIR="$CARGO_TARGET_DIR" -DMAKO_ROCKSDB_RAFT_TEST_HOOKS=OFF
cmake --build "$build_dir" --parallel "$CARGO_BUILD_JOBS" --target rocksdb_raft
ctest --test-dir "$build_dir" --output-on-failure
evidence_dir="$(mktemp -d "$build_dir/runtime-evidence.XXXXXX")"
python3 "$root/scripts/rocksdb_raft/smoke.py" --bin-dir "$build_dir" --data-dir "$evidence_dir/production"
python3 "$root/scripts/rocksdb_raft/smoke.py" --bin-dir "$build_dir" \
    --data-dir "$evidence_dir/production-async" --durability async

# Keep the production binaries distinct from the explicitly fault-enabled build.
cmake -S "$root" -B "${build_dir}_faults" -G Ninja \
    -DMAKO_BUILD_PROFILE=ROCKSDB_RAFT -DCMAKE_BUILD_TYPE=Release \
    -DMAKO_RUST_TARGET_DIR="$CARGO_TARGET_DIR" -DMAKO_ROCKSDB_RAFT_TEST_HOOKS=ON
cmake --build "${build_dir}_faults" --parallel "$CARGO_BUILD_JOBS" --target rocksdb_raft
python3 "$root/scripts/rocksdb_raft/smoke.py" --bin-dir "${build_dir}_faults" \
    --faults-only --data-dir "$evidence_dir/application-faults"
python3 "$root/scripts/rocksdb_raft/boundaries.py" --bin-dir "${build_dir}_faults" \
    --data-dir "$evidence_dir/commit-boundaries"
python3 "$root/scripts/rocksdb_raft/snapshots.py" --bin-dir "${build_dir}_faults" \
    --data-dir "$evidence_dir/native-snapshots"
