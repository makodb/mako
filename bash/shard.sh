#!/bin/bash
#sudo cgdelete -g cpuset:/cpulimit 2>/dev/null || true
#sudo cgcreate -t $USER:$USER -a $USER:$USER -g cpuset:/cpulimit

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/util.sh"

# Set LD_LIBRARY_PATH to find shared libraries (libtxlog.so, etc.)
export LD_LIBRARY_PATH="$(pwd)/${BUILD_DIR:-build}:${LD_LIBRARY_PATH}"

nshard=$1
shard=$2
trd=$3
cluster=$4
is_micro=$5
is_replicated=$6
replication_type=${7:-paxos}  # Default to paxos if not specified
let up=trd+3
#sudo cgset -r cpuset.mems=0 cpulimit
#sudo cgset -r cpuset.cpus=0-$up cpulimit
mkdir -p results
path=$(pwd)/src/mako

# Build the base command
config_path="$path/config/local-shards${nshard}-warehouses${trd}.yml"
if [ -n "$MAKO_CONFIG" ]; then
    config_path="$MAKO_CONFIG"
fi
CMD="./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-index $shard --shard-config $config_path -P $cluster"

THROTTLE_ARGS="$(mako_dbtest_throttle_args)" || exit 1
if [ -n "$THROTTLE_ARGS" ]; then
    CMD="$CMD$THROTTLE_ARGS"
fi

# Add --is-micro flag if enabled (value is 1)
if [ "$is_micro" == "1" ]; then
    CMD="$CMD --is-micro"
fi

# Add paxos config and --is-replicated flag only if replication is enabled
if [ "$is_replicated" == "1" ]; then
    replication_type_normalized="$(echo "$replication_type" | tr '[:upper:]' '[:lower:]')"

    # Pick replication-specific config files.
    # MAKO_PAXOS_CONFIG_DIR is set by the replication test wrappers when they
    # materialize a randomized-port copy of the config into a tmp dir; fall back
    # to the hardcoded path otherwise.
    REPLICATION_CONFIG_DIR="${MAKO_PAXOS_CONFIG_DIR:-config/1leader_2followers}"
    if [ "$replication_type_normalized" == "raft" ]; then
        MODE_CONFIG="config/raft.yml"
        REPLICATION_CONFIG="${REPLICATION_CONFIG_DIR}/raft${trd}_shardidx${shard}.yml"
    else
        MODE_CONFIG="config/paxos.yml"
        REPLICATION_CONFIG="${REPLICATION_CONFIG_DIR}/paxos${trd}_shardidx${shard}.yml"
    fi

    if [ ! -f "$REPLICATION_CONFIG" ]; then
        echo "Error: replication config not found: $REPLICATION_CONFIG"
        exit 1
    fi
    if [ ! -f "$MODE_CONFIG" ]; then
        echo "Error: replication mode config not found: $MODE_CONFIG"
        exit 1
    fi

    CMD="$CMD -F $REPLICATION_CONFIG -F $MODE_CONFIG --is-replicated --replication=$replication_type_normalized"
fi

# Print configuration
echo "========================================="
echo "Configuration:"
echo "========================================="
if [ -n "${MAKO_CPU_LIMIT:-}" ]; then
    cpu_throttle_label="${MAKO_CPU_LIMIT}%"
else
    cpu_throttle_label="disabled"
fi
echo "  Number of shards:  $nshard"
echo "  Shard index:       $shard"
echo "  Number of threads: $trd"
echo "  Cluster:           $cluster"
echo "  Micro benchmark:   $([ "$is_micro" == "1" ] && echo "enabled" || echo "disabled")"
echo "  Replicated mode:   $([ "$is_replicated" == "1" ] && echo "enabled" || echo "disabled")"
echo "  Replication type:  $replication_type"
echo "  CPU throttle:      ${cpu_throttle_label} (cycle=${MAKO_THROTTLE_CYCLE_MS:-default}ms)"
echo "========================================="

# Execute command (with or without gdb based on GDB_PREFIX from util.sh)
if [ "$GDB_ENABLED" == "1" ]; then
    echo "Running under gdb batch mode..."
fi
$GDB_PREFIX $CMD
