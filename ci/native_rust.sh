#!/usr/bin/env bash
# Native production gates. Each launcher owns and joins only its children.
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run inside the build container (./docker_build.sh ci nativeRust).' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
build="${BUILD_DIR:-$root/build}"
jobs="${CI_MAKE_JOBS:-4}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'CI_MAKE_JOBS must be positive' >&2; exit 2; }

compile() {
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DMAKO_NATIVE_CARGO_TARGET="${CARGO_TARGET_DIR:-$build/cargo}"
    cmake --build "$build" --parallel "$jobs"
}
components() {
    cmake --build "$build" --parallel "$jobs" --target \
        rust_sto_transaction_tests rust_support_tests rust_masstree_tests \
        masstree_tx_tests masstree_tx_quality
}
storage() {
    cmake -S "$root" -B "$build/storage-tests" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DMAKO_STORAGE_TESTS_ONLY=ON
    cmake --build "$build/storage-tests" --parallel "$jobs"
    ctest --test-dir "$build/storage-tests" --output-on-failure --parallel "$jobs"
}
quality() {
    cmake --build "$build" --parallel "$jobs" --target rust_native_quality
}
runtime() {
    cmake --build "$build" --parallel "$jobs" --target \
        rust_runtime_tests rust_shard_integration_tests rust_runtime_release_tests
}
workloads() {
    cmake --build "$build" --target workload_boundary_audit
    python3 -m unittest discover -s scripts/tests -p test_workload_boundary.py
    python3 examples/run_native.py transactions --build "$build" --shards 2 --workers 3 --count 50
    python3 examples/run_native.py transactions --build "$build" --shards 2 --replicas 3 --colocated --workers 3 --count 30
    python3 examples/run_native.py micro --build "$build" --shards 2 --colocated --persistent --workers 3 --count 200
    python3 examples/run_native.py tpcc --build "$build" --shards 2 --workers 3 --count 500
    python3 examples/run_native.py tpcc --build "$build" --shards 2 --replicas 3 --workers 2 --count 50
    python3 examples/run_native.py micro --build "$build" --shards 2 --colocated --workers 2 --duration-only --seconds 2 --cpu-limit 10
}
srpc() {
    python3 src/srpc/scripts/check_native_kernels.py
    python3 -m unittest discover -s src/srpc/scripts/tests -p test_native_kernels.py
    python3 -m unittest discover -s src/srpc/scripts/tests -p test_rust_independence.py
    cargo fetch --locked --manifest-path src/srpc/Cargo.toml
    python3 src/srpc/scripts/check_rust_independence.py
}
proofs() {
    bash scripts/verify_native_sharding.sh
    bash scripts/verify_shard_host_core.sh
}
audit() {
    cmake --build "$build" --target native_storage_source_audit
    cmake --build "$build" --target rust_native_dependency_audit
}
case "${1:-all}" in
    compile|components|storage|quality|runtime|workloads|srpc|proofs|audit) "$1" ;;
    all) compile; components; storage; quality; runtime; workloads; audit ;;
    *) echo "usage: $0 [all|compile|components|storage|quality|runtime|workloads|srpc|proofs|audit]" >&2; exit 2 ;;
esac
