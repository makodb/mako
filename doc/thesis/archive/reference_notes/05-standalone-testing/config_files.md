# Test Configuration YAML Files

## 1. Overview

The Raft implementation uses YAML configuration files to define cluster
topology, port assignments, and protocol modes.  This chapter documents
the configuration files used by the standalone test framework and
compares them to the production Mako-Raft and Mako-Paxos configurations.

## 2. Test Configuration: `raft_lab_test.yml`

**File**: `config/raft_lab_test.yml`

```yaml
mode:
  cc: none
  ab: raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1

site:
  server:
    - ["s101:9000", "s102:9001", "s103:9002", "s104:9003", "s105:9004"]
  client:
    - ["c01"]

process:
  s101: localhost
  s102: localhost
  s103: localhost
  s104: localhost
  s105: localhost
  c01: localhost

host:
  localhost: 127.0.0.1
```

### 2.1 Field-by-Field Explanation

#### `mode` Section

| Field | Value | Meaning |
|-------|-------|---------|
| `cc` | `none` | No concurrency control — tests exercise only Raft consensus |
| `ab` | `raft` | Atomic broadcast protocol is Raft (triggers `DISPATCH_RAFT_OR_PAXOS` to route to `raft_impl`) |
| `read_only` | `occ` | Read-only transaction protocol (not exercised in tests) |
| `batch` | `false` | Batching disabled (each command replicated individually) |
| `retry` | `20` | Maximum number of transaction retries (not exercised in tests) |
| `ongoing` | `1` | Maximum concurrent operations per client (not exercised in tests) |

The key field is `ab: raft`.  This is what `detect_replication_type_from_config()`
scans for to set `g_replication_type = RAFT` before calling `setup()`.

#### `site` Section

The `site` section defines the cluster topology:

```yaml
site:
  server:
    - ["s101:9000", "s102:9001", "s103:9002", "s104:9003", "s105:9004"]
  client:
    - ["c01"]
```

- **One partition**: A single array entry means one Raft group (one shard)
- **5 servers**: Matches `NSERVERS` constant in `testconf.h`
- **Port range**: 9000-9004 (simple, low-numbered ports for isolated testing)
- **1 client**: Required by the framework but not used by tests (tests
  drive commands directly via `RaftTestConfig::Start()`)

Each server entry is `"name:port"`.  The name maps through `process` →
`host` to resolve to an IP address.

#### `process` Section

Maps site names to process names:

```yaml
process:
  s101: localhost
  s102: localhost
  ...
```

All 5 servers and the client run as `localhost` — single-machine execution.

#### `host` Section

Maps process names to IP addresses:

```yaml
host:
  localhost: 127.0.0.1
```

All processes resolve to `127.0.0.1`.

### 2.2 Port Allocation

The test configuration uses ports 9000-9004 for the Raft group.  The
heartbeat/control port is computed as `base_port + CtrlPortDelta` where
`CtrlPortDelta = 10000`, giving ports 19000-19004 for internal Raft
communication.

## 3. CI Test Cluster Configs

### 3.1 `1c1s3r1p_cluster_test.yml` (3-Replica Raft Test)

**File**: `config/1c1s3r1p_cluster_test.yml`

```yaml
site:
  server:
    - ["localhost:38100", "p1:38101", "p2:38102"]
  client:
    - ["c01"]
```

- **3 replicas**: Minimum viable Raft cluster (tolerates 1 failure)
- **Port range**: 38100-38102 (in the 38xxx Raft test range)
- **1 partition**: Single shard

Used by `ci_mako_raft.sh` for 1-shard Raft replication tests with `dbtest`.

### 3.2 `1c1s5r1p_cluster_test.yml` (5-Replica Raft Test)

**File**: `config/1c1s5r1p_cluster_test.yml`

```yaml
site:
  server:
    - ["localhost:38101", "p1:38102", "p2:38103", "p3:38104", "p4:38105"]
  client:
    - ["c01"]
```

- **5 replicas**: Same as standalone test framework
- **Port range**: 38101-38105

### 3.3 `1c1s3r3p_cluster_test.yml` (3-Replica, 3-Partition)

Used for multi-shard Raft testing.  Contains 3 partitions each with 3
replicas, enabling cross-shard transaction testing.

## 4. Production Raft Config: `raft6_shardidx0.yml`

**File**: `config/1leader_2followers/raft6_shardidx0.yml`

