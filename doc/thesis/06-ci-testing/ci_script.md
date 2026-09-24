# CI Script Documentation: `ci_mako_raft.sh`

## 1. Overview

`ci/ci_mako_raft.sh` is the Continuous Integration entry point for
Mako-Raft integration tests.  It mirrors the structure of `ci/ci.sh`
(the primary Mako-Paxos CI script) but tests only the Raft replication
path.  The Raft test commands are also integrated into `ci.sh` itself
for unified `all` runs.

**Files**:

| File | Lines | Purpose |
|------|-------|---------|
| `ci/ci_mako_raft.sh` | 252 | Standalone Raft CI entry point |
| `ci/ci.sh` | 553 | Primary CI script (includes Raft tests at lines 265-331, 489-500) |

## 2. Script Structure

### 2.1 High-Level Architecture

```
ci_mako_raft.sh
├── Environment setup (set -e, MAKO_NO_GDB=1, colors)
├── check_for_hanging_processes()   — post-test process audit
├── cleanup_processes()             — pre-test process/port cleanup
├── Test functions:
│   ├── compile()
│   ├── run_simple_raft()
│   ├── run_1shard_replication_raft()
│   ├── run_2shard_replication_raft()
│   ├── run_1shard_replication_simple_raft()
│   └── run_2shard_replication_simple_raft()
├── cleanup()
└── Main case dispatch
```

### 2.2 Available Commands

```bash
./ci/ci_mako_raft.sh {command}
```

| Command | Function | Test Script Invoked |
|---------|----------|--------------------|
| `compile` | `compile()` | `make -j32` |
| `cleanup` | `cleanup()` | Process kill + temp file removal |
| `simpleRaft` | `run_simple_raft()` | `examples/mako-raft-tests/simpleRaft.sh` |
| `shard1ReplicationRaft` | `run_1shard_replication_raft()` | `examples/mako-raft-tests/test_1shard_replication_raft.sh` |
| `shard2ReplicationRaft` | `run_2shard_replication_raft()` | `examples/mako-raft-tests/test_2shard_replication_raft.sh` |
| `shard1ReplicationSimpleRaft` | `run_1shard_replication_simple_raft()` | `examples/mako-raft-tests/test_1shard_replication_simple_raft.sh` |
| `shard2ReplicationSimpleRaft` | `run_2shard_replication_simple_raft()` | `examples/mako-raft-tests/test_2shard_replication_simple_raft.sh` |
| `all` | All of the above in sequence | — |

### 2.3 The `all` Execution Order

```bash
compile
run_simple_raft                       # Basic Raft replication
run_1shard_replication_raft           # 1-shard TPC-C with Raft
run_2shard_replication_raft           # 2-shard TPC-C with Raft
run_1shard_replication_simple_raft    # 1-shard simple tx with Raft
run_2shard_replication_simple_raft    # 2-shard simple tx with Raft
```

## 3. Process Management

### 3.1 `cleanup_processes()` (lines 65-105)

Called before every test to ensure a clean slate.  Performs three phases:

**Phase 1: Process Termination** (lines 74-86)

Sends `SIGKILL` (-9) to all known test binaries and scripts:

```bash
# Binaries
pkill -9 -f "build/simpleRaft"
pkill -9 -f "build/simpleTransactionRepRaft"
pkill -9 -f "build/testPreferredReplicaStartup"
pkill -9 -f "build/testPreferredReplicaLogReplication"
pkill -9 -f "build/testNoOps"
pkill -9 -f "build/dbtest"

# Wrapper scripts
pkill -9 -f "test_1shard_replication_raft.sh"
pkill -9 -f "test_2shard_replication_raft.sh"
pkill -9 -f "test_1shard_replication_simple_raft.sh"
pkill -9 -f "test_2shard_replication_simple_raft.sh"
pkill -9 -f "bash/shard_raft.sh"
```

All `pkill` calls use `2>/dev/null || true` to suppress errors when no
matching process exists.

**Phase 2: Port Release Wait** (lines 90-98)

Polls for up to 10 seconds until common test ports are free:

```bash
for i in {1..10}; do
    if ! lsof -i :7001-8006 && ! lsof -i :31000-31100; then
        break
    fi
    sleep 1
done
```

**Phase 3: Log Archival** (lines 100-104)

Copies log files to `~/results/ci_raft_results_${RUN_NUM}_${RUN_INDEX}/`.

### 3.2 `check_for_hanging_processes()` (lines 25-59)

Called after every test to audit for leaked processes.  Waits 3 seconds
for natural exit, then counts processes matching `[d]btest|[s]impleRaft`.

If hanging processes exist:
1. Prints diagnostic (process list)
2. Sends `SIGKILL` to `build/dbtest` and `build/simpleRaft`
3. Returns 0 (pass) — hanging processes are not treated as test failures
   as long as throughput results were collected

### 3.3 Test Function Pattern

Every test function follows the same 5-step pattern:

```bash
run_X() {
    echo "========================================="    # 1. Header
    cleanup_processes                                   # 2. Pre-cleanup
    set +e                                              # 3. Run test (allow failure)
    bash ./examples/mako-raft-tests/X.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "X"                     # 4. Post-audit
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]  # 5. Combined result
}
```

## 4. Environment Variables

