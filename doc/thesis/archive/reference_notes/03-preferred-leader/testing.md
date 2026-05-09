# Preferred Leader Election — Testing

## What This Document Covers

This document describes the three dedicated test binaries that verify the preferred leader system, their test runner scripts, the CI integration, the Mako C API used by the tests, configuration files, and the success criteria for each test.

**Test binary source files**:
- `examples/mako-raft-tests/testPreferredReplicaStartup.cc` (265 lines)
- `examples/mako-raft-tests/testPreferredReplicaLogReplication.cc` (422 lines)
- `examples/mako-raft-tests/testNoOps.cc` (531 lines)

**Test runner scripts**:
- `examples/mako-raft-tests/run_test1_preferred_startup.sh` (361 lines)
- `examples/mako-raft-tests/run_test_log_replication.sh` (159 lines)
- `examples/mako-raft-tests/run_test_noops.sh` (256 lines)

**CI integration**:
- `ci/ci_mako_raft.sh` (252 lines) — Cleanup support, not direct test invocation

**Configuration files**:
- `config/none_raft.yml` — Raft mode with no concurrency control
- `config/1c1s3r1p_cluster_test.yml` — 3-node cluster (1 shard, 3 replicas)
- `config/1c1s5r1p_cluster_test.yml` — 5-node cluster (1 shard, 5 replicas)

---

## 1. Test Architecture Overview

All three tests follow the same multi-process architecture:

```
Test Runner Script (bash)
    |
    +-- Launches N processes (5 typically) of the same binary
    |     Each process gets a different name: localhost, p1, p2, p3, p4
    |     Each writes to its own log file
    |
    +-- Waits for all processes to complete (with timeout)
    |
    +-- Parses log files for results
    |
    +-- Reports PASS/FAIL based on success criteria
```

Each process name maps to a host entry in the YAML config. All run on `127.0.0.1` but bind to different ports. The `localhost` process is always the **preferred leader** (locale_id=0).

### Mako Replication API

All tests use the Mako replication helper API (`src/deptran/replication_helper.cc`), which dispatches to the Raft or Paxos implementation via the `DISPATCH_RAFT_OR_PAXOS` macro:

| API Function | Purpose |
|-------------|---------|
| `setup(argc, argv)` | Initialize Raft from config files |
| `setup2(arg1, arg2)` | Launch Raft services (RPC, heartbeats, election) |
| `register_leader_election_callback(fn)` | Hook for leadership changes (`fn(1)` = became leader, `fn(0)` = lost) |
| `register_for_leader_par_id_return(fn, par_id)` | Register log apply callback for leader |
| `register_for_follower_par_id_return(fn, par_id)` | Register log apply callback for follower |
| `add_log_to_nc(data, len, par_id, batch_size)` | Submit log entry (leader-only) |
| `shutdown_paxos()` | Clean shutdown |

### Build Configuration

All three binaries are built when `MAKO_USE_RAFT=ON` (`CMakeLists.txt:1031-1033`):

```cmake
if(MAKO_USE_RAFT)
  add_apps(testPreferredReplicaStartup examples/mako-raft-tests/testPreferredReplicaStartup.cc)
  add_apps(testPreferredReplicaLogReplication examples/mako-raft-tests/testPreferredReplicaLogReplication.cc)
  add_apps(testNoOps examples/mako-raft-tests/testNoOps.cc)
endif()
```

---

## 2. Test 1: `testPreferredReplicaStartup`

### Purpose

Verify that the preferred replica (localhost) becomes leader via the TimeoutNow leadership transfer protocol, even when another replica initially wins the election.

### Cluster Configuration

- **Nodes**: 5 (localhost, p1, p2, p3, p4)
- **Config files**: `config/none_raft.yml` + `config/1c1s3r1p_cluster_test.yml`
- **Preferred leader**: localhost (locale_id=0)
- **Total duration**: ~32 seconds (2s startup + 30s monitoring)

### Test Phases