```yaml
site:
  server:
    - ["s101:27001", "s201:27101", "s301:27201"]
    - ["s102:27002", "s202:27102", "s302:27202"]
    - ["s103:27003", "s203:27103", "s303:27203"]
    - ["s104:27004", "s204:27104", "s304:27204"]
    - ["s105:27005", "s205:27105", "s305:27205"]
    - ["s106:27006", "s206:27106", "s306:27206"]
```

- **3 replicas per partition**: Leader + 2 followers
- **6 partitions**: 6 warehouse shards (matching thread count in Mako)
- **Port range**: 27xxx (Raft production range)
- **No learner column**: Unlike Paxos configs which have a 4th column
  for the learner

## 5. Production Paxos Config: `paxos6_shardidx0.yml` (Comparison)

**File**: `config/1leader_2followers/paxos6_shardidx0.yml`

```yaml
site:
  server:
    - ["s101:17001", "s201:17101", "s301:17201", "s401:17301"]
    - ["s102:17002", "s202:17102", "s302:17202", "s402:17302"]
    ...
```

- **4 sites per partition**: Leader + 2 followers + 1 learner
- **6 partitions**: Same shard structure
- **Port range**: 17xxx (Paxos production range)
- **Learner (s4xx)**: Paxos-specific role, not present in Raft

## 6. Mode Config Files

### 6.1 `occ_raft.yml`

```yaml
mode:
  cc: occ
  ab: raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1
```

Used by `shard.sh` when `replication_type == raft`.  The key difference
from `raft_lab_test.yml` is `cc: occ` instead of `cc: none` — production
runs use OCC for concurrency control alongside Raft replication.

### 6.2 `occ_paxos.yml`

```yaml
mode:
  cc: occ
  ab: multi_paxos
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1
```

The Paxos equivalent.  Note `ab: multi_paxos` vs `ab: raft`.

## 7. Comparison: Test vs Production

| Aspect | Test (`raft_lab_test.yml`) | Production (`raft6_shardidx0.yml` + `occ_raft.yml`) |
|--------|--------------------------|-----------------------------------------------------|
| Replicas | 5 | 3 |
| Partitions | 1 | 6 (typical) |
| Concurrency control | `none` | `occ` |
| Atomic broadcast | `raft` | `raft` |
| Port range | 9000-9004 | 27001-27206 |
| Ctrl port delta | +10000 | +10000 |
| Learner | None | None |
| Processes | All localhost | Potentially distributed (localhost/p1/p2) |
| Client | 1 (unused) | 1+ (active) |

### 7.1 Why 5 Replicas in Tests?

The test framework uses 5 replicas (vs 3 in production) to enable richer
fault-tolerance scenarios:

- **2 failures tolerated** vs 1 in production
- Enables `testBackup` (3 vs 2 quorum split)
- Enables `testFigure8` (complex partition with 3 sub-groups)
- Matches the standard Raft paper examples

### 7.2 Why `cc: none` in Tests?

Setting `cc: none` isolates the Raft consensus layer:

- No transaction processing overhead
- No Masstree operations
- No OCC validation
- Failures are attributable to Raft logic, not transaction bugs

## 8. Port Range Allocation Summary

| Usage | Port Range | Ctrl Port Range | Config Files |
|-------|-----------|-----------------|-------------|
| Standalone Raft tests | 9000-9004 | 19000-19004 | `raft_lab_test.yml` |
| Paxos production | 17001-17999 | 27001-27999 | `paxos*_shardidx*.yml` |
| Raft production | 27001-27999 | 37001-37999 | `raft*_shardidx*.yml` |
| Raft CI tests | 38100-38199 | 48100-48199 | `*_cluster_test.yml` |

Port ranges are deliberately non-overlapping so that Paxos tests, Raft
tests, and standalone tests can run concurrently on the same machine
without port conflicts (documented in `challenges.md` as a resolved
integration issue).

## 9. Config Naming Convention

The configuration file naming follows a structured pattern:

```
{n_clients}c{n_shards}s{n_replicas}r{n_partitions}p[_cluster_test].yml
```

Examples:
- `1c1s3r1p.yml`: 1 client, 1 shard, 3 replicas, 1 partition
- `1c1s5r1p_cluster_test.yml`: Same but in test port range
- `1c1s3r3p_cluster_test.yml`: 3 partitions (multi-shard test)

For the `1leader_2followers/` directory:
```
{protocol}{n_threads}_shardidx{shard_index}.yml
```

Examples:
- `paxos6_shardidx0.yml`: Paxos, 6 threads, shard 0
- `raft6_shardidx0.yml`: Raft, 6 threads, shard 0