| Variable | Value | Purpose |
|----------|-------|---------|
| `MAKO_NO_GDB` | `1` | Disables GDB wrapping — GDB changes output format and breaks grep patterns used for result parsing |
| `RUN_NUM` | CI-provided | Run number for result directory naming |
| `RUN_INDEX` | CI-provided | Run index within the batch |

## 5. Comparison: `ci_mako_raft.sh` vs `ci.sh`

### 5.1 Structural Differences

| Aspect | `ci_mako_raft.sh` | `ci.sh` |
|--------|-------------------|---------|
| Lines | 252 | 553 |
| Raft-only | Yes | Includes Raft + Paxos + other tests |
| Color output | Yes (RED/GREEN/YELLOW/BLUE) | No |
| `BUILD_DIR` support | Hardcoded `build` | Configurable via env var |
| Memory limits | No | `run_with_memory_limit()` for heavy tests |
| `update_config.sh` | Not called | Called in `compile()` and `simplePaxos` |
| RocksDB cleanup | No | Cleans `/tmp/${USER}_mako_rocksdb_shard*` |
| SRPC unit tests | No | `run_srpc_unit_tests()` via ctest |
| Process kill approach | Direct `pkill -9` patterns | Filters out own PID/PPID tree |

### 5.2 Raft Tests in `ci.sh`

The Raft tests are also defined directly in `ci.sh` (lines 265-331)
with identical implementation.  This means Raft tests can be invoked
via either script:

```bash
# Equivalent invocations:
./ci/ci_mako_raft.sh shard1ReplicationRaft
./ci/ci.sh shard1ReplicationRaft
```

The `ci.sh all` command runs Raft tests after Paxos tests (lines 541-544):

```bash
# Raft replication tests
run_1shard_replication_raft
run_2shard_replication_raft
run_1shard_replication_simple_raft
run_2shard_replication_simple_raft
```

### 5.3 Process Cleanup Differences

`ci.sh` uses a more careful process kill that avoids killing itself:

```bash
for proc in simpleTransactionRep dbtest simplePaxos simpleTransaction; do
    pgrep -f "$proc" | while read pid; do
        if [ "$pid" != "$my_pid" ] && [ "$pid" != "$my_ppid" ]; then
            kill -9 "$pid"
        fi
    done
done
```

`ci_mako_raft.sh` uses simpler `pkill -9 -f` patterns which rely on
the fact that the script name (`ci_mako_raft.sh`) doesn't match the
binary patterns (`build/simpleRaft`, `build/dbtest`).

## 6. Shard Launch Scripts

### 6.1 `bash/shard_raft.sh` (Raft-Specific)

A Raft-dedicated shard launcher (39 lines) with hardcoded Raft config:

```bash
CMD="./build/dbtest \
  --num-threads $trd \
  --shard-index $shard \
  --shard-config $path/config/local-shards$nshard-warehouses$trd.yml \
  -F config/1leader_2followers/raft${trd}_shardidx${shard}.yml \
  -F config/occ_raft.yml \
  -P $cluster \
  --replication raft"
```

Key differences from `shard.sh`:
- Uses `raft${trd}_shardidx${shard}.yml` (port range 27xxx)
- Uses `occ_raft.yml` (mode `ab: raft`)
- Passes `--replication raft` explicitly
- No GDB support (no `util.sh` sourcing)

### 6.2 `bash/shard.sh` (Unified Launcher)

The unified shard launcher (62 lines) supports both Paxos and Raft
via a 7th argument:

```bash
replication_type=${7:-paxos}  # Default to paxos
```

Configuration selection:

```bash
if [ "$replication_type" == "raft" ]; then
    OCC_CONFIG="config/occ_raft.yml"
else
    OCC_CONFIG="config/occ_paxos.yml"
fi
CMD="$CMD -F config/1leader_2followers/paxos${trd}_shardidx${shard}.yml \
     -F $OCC_CONFIG --is-replicated --replication=$replication_type"
```

Note: The unified `shard.sh` still references `paxos${trd}_shardidx`
for the site config even in Raft mode (the port assignments work because
the Paxos config is used for site topology while `occ_raft.yml` sets
the protocol).  The dedicated `shard_raft.sh` correctly uses
`raft${trd}_shardidx` for the Raft-specific port range.

### 6.3 Arguments

Both scripts accept the same positional arguments:

| Position | Name | Example | Purpose |
|----------|------|---------|---------|
| `$1` | `nshard` | `2` | Number of shards in the cluster |
| `$2` | `shard` | `0` | This shard's index |
| `$3` | `trd` | `6` | Number of threads (also used for config lookup) |
| `$4` | `cluster` | `localhost` | Cluster process name |
| `$5` | `is_micro` | `0` | Enable micro benchmark flag |
| `$6` | `is_replicated` | `1` | Enable replication |
| `$7` | `replication_type` | `raft` | `shard.sh` only: paxos or raft |

## 7. Result Archival

Test results are archived to `~/results/ci_raft_results_${RUN_NUM}_${RUN_INDEX}/`
during `cleanup_processes()`.  This includes:

- `*.log` — general log files from the working directory
- `raft_*.log` — Raft-specific log files

The `cleanup()` command also removes performance output directories:

```bash
rm -rf ./out-perf.masstree/*
rm -rf ./src/mako/out-perf.masstree/*
```
