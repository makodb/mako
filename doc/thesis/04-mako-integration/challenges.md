# Integration Challenges and Bugs Fixed

## 1. Overview

Adding Raft as an alternative consensus protocol to a system designed around
Multi-Paxos required solving a series of integration challenges.  This
chapter documents the specific bugs encountered during the process, their
root causes, the fixes applied, and the lessons learned.  Each bug is
presented with exact source locations so that a reader can trace the fix in
the codebase.

## 2. Bug: Dispatcher Routing to Paxos (simpleRaft.cc)

### Symptom

The standalone `simpleRaft` example compiled and ran without errors, but
Raft RPCs (AppendEntries, Vote) were never sent.  Instead, the system
initialised Paxos workers and attempted Multi-Paxos consensus.

### Root Cause

The `DISPATCH_RAFT_OR_PAXOS` macro checks `janus::is_using_raft()` which
reads `g_replication_type`.  The default value is `ReplicationType::PAXOS`.
`simpleRaft.cc` was calling `setup()` without first calling
`set_replication_type(RAFT)`, so the dispatcher routed to
`paxos_impl::setup()`:

```cpp
// BEFORE fix (incorrect):
std::vector<string> ret = setup(argc, argv);  // → paxos_impl::setup()

// AFTER fix (correct):
janus::set_replication_type(janus::ReplicationType::RAFT);
std::vector<string> ret = setup(argc, argv);  // → raft_impl::setup()
```

### Fix

A single line was added before the `setup()` call in `simpleRaft.cc:92`:

```cpp
// examples/mako-raft-tests/simpleRaft.cc:88-94
// Set replication type to Raft BEFORE setup() so the dispatcher routes
// to raft_impl::setup() which disables Jetpack recovery (MAKO_DISABLE_JETPACK=1).
// Without this, the default is PAXOS and setup() goes through paxos_main_helper
// which does not disable Jetpack, causing RPC handler mismatches at runtime.
janus::set_replication_type(janus::ReplicationType::RAFT);

std::vector<string> ret = setup(18, argv_raft);
```

### Verification

After the fix, all 3 replicas received 303 callbacks (≥300 required):
`p1=303, p2=303, leader=303`.

### Lesson

**Dispatcher patterns require explicit type setting before the first
dispatch call.**  Global state defaults are not safe assumptions.

## 3. Bug: Auto-Detection Failure in `dbtest`

### Symptom

