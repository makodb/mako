#!/bin/bash
#sudo cgdelete -g cpuset:/cpulimit 2>/dev/null || true
#sudo cgcreate -t $USER:$USER -a $USER:$USER -g cpuset:/cpulimit

# Source common utilities
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/util.sh"

# Set LD_LIBRARY_PATH to find shared libraries (libtxlog.so, etc.)
export LD_LIBRARY_PATH="$(pwd)/build:${LD_LIBRARY_PATH}"

nshard=$1
shard=$2
trd=$3
cluster=$4
is_micro=$5
is_replicated=$6
let up=trd+3
#sudo cgset -r cpuset.mems=0 cpulimit
#sudo cgset -r cpuset.cpus=0-$up cpulimit
mkdir -p results
path=$(pwd)/src/mako

# Read GDB setting from ~/.makorc
use_gdb=$(read_makorc_value "use_gdb_in_shard" "0")
# GDB batch mode setup
GDB_CMD_FILE="./bash/shard_gdb.txt"
GDB_LOG_DIR="./crash_logs"
# Generate unique filename with all parameters and a 6-character random string
RAND_STR=$(head -c 100 /dev/urandom | tr -dc 'a-z0-9' | head -c 6)
GDB_LOG_NAME="gdb_nshard${nshard}_shard${shard}_trd${trd}_${cluster}_micro${is_micro}_rep${is_replicated}_${RAND_STR}.log"
if [ "$use_gdb" == "1" ]; then
    mkdir -p "$GDB_LOG_DIR"
    # Create gdb command file for this shard
    cat > "${GDB_CMD_FILE}" <<EOF
set pagination off
set logging file ${GDB_LOG_DIR}/${GDB_LOG_NAME}
set logging overwrite on
set logging on
run
echo \n========== CRASH DETECTED ==========\n
echo Shard ${shard} crashed!\n
echo ====================================\n
thread apply all bt full
quit
EOF
fi

# Build the base command
CMD="./build/dbtest --num-threads $trd --shard-index $shard --shard-config $path/config/local-shards$nshard-warehouses$trd.yml -P $cluster"

# Add --is-micro flag if enabled (value is 1)
if [ "$is_micro" == "1" ]; then
    CMD="$CMD --is-micro"
fi

# Add paxos config and --is-replicated flag only if replication is enabled
if [ "$is_replicated" == "1" ]; then
    CMD="$CMD -F config/1leader_2followers/paxos${trd}_shardidx${shard}.yml -F config/occ_paxos.yml --is-replicated"
fi

# Print configuration
echo "========================================="
echo "Configuration:"
echo "========================================="
echo "  Number of shards:  $nshard"
echo "  Shard index:       $shard"
echo "  Number of threads: $trd"
echo "  Cluster:           $cluster"
echo "  Micro benchmark:   $([ "$is_micro" == "1" ] && echo "enabled" || echo "disabled")"
echo "  Replicated mode:   $([ "$is_replicated" == "1" ] && echo "enabled" || echo "disabled")"
echo "========================================="

# Execute command (with or without gdb)
if [ "$use_gdb" == "1" ]; then
    echo "Running under gdb batch mode..."
    echo "GDB log will be saved to: ${GDB_LOG_DIR}/${GDB_LOG_NAME}"
    gdb -batch -x "${GDB_CMD_FILE}" --args $CMD
else
    eval $CMD
fi 