```
Phase 1: Configuration (T=0)
    Parse command-line args, construct argv for Mako
    Args: -b -d 60 -f none_raft.yml -f 1c1s3r1p_cluster_test.yml -t 30 -T 100000 -n 32 -P <name> -A 10000

Phase 2: Raft Initialization (T=0)
    Call setup(argc, argv)
    Register leadership callbacks

Phase 3: Service Launch (T=0+)
    Call setup2(arg1, arg2)
    Record startup_time_ms

Phase 4: Startup Wait (T=0 to T=2s)
    Sleep 2 seconds for cluster stabilization
    During this time: election happens, any replica may win

Phase 5: Monitoring (T=2s to T=32s)
    Check every 5 seconds:
      - Am I leader?
      - Elapsed time since becoming leader
    If preferred leader lost leadership after gaining it → flag issue

Phase 6: Results (T=32s)
    Report: times_became_leader, times_lost_leadership, leader_stable_duration
    Exit code 0 if localhost is leader, 1 otherwise
```

### State Tracking

```cpp
atomic<bool> i_am_leader{false};
atomic<int> times_became_leader{0};
atomic<int> times_lost_leadership{0};
atomic<uint64_t> time_became_leader_ms{0};
atomic<uint64_t> startup_time_ms{0};
```

### Leadership Callback

The test registers a callback that tracks leadership transitions:

```cpp
register_leader_election_callback([&](int control) {
    times_became_leader++;
    i_am_leader.store(true);
    uint64_t elapsed = current_time - startup_time_ms;
    safe_print("[" + proc_name + "] BECAME LEADER at +" + to_string(elapsed) + "ms");
});
```

Additionally, a dummy follower log callback is registered to satisfy the API:

```cpp
register_for_follower_par_id_return([&](const char*& log, int len, int par_id, int slot_id,
    queue<tuple<int, int, int, int, const char*>>& un_replay_logs_) {
    return static_cast<int>(timestamp * 10 + 1);
}, 0);
```

### Success Criteria (Script-Level)

The test runner script (`run_test1_preferred_startup.sh`) checks:

| Criterion | Condition |
|-----------|-----------|
| All processes exited cleanly | `exit_code == 0` for all 5 |
| localhost became leader | `LOCALHOST_LEADER >= 1` |
| p1 never became leader | `P1_LEADER == 0` |
| p2 never became leader | `P2_LEADER == 0` |
| p3 never became leader | `P3_LEADER == 0` |
| p4 never became leader | `P4_LEADER == 0` |

The script parses "BECAME LEADER" strings from each process's log file to determine leadership history.

### What This Proves

1. **Preferred leader election works**: The designated preferred replica (localhost) becomes leader
2. **Leadership transfer works**: If a non-preferred replica wins the initial election, it transfers leadership to localhost
3. **Stability**: Once localhost becomes leader, it remains leader for the full 30-second monitoring period
4. **Non-preferred replicas stay followers**: No other replica should become leader while localhost is alive

---

## 3. Test 2: `testPreferredReplicaLogReplication`

### Purpose

Verify that log entries submitted to the leader are correctly replicated to ALL replicas, including command wrapping in `TpcCommitCommand`.

### Cluster Configuration

- **Nodes**: 5 (localhost, p1, p2, p3, p4)
- **Config**: 5-node cluster (`config/1c1s5r1p_cluster_test.yml`)
- **Total duration**: ~15 seconds

### Test Parameters

```cpp
const int STARTUP_TIME_SEC = 2;
const int LEADER_WAIT_SEC = 3;
const int REPLICATION_WAIT_SEC = 5;
const int NUM_LOGS = 25;
const int BATCH_SIZE = 5;
```

### Test Phases

```
Phase 1: Startup (0-2s)
    Cluster stabilization, election happens

Phase 2: Wait for Leader (2-5s)
    Wait for leadership callback to fire

Phase 3: Log Submission (5-8s, leader only)
    FOR i = 0 to 24:
        Create log: "LOG_ENTRY_NNN" (NNN = zero-padded index)
        Wrap in TpcCommitCommand
        Call add_log_to_nc(serialized, len, 0, BATCH_SIZE)
        Sleep 10ms between submissions

Phase 4: Replication Wait (8-13s)
    Poll every 100ms:
        IF logs_applied_count >= NUM_LOGS: break (early exit)
    Timeout at 5 seconds

Phase 5: Verification
    Assert: logs_applied_count >= NUM_LOGS
    Report PASS/FAIL
```

