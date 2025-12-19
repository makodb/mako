#!/bin/bash
nshard=$1
shard=$2
trd=$3
cluster=$4
is_micro=$5
is_replicated=$6
let up=trd+3
mkdir -p results
path=$(pwd)/src/mako

# Build the command with Raft config instead of Paxos
CMD="./build/dbtest --num-threads $trd --shard-index $shard --shard-config $path/config/local-shards$nshard-warehouses$trd.yml -F config/1leader_2followers/raft$trd\_shardidx$shard.yml -F config/occ_raft.yml -P $cluster"

# Add --is-micro flag if enabled (value is 1)
if [ "$is_micro" == "1" ]; then
    CMD="$CMD --is-micro"
fi

# Add --is-replicated flag if enabled (value is 1)
if [ "$is_replicated" == "1" ]; then
    CMD="$CMD --is-replicated"
fi

# Print configuration
echo "========================================="
echo "Configuration (Raft):"
echo "========================================="
echo "  Number of shards:  $nshard"
echo "  Shard index:       $shard"
echo "  Number of threads: $trd"
echo "  Cluster:           $cluster"
echo "  Micro benchmark:   $([ "$is_micro" == "1" ] && echo "enabled" || echo "disabled")"
echo "  Replicated mode:   $([ "$is_replicated" == "1" ] && echo "enabled" || echo "disabled")"
echo "========================================="

eval $CMD
