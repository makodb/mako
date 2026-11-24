#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if (( $# < 4 || $# > 6 )); then
    echo 'Usage: bash/shard_raft.sh SHARDS SHARD WORKERS SITE [MICRO=0] [REPLICATED=1]' >&2
    exit 2
fi
exec bash "$root/bash/shard.sh" "$1" "$2" "$3" "$4" "${5:-0}" "${6:-1}" raft