When running `dbtest` (Mako's main benchmark binary) with Raft config files
containing `ab: raft`, the system still used the Paxos code path.  The
`--replication raft` CLI flag worked, but the automatic config-based
detection did not.

### Root Cause

`dbtest` calls `init_env()` which calls `setup()`.  The `setup()` function
internally calls `Config::CreateConfig()` which parses the YAML and sets
`replica_proto_ = MODE_RAFT`.  However, the `DISPATCH_RAFT_OR_PAXOS` macro
in `setup()` itself checks `is_using_raft()` **before** `Config::CreateConfig()`
runs.  This creates a chicken-and-egg problem: the YAML is only parsed
inside `setup()`, but the dispatch decision happens at the `setup()` entry
point.

### Fix

Added `detect_replication_type_from_config()` in `mako.hh:779-816`.  This
function performs a lightweight text scan of the YAML files before `setup()`
is called, looking for `ab: raft`:

```cpp
// mako.hh:886-889 — called in init_env() before setup()
detect_replication_type_from_config(benchConfig.getPaxosConfigFile());

// ... then setup() correctly dispatches to raft_impl
std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
```

As a safety measure, `shard_raft.sh` also passes `--replication raft` on
the command line, providing a belt-and-suspenders approach.

### Priority Chain

| Detection Method | Priority | When Set |
|-----------------|----------|----------|
| `--replication raft` CLI flag | Highest | `dbtest.cc:main()` |
| `detect_replication_type_from_config()` | Medium | `init_env()` |
| Default (PAXOS) | Lowest | Static initialiser |

### Verification

After the fix, `dbtest` with Raft configs achieved 69,784.6 ops/sec with
`replay_batch: 796`.

### Lesson

**When a dispatcher pattern is used, auto-detection must run before the
first dispatch point**, not inside it.

## 4. Bug: Cross-Shard RPC Failures During Raft Elections (FAIL_NEW_VERSION)

### Symptom

In 2-shard mode with `FAIL_NEW_VERSION` enabled, the system would hang with
RPC timeout errors whenever a Raft leader election occurred.

### Root Cause

The `setup_leader_election_callbacks()` function in `mako.hh` registers a
callback that fires on every leadership change.  Under Paxos (fixed leader),
this callback is rarely invoked.  Under Raft, it fires on every election
and every leadership transfer.

The callback's `case 0` (leader lost) and `case 2` (became leader) handlers
called `sync_util::sync_logger::client_control()`, which issues cross-shard
RPCs to coordinate failure recovery.  These RPCs assume the target shards
have Paxos-compatible handlers.  When running under Raft, the targets don't
have these handlers, causing RPC timeouts.

```cpp
// mako.hh:648-660 — BEFORE fix
#if defined(FAIL_NEW_VERSION)
case 0: {
    // This was called on EVERY Raft election, not just datacenter failures:
    sync_util::sync_logger::client_control(0, benchConfig.getShardIndex());
    // ↑ Sends cross-shard RPC that times out under Raft
    break;
}
```

### Fix

Added `janus::is_using_raft()` guard at the top of every case handler:

```cpp
// mako.hh:648-660 — AFTER fix
#if defined(FAIL_NEW_VERSION) && !defined(MAKO_USE_RAFT)
case 0: {
    if (janus::is_using_raft()) {
        // Raft: Leader stepped down - no action needed
        break;
    }
    // Paxos-only recovery code...
    sync_util::sync_logger::client_control(0, ...);
    break;
}
case 2: {
    if (janus::is_using_raft()) {
        // Raft: Became leader - no action needed
        break;
    }
    // Paxos-only recovery code...
```

The guards are present in **both** `FAIL_NEW_VERSION` and
`!FAIL_NEW_VERSION` code paths (`mako.hh:650, 662, 700, 722`) — four
guards total.

### Verification

After the fix, 2-shard Raft tests pass without hangs.  Leadership changes
complete in ~40ms without triggering cross-shard RPCs.

### Lesson

**When adding a new protocol to a system with protocol-specific failure
recovery, every failure-recovery code path must be guarded.**  Raft's
frequent leadership changes expose paths that Paxos's fixed leader never
triggers.

## 5. Bug: GetOrCreateClient() Race Condition

### Symptom

Intermittent segmentation faults during shutdown or high-concurrency
scenarios, with stack traces pointing to the RRR transport backend's
`GetOrCreateClient()` method.

### Root Cause

Classic TOCTOU (Time-of-Check to Time-of-Use) race condition.  The code
found an entry in the `clients_` map, then released the mutex, then tried
to use the iterator:

```cpp
// rrr_rpc_backend.cc — BEFORE fix (buggy)
auto it = clients_.find(session_key);
if (it != clients_.end()) {
    clients_lock_.unlock();          // Release lock
    return it->second.clone();       // BUG: iterator may be invalid
}
```

**Race timeline**:
1. Thread A finds client → `it` points to valid entry
2. Thread A unlocks `clients_lock_`
3. Thread B (in `Stop()`) acquires lock and calls `clients_.clear()`
4. Thread A accesses `it->second` → **use-after-free**

### Fix

Clone the `Arc` before releasing the lock (`rrr_rpc_backend.cc:206-211`):

```cpp
// AFTER fix
auto it = clients_.find(session_key);
if (it != clients_.end()) {
    auto result = it->second.clone();  // Clone BEFORE unlock
    clients_lock_.unlock();
    return result;
}
```

**Commit**: `c84909cc` — "Fix race condition in GetOrCreateClient causing
intermittent segfault"

### Verification

- `shardNoReplication`: 5/5 passes
- `rrrTests`: 66/66 passes
- `shard2Replication`: passes consistently

### Lesson

**Always copy/clone data from shared structures before releasing the lock
that protects them.**  This applies universally, but is particularly
important in shutdown paths where data structures may be cleared.

## 6. Challenge: Process Cleanup and Port Release

### Problem

Raft test binaries (`simpleRaft`, `testPreferredReplicaStartup`,
`testPreferredReplicaLogReplication`, `testNoOps`) would sometimes hang
during shutdown, holding network ports open.  The next test would then fail
to bind to those ports.

### Root Cause

1. **Raft heartbeat threads**: Background OS threads (`StartLeadershipTransferMonitoring`,
   `HeartbeatLoop`) don't always exit cleanly if the election timer hasn't
   fired yet.
2. **RPC connections**: `rrr::Server::~Server()` enqueues cleanup commands
   to the poll thread, but if the poll thread has already exited, the
   commands are lost and connection reference counts never reach zero.
3. **TCP TIME_WAIT**: Even after process exit, TCP ports remain in
   TIME_WAIT state for 60 seconds.

### Fix

Multi-layered cleanup in `ci/ci_mako_raft.sh`:

```bash
# ci_mako_raft.sh:65-105
cleanup_processes() {
    # Layer 1: SIGKILL all known test binaries
    pkill -9 -f "build/simpleRaft" 2>/dev/null || true
    pkill -9 -f "build/testPreferredReplicaStartup" 2>/dev/null || true
    pkill -9 -f "build/testPreferredReplicaLogReplication" 2>/dev/null || true
    pkill -9 -f "build/testNoOps" 2>/dev/null || true
    pkill -9 -f "build/dbtest" 2>/dev/null || true

    # Layer 2: Kill wrapper scripts
    pkill -9 -f "test_1shard_replication_raft.sh" 2>/dev/null || true
    pkill -9 -f "test_2shard_replication_raft.sh" 2>/dev/null || true

    sleep 3  # Give OS time to release file descriptors

    # Layer 3: Poll for port release
    for i in {1..10}; do
        if ! lsof -i :7001-8006 >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done
}
```

Additionally, `check_for_hanging_processes()` runs after each test to detect
and kill zombie processes:

```bash
check_for_hanging_processes() {
    local hanging_count=$(ps aux | grep -E "[d]btest|[s]impleRaft" | wc -l)
    if [ "$hanging_count" -gt 0 ]; then
        pkill -9 -f "build/dbtest" 2>/dev/null || true
        pkill -9 -f "build/simpleRaft" 2>/dev/null || true
        sleep 2
    fi
}
```

### Lesson

**Distributed system tests require aggressive process cleanup.**  SIGTERM is
not sufficient; SIGKILL is needed.  Port release polling with backoff
prevents flaky tests from cascading failures.

## 7. Challenge: Port Conflicts Between Paxos and Raft

### Problem

When both Paxos and Raft tests run on the same machine (as in CI), they
can collide on TCP ports if they use the same ranges.

### Solution

Separate port ranges are assigned:

| Protocol | Base Port Range | Heartbeat Port Range | Config Files |
|----------|----------------|---------------------|-------------|
| Paxos | 17001-17999 | 27001-27999 | `paxos6_shardidx*.yml` |
| Raft | 27001-27999 | 37001-37999 | `raft6_shardidx*.yml` |
| Raft tests | 38100-38199 | 48100-48199 | `1c1s*r*p_cluster_test.yml` |

The heartbeat port is always `base_port + CtrlPortDelta` where
`CtrlPortDelta = 10000` (defined in `RaftWorker`).

**Config example** (`config/1c1s3r1p_cluster_test.yml`):
```yaml
site:
  server:
    - ["localhost:38100", "p1:38101", "p2:38102"]
```

### Lesson

**Assign non-overlapping port ranges per protocol from the start.**  Document
the allocation scheme so future developers know which ranges are reserved.

## 8. Challenge: Jetpack Recovery Incompatibility

### Problem

Mako's Jetpack optimisation (speculative execution with optimistic log
aggregation) conflicts with Raft's own recovery mechanisms.  Running both
simultaneously causes state inconsistencies.

