#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run CI in the build container with ./docker_build.sh ci <target>.' >&2
    exit 1
fi
build="${BUILD_DIR:-build}"
launch() { python3 examples/run_native.py "$@" --build "$build"; }
case "${1:-}" in
    compile) bash ci/native_rust.sh compile ;;
    nativeRust) bash ci/native_rust.sh all ;;
    upper) bash ci/upper.sh ;;
    rustStorageGates|rustStoBoundaryBinaries) bash ci/native_rust.sh components ;;
    nativeRuntime|nativeShardingSmoke|clientServer|raftKillTest|shard1ReplicationRaftRestart)
        bash ci/native_rust.sh runtime ;;
    nativeShardingProof) bash ci/native_rust.sh proofs ;;
    srpcTests) bash ci/native_rust.sh srpc ;;
    rocksdbRaft|rocksdbTests|raftLabTest) bash ci/rocksdb_raft.sh ;;
    simpleTransaction) launch transactions --workers 3 --count 100 ;;
    shardNoReplication) launch tpcc --shards 2 --workers 3 --count 500 ;;
    shard1ReplicationRaft) launch tpcc --replicas 3 --workers 2 --count 50 ;;
    shard2ReplicationRaft) launch tpcc --shards 2 --replicas 3 --workers 2 --count 50 ;;
    shard1ReplicationSimpleRaft) launch transactions --replicas 3 --workers 3 --count 50 ;;
    shard2ReplicationSimpleRaft) launch transactions --shards 2 --replicas 3 --workers 3 --count 50 ;;
    multiShardSingleProcess|shard2SingleProcess)
        launch tpcc --shards 2 --colocated --workers 3 --count 500 ;;
    cpuThrottlingScaling) bash ci/test_cpu_throttling_scaling.sh ;;
    cleanup) echo 'Native launchers stop their own processes. Build and failure artifacts are retained.' ;;
    all)
        bash ci/upper.sh
        bash ci/native_rust.sh proofs
        bash ci/native_rust.sh srpc
        bash ci/native_rust.sh all
        bash ci/rocksdb_raft.sh
        ;;
    *) echo "Unknown CI target: ${1:-}. Use nativeRust, nativeRuntime, nativeShardingProof, srpcTests, rocksdbRaft, compile or all." >&2; exit 2 ;;
esac
