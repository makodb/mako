#!/usr/bin/env bash
# Launch one native Rust benchmark shard using an existing topology file.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
if (( $# < 4 || $# > 7 )); then
    echo "Usage: bash/shard.sh SHARDS SHARD WORKERS SITE [MICRO=0] [REPLICATED=0] [raft]" >&2
    exit 2
fi
shards=$1
shard=$2
workers=$3
site=$4
micro=${5:-0}
replicated=${6:-0}
protocol=${7:-raft}
for number in "$shards" "$workers"; do
    [[ "$number" =~ ^[1-9][0-9]*$ ]] || { echo 'SHARDS and WORKERS must be positive integers' >&2; exit 2; }
done
[[ "$micro" =~ ^[01]$ && "$replicated" =~ ^[01]$ ]] || { echo 'MICRO and REPLICATED must be 0 or 1' >&2; exit 2; }
[[ "$protocol" == raft ]] || { echo 'Only Raft replication is supported' >&2; exit 2; }
config=${MAKO_CONFIG:-src/mako/config/local-shards${shards}-warehouses${workers}.yml}
binary=${MAKO_BENCH_BIN:-${BUILD_DIR:-build}/mako_bench}
args=(--shard-config "$config" --num-threads "$workers" --site-name "$site")
if [[ "$shard" == all || "$shard" == *,* ]]; then
    args+=(--local-shards "$shard")
else
    args+=(--shard-index "$shard")
fi
[[ "$micro" == 0 ]] || args+=(--is-micro)
[[ -z "${MAKO_RUNTIME_SECONDS:-}" ]] || args+=(--runtime "$MAKO_RUNTIME_SECONDS")
[[ -z "${MAKO_CPU_LIMIT:-}" ]] || args+=(--cpu-limit "$MAKO_CPU_LIMIT")
[[ -z "${MAKO_THROTTLE_CYCLE_MS:-}" ]] || args+=(--throttle-cycle "$MAKO_THROTTLE_CYCLE_MS")
[[ -z "${MAKO_STARTUP_TIMEOUT_SEC:-}" ]] || args+=(--startup-timeout-sec "$MAKO_STARTUP_TIMEOUT_SEC")
[[ "${MAKO_MIGRATION:-0}" != 1 ]] || args+=(--migration)
if [[ -n "${MAKO_DATA_DIR:-}" ]]; then
    args+=(--data-dir "$MAKO_DATA_DIR")
    [[ "${MAKO_CREATE:-0}" != 1 ]] || args+=(--create)
fi
if [[ "$replicated" == 1 ]]; then
    : "${MAKO_DATA_DIR:?Set MAKO_DATA_DIR for native Raft storage; set MAKO_CREATE=1 only to allow creation}"
    directory=${MAKO_RAFT_CONFIG_DIR:-config/1leader_2followers}
    args+=(--replication raft)
    if [[ "$shard" == all || "$shard" == *,* ]]; then
        selection=$shard
        [[ "$selection" != all ]] || selection=$(seq -s, 0 "$((shards - 1))")
        IFS=, read -ra owners <<< "$selection"
        for owner in "${owners[@]}"; do
            args+=(--raft-config "${owner}=${directory}/raft${workers}_shardidx${owner}.yml")
        done
    else
        args+=(--raft-config "${directory}/raft${workers}_shardidx${shard}.yml")
    fi
fi
[[ "${MAKO_PRINT_CONFIG:-0}" != 1 ]] || args+=(--print-config)
exec "$binary" "${args[@]}"
