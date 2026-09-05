#!/bin/bash

# Helpers for generating a temporary config with randomized port bases.

PORT_UTILS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${PORT_UTILS_DIR}/.."

ensure_paxos_replication_configs() {
    local nthreads="$1"
    local nshards="${2:-1}"

    if ! [[ "$nthreads" =~ ^[0-9]+$ ]] || [ "$nthreads" -le 0 ]; then
        echo "ERROR: ensure_paxos_replication_configs requires positive thread count (got '$nthreads')" >&2
        return 1
    fi
    if ! [[ "$nshards" =~ ^[0-9]+$ ]] || [ "$nshards" -le 0 ]; then
        echo "ERROR: ensure_paxos_replication_configs requires positive shard count (got '$nshards')" >&2
        return 1
    fi

    local cfg_dir="${BASE_DIR}/config/1leader_2followers"
    local generator="${cfg_dir}/generator.py"
    if [ ! -f "$generator" ]; then
        echo "ERROR: Paxos config generator not found at '$generator'" >&2
        return 1
    fi

    local shard
    local missing=0
    for ((shard = 0; shard < nshards; shard++)); do
        local cfg_file="${cfg_dir}/paxos${nthreads}_shardidx${shard}.yml"
        if [ ! -s "$cfg_file" ]; then
            missing=1
            break
        fi
    done

    if [ "$missing" -eq 1 ]; then
        echo "Generating missing Paxos configs (threads=$nthreads, shards=$nshards)..."
        if ! (cd "$cfg_dir" && python3 generator.py); then
            echo "ERROR: Failed to generate Paxos configs via '$generator'" >&2
            return 1
        fi
    fi

    for ((shard = 0; shard < nshards; shard++)); do
        local cfg_file="${cfg_dir}/paxos${nthreads}_shardidx${shard}.yml"
        if [ ! -s "$cfg_file" ]; then
            echo "ERROR: Missing required Paxos config '$cfg_file' after generation" >&2
            return 1
        fi
    done
}