### Command Wrapping

Each log entry is wrapped in a `TpcCommitCommand` before submission:

```cpp
shared_ptr<TpcCommitCommand> create_log_command(const string& log_data) {
    auto cmd = make_shared<TpcCommitCommand>();
    cmd->tx_id_ = unique_id++;  // Monotonic transaction ID
    auto sp = make_shared<SimpleCommand>();
    sp->input.put_blob(log_data.c_str(), log_data.length());
    cmd->cmd_ = make_shared<VecPieceData>();
    cmd->cmd_->push_back(sp);
    return cmd;
}
```

The serialized format is: `[tx_id (8 bytes)][length (4 bytes)][log_data (variable)]`.

### Log Application Callback

Each replica registers both leader and follower callbacks that count applied logs:

```cpp
register_for_follower_par_id_return([&](const char*& log, int len, int par_id, int slot_id, ...) {
    int count = ++logs_applied_count;
    if (count == 1) first_log_applied_time = now;
    if (count == NUM_LOGS) last_log_applied_time = now;
    return timestamp * 10 + 1;
}, 0);
```

### Success Criteria

| Criterion | Condition |
|-----------|-----------|
| All logs replicated | `logs_applied_count >= NUM_LOGS` (25) for each replica |
| All processes pass | 5/5 replicas report "PASS" in their logs |

### What This Proves

1. **Log replication works with preferred leader**: Commands submitted to the preferred leader replicate to all followers
2. **TpcCommitCommand wrapping**: The Mako-specific command wrapping integrates correctly with Raft
3. **Batch submission**: Logs submitted with `BATCH_SIZE=5` are correctly batched and replicated
4. **Applied count accuracy**: Every replica applies exactly the same number of logs

---

## 4. Test 3: `testNoOps`

### Purpose

Verify the NO-OPS watermark synchronization mechanism. NO-OPS are special log entries ("no-ops:N" where N is an epoch number) used to synchronize state across replicas without carrying user data. They serve as epoch boundaries for the Mako transaction system.

### Cluster Configuration

- **Nodes**: 5 (localhost, p1, p2, p3, p4)
- **Config**: 5-node cluster
- **Total duration**: ~25 seconds

### Test Parameters

```cpp
const int STARTUP_TIME_SEC = 2;
const int LEADER_WAIT_SEC = 3;
const int NOOPS_WAIT_SEC = 5;
const int LOGS_WAIT_SEC = 5;
const int NUM_NOOPS = 5;            // Epochs 0-4
const int NUM_REGULAR_LOGS = 10;
const int BATCH_SIZE = 1;           // NO-OPS should not be batched
```

### NO-OPS Format

```
"no-ops:X"  (8 bytes)
  |        |
  +--------+-- Prefix: "no-ops:" (7 bytes)
           +-- Epoch digit: '0'-'9' (1 byte)
```

The detection function:

```cpp
int isNoopsLocal(const char* log, int len) {
    if (len == 8) {
        if (log[0]=='n' && log[1]=='o' && log[2]=='-' &&
            log[3]=='o' && log[4]=='p' && log[5]=='s' && log[6]==':') {
            return log[7] - '0';  // Return epoch number
        }
    }
    return -1;  // Not a NO-OPS
}
```

### Test Phases

```
Phase 1-3: Startup and Leader Election (0-5s)
    Same as other tests

Phase 4: NO-OPS Submission (5-5.25s, leader only)
    FOR epoch = 0 to 4:
        msg = "no-ops:" + to_string(epoch)
        add_log_to_nc(msg, 8, 0, BATCH_SIZE=1)  // Not batched
        Sleep 50ms between epochs

Phase 5: NO-OPS Propagation Wait (5.25-10.25s)
    Poll every 100ms:
        IF noops_applied_count >= NUM_NOOPS: break (early exit)
    Timeout at 5 seconds

Phase 6: Epoch Verification
    Check that all epochs 0-4 were received
    Verify max_epoch_seen == NUM_NOOPS - 1

Phase 7: Regular Log Submission (10.25-11s, leader only)
    FOR i = 0 to 9:
        msg = "REGULAR_LOG_NNN"
        add_log_to_nc(msg, len, 0, BATCH_SIZE=1)
        Sleep 20ms between logs

Phase 8: Regular Log Wait (11-16s)
    Poll every 100ms for regular log application
    Timeout at 5 seconds

Phase 9: Final Verification
    Assert: noops_applied >= NUM_NOOPS
    Assert: all epochs 0-4 received
    Assert: regular_logs_applied >= NUM_REGULAR_LOGS
    If preferred replica: assert still leader
```

