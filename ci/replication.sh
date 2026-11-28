#!/usr/bin/env bash
# Shared storage/Raft components and the distributed runtime without Masstree.
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run this suite through ./docker_build.sh ci replication.' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
build_dir="${REPLICATION_BUILD_DIR:-$root/build_replication}"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$build_dir/cargo}"
export CARGO_BUILD_JOBS="${CI_BUILD_JOBS:-32}"
[[ "$CARGO_BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'CI_BUILD_JOBS must be positive' >&2; exit 2; }
mkdir -p "$build_dir"
# Keep component qualification independent of Masstree/native storage settings.
unset MAKO_MTREE_NATIVE_INTEGRATION MAKO_MTREE_NATIVE_LIB_DIRS MAKO_MTREE_NATIVE_LIBS
common=(--locked --manifest-path "$root/crates/Cargo.toml")
{
    cargo test "${common[@]}" -p sto-core --no-default-features --features test-install-observer
    cargo test "${common[@]}" -p sto-core --features test-install-observer
    cargo test "${common[@]}" -p sto-core --no-default-features --features clock-test-hooks --lib clock::
    cargo test "${common[@]}" -p sto-core --features clock-test-hooks --lib clock::
    # Protocol scenarios own real SRPC workers, disk fsyncs and snapshots.
    cargo test "${common[@]}" -p mako-rocksdb-store -p sto-rocksdb -p raft-host \
        --all-targets --features mako-rocksdb-store/test-hooks,sto-rocksdb/test-hooks,raft-host/test-hooks \
        -- --test-threads=1
    cargo test --locked --manifest-path "$root/src/cluster/Cargo.toml" -- --test-threads=1
    cargo test --locked --manifest-path "$root/src/deptran/raft/Cargo.toml" \
        -p raft-store --features rocksdb -- --test-threads=1
} 2>&1 | tee "$build_dir/components.log"

cmake -S "$root" -B "$build_dir" -G Ninja \
    -DMAKO_BUILD_PROFILE=ROCKSDB_RUNTIME -DCMAKE_BUILD_TYPE=Debug \
    -DMAKO_RUST_TARGET_DIR="$CARGO_TARGET_DIR"
cmake --build "$build_dir" --parallel "$CARGO_BUILD_JOBS" \
    --target rust_runtime_tests rocks_runtime_audit 2>&1 | tee "$build_dir/runtime.log"

# Small correctness workload for the leaf executable, outside performance CI.
python3 examples/run_native.py tpcc --build "$build_dir" --engine rocksdb \
    --shards 2 --workers 2 --count 30 --seconds 30 2>&1 | tee "$build_dir/workload.log"