### Solution

`raft_impl::setup()` forces `MAKO_DISABLE_JETPACK=1` before any worker
initialisation:

```cpp
// raft_main_helper.cc:243-248
if (std::getenv("MAKO_DISABLE_JETPACK") == nullptr) {
    setenv("MAKO_DISABLE_JETPACK", "1", 1);
}
```

At runtime, `JetpackRecoveryEnabled()` in `server.cc:280-310` checks this
environment variable and disables Jetpack's speculative recovery when the
flag is set.

### Design Choice

The flag is set as an environment variable (not a config parameter) because:
1. It must be set before `Config::CreateConfig()` runs
2. It can be overridden by operators who know what they're doing
3. It affects behaviour across multiple compilation units

### Lesson

**Protocol-specific optimisations must be explicitly disabled when switching
protocols.**  Using an environment variable provides the right balance of
safety (default off) and flexibility (overridable).

## 9. Additional Fixes: Transport Layer Shutdown Races

Beyond the bugs above, the RRR transport layer required five coordinated
fixes for clean shutdown:

| Fix | Description | Location |
|-----|-------------|----------|
| Atomic stop flag | `stop_` changed from `bool` to `std::atomic<bool>` | `rrr_rpc_backend.h` |
| Idempotent `Stop()` | Atomic compare-exchange prevents concurrent execution | `rrr_rpc_backend.cc` |
| Early stop checks | Check `stop_` at entry of all RPC send methods | `rrr_rpc_backend.cc` |
| Lock-protected check | Check `stop_` inside lock in `GetOrCreateClient()` | `rrr_rpc_backend.cc` |
| Post-wait check | Check `stop_` after RPC wait completes | `rrr_rpc_backend.cc` |