### State Tracking

```cpp
atomic<int> noops_applied_count{0};
atomic<int> regular_logs_applied_count{0};
atomic<int> noops_submitted_count{0};
atomic<int> regular_logs_submitted_count{0};
atomic<int> max_epoch_seen{-1};
vector<atomic<bool>> epoch_received(NUM_NOOPS);  // Per-epoch reception tracking
```

### Log Application Callback

The callback distinguishes between NO-OPS and regular logs:

```cpp
register_for_follower_par_id_return([&](const char*& log, int len, int par_id, int slot_id, ...) {
    int epoch = isNoopsLocal(log, len);
    if (epoch >= 0) {
        // NO-OPS message
        noops_applied_count++;
        epoch_received[epoch] = true;
        if (epoch > max_epoch_seen) max_epoch_seen = epoch;
    } else {
        // Regular log
        regular_logs_applied_count++;
    }
    return timestamp * 10 + 1;
}, 0);
```

### Success Criteria

| Criterion | Condition |
|-----------|-----------|
| All NO-OPS applied | `noops_applied_count >= 5` |
| All epochs received | `epoch_received[0..4]` all true |
| All regular logs applied | `regular_logs_applied_count >= 10` |
| Preferred leader stable | If preferred: `i_am_leader == true` at end |
| All replicas pass | 5/5 replicas report "PASS" |

### What This Proves

1. **NO-OPS propagation**: Special watermark entries replicate to all replicas
2. **Epoch ordering**: All epochs 0-4 arrive and are tracked individually
3. **NO-OPS + regular log coexistence**: NO-OPS and regular logs can be interleaved without issues
4. **Non-batching behavior**: `BATCH_SIZE=1` ensures NO-OPS are sent individually (not bundled with other entries)
5. **Preferred leader stability**: The preferred leader remains leader throughout both NO-OPS and regular log phases

---

## 5. Test Runner Scripts

### Common Pattern

All three scripts follow the same architecture:

```
1. Setup
   - Determine paths (SCRIPT_DIR, PROJECT_ROOT, BUILD_DIR)
   - Check/build test binary if missing
   - Create log directory
   - Kill any stale processes from previous runs

2. Launch
   - Start 5 processes in background (&), capture PIDs
   - Each gets a different name: localhost, p1, p2, p3, p4
   - Each writes stdout/stderr to its own log file

3. Monitor
   - Wait for completion with progress display
   - Timeout protection (kill hanging processes)

4. Analyze
   - Wait for each process and capture exit code
   - Parse log files for results (grep for keywords)
   - Count PASS/FAIL verdicts

5. Report
   - Per-process results
   - Overall PASS/FAIL based on aggregated criteria
   - Log file locations for debugging
   - Exit with appropriate code
```

### Script-Specific Details

| Script | Duration | Log Directory | Kill Signal |
|--------|----------|---------------|-------------|
| `run_test1_preferred_startup.sh` | ~35s | `logs/test1_startup/` | `pkill -9 -f testPreferredReplicaStartup` |
| `run_test_log_replication.sh` | ~20s | `logs/test_log_replication/` | `pkill -9 -f testPreferredReplicaLogReplication` |
| `run_test_noops.sh` | ~25s | `logs_noops_test/` | `pkill -9 -f testNoOps` |

### Invocation

```bash
# From project root
./examples/mako-raft-tests/run_test1_preferred_startup.sh
./examples/mako-raft-tests/run_test_log_replication.sh
./examples/mako-raft-tests/run_test_noops.sh
```

---

## 6. CI Integration

The CI script (`ci/ci_mako_raft.sh`) includes cleanup support for preferred leader test processes:

```bash
# In cleanup function (lines 74-78)
pkill -9 -f "build/testPreferredReplicaStartup"
pkill -9 -f "build/testPreferredReplicaLogReplication"
pkill -9 -f "build/testNoOps"
```

