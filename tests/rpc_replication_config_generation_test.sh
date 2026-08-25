#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
source "${REPO_ROOT}/examples/simple_transaction_rep_port_utils.sh"

THREADS=6
SHARDS=2
TARGETS=(
    "${REPO_ROOT}/config/1leader_2followers/paxos${THREADS}_shardidx0.yml"
    "${REPO_ROOT}/config/1leader_2followers/paxos${THREADS}_shardidx1.yml"
)

for cfg in "${TARGETS[@]}"; do
    rm -f "${cfg}"
done

ensure_paxos_replication_configs "${THREADS}" "${SHARDS}"

for cfg in "${TARGETS[@]}"; do
    if [ ! -s "${cfg}" ]; then
        echo "ERROR: Expected generated config '${cfg}' to exist and be non-empty" >&2
        exit 1
    fi
done

echo "Paxos replication config generation preflight passed."
