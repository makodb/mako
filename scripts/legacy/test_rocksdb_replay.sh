#!/bin/bash
#
# Simple test script for RocksDB Replay Application
#

set -e

echo "==================================="
echo "RocksDB Replay Test"
echo "===================================="
echo ""

# Check if replay app exists
if [ ! -f "./build/rocksdb_replay_app" ]; then
    echo "Error: ./build/rocksdb_replay_app not found"
    echo "Please run 'make -j32' first"
    exit 1
fi

# Check if RocksDB data exists
USERNAME=${USER:-unknown}
ROCKSDB_DIR=$(ls -d /tmp/${USERNAME}_mako_rocksdb_shard0_leader_pid*_partition0 2>/dev/null | head -1 | sed 's/_partition0$//')

if [ -z "$ROCKSDB_DIR" ]; then
    echo "Error: No RocksDB data found"
    echo ""
    echo "Please generate data first by running a workload, for example:"
    echo "  ./examples/test_2shard_replication.sh"
    exit 1
fi

echo "Found RocksDB: $ROCKSDB_DIR"
echo ""

# Run replay
./build/rocksdb_replay_app

echo ""
echo "==================================="
echo "Test completed!"
echo "==================================="
