#!/bin/bash

# Helpers for generating a temporary config with randomized port bases.

PORT_UTILS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${PORT_UTILS_DIR}/.."

pick_simple_transaction_port_base() {
    local transport="${MAKO_TRANSPORT:-rrr}"
    local base_min=20000
    local base_max=45000
    if [ "$transport" = "erpc" ]; then
        base_min=31000
        base_max=37899
    fi
    python - <<'PY' "$base_min" "$base_max"
import random
import socket
import sys

OFFSETS = [0, 100, 1000, 1100, 2000, 2100, 3000, 3100]
BASE_MIN = int(sys.argv[1])
BASE_MAX = int(sys.argv[2])

def port_free(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", port))
    except OSError:
        return False
    finally:
        sock.close()
    return True

for _ in range(2000):
    base = random.randint(BASE_MIN, BASE_MAX)
    if all(port_free(base + offset) for offset in OFFSETS):
        print(base)
        raise SystemExit(0)
print(random.randint(BASE_MIN, BASE_MAX))
raise SystemExit(0)
PY
}

write_simple_transaction_config() {
    local base_port=$1
    local src_config=$2
    local dest_config=$3
    python - <<'PY' "$base_port" "$src_config" "$dest_config"
import sys
import yaml

base_port = int(sys.argv[1])
src_config = sys.argv[2]
dest_config = sys.argv[3]

data = yaml.safe_load(open(src_config, "r"))
delta = base_port - 31000
for group in ("localhost", "p1", "p2", "learner"):
    if group not in data:
        continue
    for node in data[group]:
        if "port" in node:
            node["port"] = int(node["port"]) + delta

with open(dest_config, "w") as f:
    yaml.safe_dump(data, f, sort_keys=False)
PY
}

make_simple_txn_rep_config() {
    local nshards=$1
    local nthreads=$2
    local base_port
    base_port=$(pick_simple_transaction_port_base)
    if [ -z "$base_port" ]; then
        echo "ERROR: Failed to select a base port for simpleTransactionRep" >&2
        return 1
    fi
    local src_config="${BASE_DIR}/src/mako/config/local-shards${nshards}-warehouses${nthreads}.yml"
    local tmp_config
    tmp_config=$(mktemp /tmp/mako_simple_txn_rep_XXXX.yml)
    write_simple_transaction_config "$base_port" "$src_config" "$tmp_config"
    echo "$tmp_config"
}
