# YAML Configuration Reference

## 1. Mode Configuration

Mode configs specify the concurrency control and atomic broadcast
protocols.

### 1.1 Raft Mode (`config/occ_raft.yml`)

```yaml
cc: occ            # Concurrency control: optimistic
ab: raft           # Atomic broadcast: Raft
read_only: false   # Read-only transaction optimisation
batch: false       # Batch mode for transactions
retry: false       # Auto-retry on abort
ongoing: 1         # Ongoing transaction limit
```

### 1.2 Paxos Mode (`config/occ_paxos.yml`)

```yaml
cc: occ            # Same CC as Raft
ab: paxos          # Atomic broadcast: Multi-Paxos
read_only: false
batch: false
retry: false
ongoing: 1
```

### 1.3 Other Raft Mode Variants

| File | `cc` | `ab` | Use Case |
|------|------|------|----------|
| `none_raft.yml` | `none` | `raft` | Testing without CC |
| `notx_raft.yml` | `notx` | `fpga_raft` | No transactions |

The former Rule mode and its Raft configuration are retired. The generic
Jetpack recovery subsystem remains legacy code pending a separate audit and is
not a supported configuration.

### 1.4 Mode Config Fields

| Field | Type | Values | Description |
|-------|------|--------|-------------|
| `cc` | string | `occ`, `none`, `notx` | Concurrency control protocol |
| `ab` | string | `multi_paxos`, `raft`, `fpga_raft` | Atomic broadcast protocol |
| `read_only` | bool | `true`/`false` | Enable read-only optimisation |
| `batch` | bool | `true`/`false` | Enable transaction batching |
| `retry` | bool | `true`/`false` | Auto-retry aborted transactions |
| `ongoing` | int | >= 1 | Maximum concurrent in-flight transactions |

## 2. Replication Group Configuration

### 2.1 Raft 6-Partition Shard 0 (`config/1leader_2followers/raft6_shardidx0.yml`)

```yaml
site:
  - host: localhost
    port: 27001   # Partition 0, replica 1 (preferred leader)
    partition: [s101]
  - host: localhost
    port: 27002   # Partition 0, replica 2
    partition: [s102]
  ...
  - host: localhost
    port: 27006   # Partition 5, replica 1
    partition: [s106]
  - host: p1
    port: 27101   # Partition 0, replica 2 (follower)
    partition: [s201]
  ...
  - host: p2
    port: 27201   # Partition 0, replica 3 (follower)
    partition: [s301]
  ...
```

Each partition has 3 replicas (localhost, p1, p2).  The first replica
in each group is the preferred leader.

### 2.2 Structure

The `site` array defines all replicas in the cluster.  Each entry has:

| Field | Type | Description |
|-------|------|-------------|
| `host` | string | Hostname (maps to `--P` arg: localhost, p1, p2) |
| `port` | int | RPC port number |
| `partition` | list | Partition identifier (e.g., `[s101]`) |

### 2.3 Partition Naming Convention

Partition IDs follow the format `s{R}{PP}` where:
- `R` = replica number (1 = leader, 2 = follower 1, 3 = follower 2)
- `PP` = partition index (01-06)

Examples:
- `s101` = replica 1 (leader), partition 0
- `s206` = replica 2 (follower), partition 5
- `s303` = replica 3 (follower), partition 2

## 3. Port Allocation Scheme

### 3.1 Port Ranges

| Protocol | Shard 0 | Shard 1 | Purpose |
|----------|---------|---------|---------|
| Raft | 27001-27306 | 27002-27302 | CI integration tests |
| Paxos | 17001-17301 | 17002-17302 | CI integration tests |
| Standalone tests | 9001-9005 | N/A | 5-node lab tests |
| CI misc | 7001-8006, 31000-31100 | — | Process cleanup polling |

### 3.2 Port Allocation Formula

For CI tests with `trd` threads per shard:

```
base_port = {27000 for Raft, 17000 for Paxos}
port = base_port + (shard_index) + (replica_number * 100) + partition_index

Example (Raft, shard 0, replica 1, partition 3):
  port = 27000 + 0 + 100 + 3 = 27103
```

### 3.3 How to Avoid Port Conflicts

The 2-shard tests insert a 5-second delay between starting shard 0 and
shard 1 to avoid port binding conflicts.  Shard 0 uses
`raft6_shardidx0.yml` (port suffix `...01`) and shard 1 uses
`raft6_shardidx1.yml` (port suffix `...02`).

## 4. Shard Configuration

### 4.1 Shard Config (`src/mako/config/local-shards1-warehouses6.yml`)

```yaml
shards:
  - shard_id: 0
    warehouses: 6
```

### 4.2 Multi-Shard Config (`src/mako/config/local-shards2-warehouses6.yml`)

```yaml
shards:
  - shard_id: 0
    warehouses: 6
  - shard_id: 1
    warehouses: 6
```

### 4.3 Fields

| Field | Type | Description |
|-------|------|-------------|
| `shard_id` | int | Zero-based shard index |
| `warehouses` | int | TPC-C warehouses per shard (also matches thread count) |

## 5. Standalone Test Configuration

### 5.1 Lab Test Config (`config/raft_lab_test.yml`)

```yaml
cc: none
ab: raft
# ... commented test parameters:
# n_test_servers: 5
# election_timeout_ms: 5000
# heartbeat_interval_ms: 150
# n_partitions: 3
# log_entries_per_partition: 100
```

This config is used by `simpleRaft` and the standalone test binaries.
It specifies `cc: none` because the standalone tests do not use
transactional concurrency control.

### 5.2 Test Cluster Configs

| File | Replicas | Partitions | Use Case |
|------|----------|------------|----------|
| `1c1s3r1p_cluster_test.yml` | 3 | 1 | CI integration tests |
| `1c1s5r1p_cluster_test.yml` | 5 | 1 | Standalone lab tests |

## 6. Switching Between Paxos and Raft

### 6.1 Via `shard_raft.sh` (Raft-dedicated)

```bash
bash/shard_raft.sh $nshard $shard $trd $cluster $is_micro $is_replicated
# Always uses: --replication raft, occ_raft.yml, raft*_shardidx*.yml
```

### 6.2 Via `shard.sh` (Unified)

```bash
bash/shard.sh $nshard $shard $trd $cluster $is_micro $is_replicated raft
# 7th arg: "raft" or "paxos" (default: paxos)
```

### 6.3 Via Mode Config

The `ab` field in the mode config selects the protocol:

```yaml
# For Raft:
ab: raft

# For Paxos:
ab: paxos
```

### 6.4 Via Command-Line Flag

The `--replication` flag on `dbtest` overrides config auto-detection:

```bash
./build/dbtest --replication raft ...
./build/dbtest --replication paxos ...
```

## 7. Config File Selection by `shard_raft.sh`

The shard launcher selects config files based on arguments:

```bash
trd=${1:-6}    # Thread count
shard=${2:-0}  # Shard index

# Site topology:
-F config/1leader_2followers/raft${trd}_shardidx${shard}.yml

# Mode config:
-F config/occ_raft.yml

# Shard config:
--shard-config src/mako/config/local-shards${nshard}-warehouses${trd}.yml
```

| Argument | Resolves To |
|----------|-------------|
| `trd=6, shard=0` | `raft6_shardidx0.yml` |
| `trd=6, shard=1` | `raft6_shardidx1.yml` |
| `trd=2, shard=0` | `raft2_shardidx0.yml` |