pick_simple_transaction_port_base() {
    local base_min=20000
    # Keep SRPC dynamic ports out of:
    # 1) fixed Paxos/Raft control ports (45001+), and
    # 2) Linux default ephemeral range (32768+), which avoids self-collisions
    #    when worker threads open outbound TCP connections during startup.
    # With max offset 3100, base_max=28599 keeps highest port at 31699.
    local base_max=28599
    python3 - <<'PY' "$base_min" "$base_max"
import random
import socket
import sys

# Probe CONTIGUOUS per-shard windows, not spot offsets: dbtest binds
# base+id for id in [0, ~warehouses+5+num_rpc_servers) within each shard
# block (blocks at +0, +100, +1000, ... per the config layout). CI
# died on base+6 — a mid-window port a spot-offset probe never
# checked, squatted by a leftover listener from an earlier suite.
WINDOW = 40  # ports probed per shard block; covers ids with slack
BLOCK_STARTS = [0, 100, 1000, 1100, 2000, 2100, 3000, 3100]
OFFSETS = [b + i for b in BLOCK_STARTS for i in range(WINDOW)]
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
    python3 - <<'PY' "$base_port" "$src_config" "$dest_config"
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

# Pick a port base for paxos/raft replication configs.
# The replication config uses a contiguous range per shard:
#   shard i ports = base + i*1000 + cluster*100 + partition
# where cluster ∈ {0=localhost, 1=p1, 2=p2, 3=learner} and partition ∈ [0, nthreads).
# Probe leader port of each cluster on each shard (so 4 * nshards bind attempts).
# Keeps the range out of the simpleTransaction band (20000-31699). NOTE: unlike
# that band, this one sits INSIDE the Linux default ephemeral range
# (32768-60999) — it cannot fit below 32768 because the +10000 heartbeat ports
# would land in the simpleTransaction band. Bind-probing only proves a port is
# free at PICK time; a later outbound connect() can still steal it as its
# source port. In CI the container reserves these bands from the ephemeral
# allocator (net.ipv4.ip_local_reserved_ports, see .github/workflows/ci.yml);
# elsewhere the srpc self-connect guard removes the worst failure mode.
pick_paxos_replication_port_base() {
    local nshards="${1:-2}"
    local nthreads="${2:-3}"
    python3 - <<'PY' "$nshards" "$nthreads"
import random
import socket
import sys

NSHARDS = int(sys.argv[1])
NTHREADS = int(sys.argv[2])
# Each site spawns a paxos listener at `port` AND a heartbeat listener at
# `port + 10000` (PaxosWorker::CtrlPortDelta in deptran/paxos_worker.h).
# Both must fit in the valid TCP port range; cap BASE_MAX accordingly.
CTRL_PORT_DELTA = 10000
PORT_MAX = 65535
# Lower bound: stay above simpleTransaction's 20000-31699 range and the Linux
# ephemeral floor (32768 by default).
BASE_MIN = 40000
# Per shard we use a 1000-port window (4 cluster decades * 100 + nthreads spread).
# Reserve 400 ports per shard for the cluster offsets + per-partition spread.
BASE_MAX = PORT_MAX - CTRL_PORT_DELTA - (NSHARDS * 1000 + 400)

def port_free(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("0.0.0.0", port))
    except OSError:
        return False
    finally:
        s.close()
    return True

def probe(base):
    # Probe the leader port of each cluster on each shard, plus the matching
    # heartbeat port (paxos + 10000). Catches both the listen-port collision
    # AND the heartbeat-port collision in one pass.
    for sh in range(NSHARDS):
        for cl in (0, 100, 200, 300):
            p = base + sh * 1000 + cl
            if not port_free(p):
                return False
            if not port_free(p + CTRL_PORT_DELTA):
                return False
    return True

for _ in range(2000):
    base = random.randint(BASE_MIN, BASE_MAX)
    if probe(base):
        print(base)
        sys.exit(0)
sys.exit(1)
PY
}

# Rewrite a paxos/raft config's `site.server` port assignments by applying a
# uniform delta = new_base - existing_base, where existing_base is the
# minimum port in the source file. The site name → port map structure stays
# intact; only the port numbers shift.
write_paxos_replication_config() {
    local new_base=$1
    local src_config=$2
    local dest_config=$3
    python3 - <<'PY' "$new_base" "$src_config" "$dest_config"
import sys
import yaml

new_base = int(sys.argv[1])
src = sys.argv[2]
dest = sys.argv[3]

data = yaml.safe_load(open(src, "r"))
servers = data["site"]["server"]

# Find the minimum port — this is the per-shard base (e.g. 45001 for paxos
# shard 0, 46001 for shard 1, 27001 for raft shard 0).
min_port = min(int(t.rsplit(":", 1)[1]) for row in servers for t in row)
delta = new_base - min_port

for row in servers:
    for i, t in enumerate(row):
        name, p = t.rsplit(":", 1)
        row[i] = "%s:%d" % (name, int(p) + delta)

# Preserve the original yaml-cpp-friendly flow style for nested server lists
# (`- [s101:..., s201:...]`) instead of PyYAML's default block style
# (`- - s101:...`). yaml-cpp parses both equivalently, but we want the diff
# vs the source file to be minimal in CI logs.
class FlowList(list):
    pass

def _flow_repr(dumper, value):
    return dumper.represent_sequence("tag:yaml.org,2002:seq", value, flow_style=True)

yaml.add_representer(FlowList, _flow_repr)

data["site"]["server"] = [FlowList(row) for row in servers]

with open(dest, "w") as f:
    yaml.dump(data, f, sort_keys=False, default_flow_style=False)
PY
}

# Materialize randomized paxos/raft configs into a tmp dir.
# Returns the tmp dir path; caller exports MAKO_PAXOS_CONFIG_DIR so shard.sh
# picks the rebased configs instead of the hardcoded ones in
# config/1leader_2followers/.
#
# Each shard's source config has its own base ($base+0, $base+1000, ...),
# so we pick one global $base and pass $base + shard_idx*1000 as the target
# for each shard's file.
make_paxos_replication_configs() {
    local nshards=$1
    local nthreads=$2
    local replication_type="${3:-paxos}"
    local base_port
    base_port=$(pick_paxos_replication_port_base "$nshards" "$nthreads")
    if [ -z "$base_port" ]; then
        echo "ERROR: Failed to pick a paxos/raft port base after 2000 attempts" >&2
        return 1
    fi
    local tmp_dir
    tmp_dir=$(mktemp -d /tmp/mako_paxos_cfg_XXXX)
    local cfg_dir="${BASE_DIR}/config/1leader_2followers"
    for ((sh = 0; sh < nshards; sh++)); do
        local src="${cfg_dir}/${replication_type}${nthreads}_shardidx${sh}.yml"
        local dest="${tmp_dir}/${replication_type}${nthreads}_shardidx${sh}.yml"
        if [ ! -f "$src" ]; then
            echo "ERROR: source replication config not found: $src" >&2
            rm -rf "$tmp_dir"
            return 1
        fi
        local shard_base=$((base_port + sh * 1000))
        if ! write_paxos_replication_config "$shard_base" "$src" "$dest"; then
            echo "ERROR: Failed to rebase $src to base $shard_base" >&2
            rm -rf "$tmp_dir"
            return 1
        fi
    done
    echo "$tmp_dir"
}