These work together as **defence-in-depth** against shutdown races that
manifest as segfaults or hangs when stopping Raft workers.

## 10. Summary

| Bug | Root Cause | Fix | Impact |
|-----|-----------|-----|--------|
| Missing `set_replication_type` | Dispatcher defaults to Paxos | Call before `setup()` | Paxos workers created instead of Raft |
| Auto-detection failure | YAML parsed inside dispatch target | Scan YAML before `setup()` | `dbtest` ignores `ab: raft` config |
| FAIL_NEW_VERSION RPCs | Leader election triggers Paxos recovery | `is_using_raft()` guard | System hangs in 2-shard mode |
| GetOrCreateClient race | Iterator used after unlock | Clone before unlock | Intermittent segfaults |
| Process cleanup | Raft threads don't exit cleanly | SIGKILL + port polling | Tests fail to bind ports |
| Port conflicts | Same ranges for Paxos and Raft | Separate port ranges | Paxos and Raft tests collide |
| Jetpack incompatibility | Different recovery models | Force disable for Raft | State inconsistencies |
| Transport shutdown | Non-atomic stop flag | Atomic + defence-in-depth | Segfaults during shutdown |

### Key Architectural Lesson

The dispatcher pattern (`DISPATCH_RAFT_OR_PAXOS`) is a clean abstraction,
but it creates a **single critical point**: the global `ReplicationType`
must be correctly set before any dispatch call.  Three separate bugs
(#2, #3, and this dispatcher issue) were all caused by the same
fundamental problem: the replication type was not set early enough.
The solution—belt-and-suspenders with CLI flag, auto-detection, and
explicit calls—ensures correctness regardless of how the binary is launched.