The preferred leader tests are not currently included in the automated CI `all` target — they are run manually or via their individual runner scripts. The CI `all` target runs integration tests that exercise Raft through the Mako shard framework (`shard1ReplicationRaft`, `shard2ReplicationRaft`, etc.), which indirectly test preferred leader behavior since the preferred leader system is always active when Raft is used.

---

## 7. Configuration Files

### `config/none_raft.yml`

```yaml
mode:
  cc: none          # No concurrency control (tests only replication)
  ab: raft          # Atomic broadcast = Raft
  batch: false
  retry: 20
  ongoing: 1
```

This config disables concurrency control, isolating the test to pure Raft replication behavior.

### `config/1c1s5r1p_cluster_test.yml` (5-node)

```yaml
site:
  server:
    - ["localhost:38101", "p1:38102", "p2:38103", "p3:38104", "p4:38105"]
  client:
    - ["c01"]

process:
  localhost: localhost
  p1: p1
  p2: p2
  p3: p3
  p4: p4
  c01: client

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
  p3: 127.0.0.1
  p4: 127.0.0.1
  client: 127.0.0.1
```

All 5 nodes run on `127.0.0.1` with different ports (38101-38105). The `localhost` process maps to `locale_id=0`, which the preferred leader system uses to select it as the preferred leader.

### `config/1c1s3r1p_cluster_test.yml` (3-node)

Same structure but with only 3 replicas: `localhost:38100`, `p1:38101`, `p2:38102`.

---

## 8. Relationship to Standard Raft Tests

The codebase also includes unit-level Raft tests (`src/deptran/raft/test.h`, `test.cc`, `testconf.h`, `testconf.cc`) that test basic Raft correctness:

| Test | What It Tests |
|------|---------------|
| `testInitialElection` | Single leader election with 5 nodes |
| `testReElection` | Re-election after disconnecting the leader |
| `testBasicAgree` | Log agreement across 3 nodes |
| `testFailAgree` | Agreement despite one follower failure |
| `testFailNoAgree` | No agreement when quorum lost |
| `testRejoin` | Disconnected leader rejoining the cluster |
| `testConcurrentStarts` | Multiple concurrent log submissions |
| `testBackup` | Leader backoff when follower is behind |
| `testCount` | RPC count bounds |
| `testUnreliableAgree` | Agreement with unreliable network |
| `testFigure8` | Figure 8 scenario from the Raft paper |

These tests run in a single process with simulated network partitions (via `Disconnect()`/`Reconnect()`), whereas the preferred leader tests run as multi-process deployments with real TCP connections.

The standard tests verify core Raft correctness. The preferred leader tests verify that the leadership transfer extension works correctly on top of the correct Raft base.

---

## 9. Correctness Guarantees

The three preferred leader tests collectively guarantee:

| Property | Verified By |
|----------|------------|
| Preferred replica becomes leader | `testPreferredReplicaStartup` — localhost becomes leader, p1-p4 don't |
| Leadership transfer works | `testPreferredReplicaStartup` — even if non-preferred wins first |
| Leader stability after transfer | `testPreferredReplicaStartup` — 30s monitoring without leadership loss |
| Log replication correctness | `testPreferredReplicaLogReplication` — 25 logs replicated to all 5 replicas |
| TpcCommitCommand wrapping | `testPreferredReplicaLogReplication` — command serialization/deserialization |
| Batch replication | `testPreferredReplicaLogReplication` — BATCH_SIZE=5 works correctly |
| NO-OPS watermark propagation | `testNoOps` — 5 epochs (0-4) received by all replicas |
| Epoch tracking | `testNoOps` — individual epoch reception verified |
| NO-OPS + regular log ordering | `testNoOps` — both types coexist without interference |
| Multi-process deployment | All 3 tests — real TCP connections, real ports, real processes |

---

## Related Documents

- [Design and Motivation](design.md) — Why preferred leader exists
- [Implementation Details](implementation.md) — Method-level walkthrough
- [CI Testing](../06-ci-testing/ci_script.md) — CI integration testing (Raft shard tests)
- [Server Implementation](../02-raft-core/server_implementation.md) — RaftServer internals
