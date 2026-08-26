# Mako-Side Integration Points

## 1. Overview

While the `replication_helper` dispatcher and `raft_main_helper` glue layer
live in the `src/deptran/` tree, several critical integration points exist on
the **Mako side** (`src/mako/`).  These are the places where Mako's
transaction engine calls into the unified replication API, registers
callbacks, and handles leadership changes.

The key principle is that Mako remains protocol-agnostic: all Raft-specific
logic is behind the dispatcher.  The Mako-side changes are limited to:

1. **Auto-detection**: Scanning YAML configs for `ab: raft`
2. **Guard clauses**: `is_using_raft()` checks in failure-handling code paths
3. **Launch scripts**: Separate Raft-specific scripts with correct config files
4. **Config files**: `occ_raft.yml` alongside `occ_paxos.yml`

**Source**: `src/mako/mako.hh`, `bash/shard.sh`, `bash/shard_raft.sh`,
`config/occ_raft.yml`

## 2. `init_env()` — The Main Initialisation Sequence

All replication setup happens inside `init_env()` (`mako.hh:858`).  This
function is called from `dbtest.cc::main()` after command-line parsing.

```
init_env()                                        [mako.hh:858]
  +-- init_config_node() / fetch_config_from_cnode()
  +-- setup_sync_util_callbacks()                  // epoch queries
  +-- if (isReplicated):
  |   +-- Create replicated_db (TSharedThreadPoolMbta)
  |   +-- setup_transport_callbacks()              // failure recovery
  |   +-- setup_leader_election_callbacks()         // leadership changes
  |   +-- detect_replication_type_from_config()     // ← Raft auto-detection
  |   +-- prepare_paxos_args() → argc/argv
  |   +-- setup(argc, argv)                         // → raft_impl::setup()
  |   +-- setup_paxos_leader_callbacks(tracker)    // watermark callbacks
  |   +-- setup_paxos_follower_callbacks(replicated_db)
  |   +-- setup2(0, shardIndex)                    // → raft_impl::setup2()
  |   +-- sleep(3)                                  // wait for all nodes
  +-- return db
```

**Critical ordering**: `detect_replication_type_from_config()` must run
**before** `setup()` because `setup()` uses the `DISPATCH_RAFT_OR_PAXOS`
macro, and the global `ReplicationType` must be set correctly before the
first dispatch call.

The callback registrations (`setup_paxos_leader_callbacks` and
`setup_paxos_follower_callbacks`) run **after** `setup()` but **before**
`setup2()`.  This is because `setup()` creates the `RaftWorker` instances
that the callbacks will be registered on, and `setup2()` starts the election
timer which may immediately trigger leadership changes.

## 3. `detect_replication_type_from_config()`

### 3.1 Implementation

```cpp
// mako.hh:779-816
static void detect_replication_type_from_config(
    const vector<string>& config_files) {
    // Don't override explicit CLI setting
    if (janus::is_using_raft()) return;

    for (const auto& file_path : config_files) {
        std::ifstream ifs(file_path);
        std::string line;
        while (std::getline(ifs, line)) {
            auto pos = line.find("ab:");
            if (pos != std::string::npos) {
                // trim and extract value
                if (value == "raft") {
                    janus::set_replication_type(
                        janus::ReplicationType::RAFT);
                    return;
                }
            }
        }
    }
}
```

### 3.2 Design Rationale

This function exists because of a real integration bug: `dbtest` used the
Paxos code path even when the YAML config had `ab: raft`, because
`Config::CreateConfig()` (which parses the YAML and sets `replica_proto_`)
only runs inside `setup()`, but the dispatcher needs to know the protocol
**before** `setup()` is called.

The fix is a lightweight YAML scanner that:
1. Opens each config file as plain text
2. Searches for lines containing `ab:`
3. Extracts the value after the colon
4. If the value is `"raft"`, sets the global type

### 3.3 Priority Rules

| Detection Method | Priority | When Set |
|-----------------|----------|----------|
| `--replication raft` CLI flag | Highest | `dbtest.cc:main()`, before `init_env()` |
| `detect_replication_type_from_config()` | Medium | `init_env()`, before `setup()` |
| Default (PAXOS) | Lowest | Static initialiser of `g_replication_type` |

The function checks `is_using_raft()` first and returns immediately if the
CLI flag already set the type.  This ensures CLI always wins.

## 4. `setup_leader_election_callbacks()`

### 4.1 Purpose

This function registers a single callback that fires on every Raft (or
Paxos) leadership change.  It handles the cross-shard coordination needed
when a datacenter fails and a new leader must be elected.

### 4.2 Implementation Structure

```cpp
// mako.hh:641-761
static void setup_leader_election_callbacks() {
    register_leader_election_callback([&](int control) {
        switch (control) {
#if defined(FAIL_NEW_VERSION) && !defined(MAKO_USE_RAFT)
            case 0: { /* leader stepped down */ }
            case 2: { /* became leader */ }
#endif
#if !defined(FAIL_NEW_VERSION)
            case 0: { /* leader stepped down (partial failure) */ }
            case 2: { /* became leader (PREPARE phase) */ }
            case 3: { /* COMMIT phase */ }
            case 4: { /* datacenter failure */ }
#endif
        }
    });
}
```

The `control` parameter values:

| Value | Meaning | Paxos Action | Raft Action |
|-------|---------|-------------|-------------|
| 0 | Lost leadership | Stop exchange, issue `client_control(0)` | No-op (Raft handles internally) |
| 1 | (reserved) | — | — |
| 2 | Gained leadership | Collect FVW, `client_control(1)`, notify workers | No-op (Raft handles internally) |
| 3 | Commit new leader | Notify workers, `client_control(2)` | No-op |
| 4 | Datacenter failure | Forward to all followers | No-op |

### 4.3 The `is_using_raft()` Guard

Every `case` that performs cross-shard RPC or coordination checks
`janus::is_using_raft()` and breaks immediately if true:

```cpp
case 0: {
    if (janus::is_using_raft()) {
        // Raft: Leader stepped down - no action needed
        break;
    }
    // Paxos: complex recovery...
    sync_util::sync_logger::client_control(0, ...);
    break;
}
```

This guard exists because of a real bug: without it, Raft leadership
changes (which are frequent — every election, every transfer) would trigger
the Paxos failure-recovery code path, which calls `client_control()` to
issue cross-shard RPCs.  In a 2-shard system, these RPCs would fail because
the target shards are running Raft (not Paxos) and don't have the expected
RPC handlers.

**The bug manifested as**: cross-shard RPC timeouts during Raft leader
elections in 2-shard mode, causing the entire system to hang.

### 4.4 Compile-Time vs Runtime Guards

The code uses **both** compile-time (`#if defined(FAIL_NEW_VERSION)`) and
runtime (`janus::is_using_raft()`) guards:

- `FAIL_NEW_VERSION` is a compile-time flag that enables the new
  failure-recovery code path (datacenter failover).  It selects which set
  of `case` handlers are compiled.
- `is_using_raft()` is a runtime check within each handler.  It is needed
  because the same binary can run with either Paxos or Raft, selected by
  config or CLI flag.

Both code paths (`FAIL_NEW_VERSION` and `!FAIL_NEW_VERSION`) contain the
same `is_using_raft()` guards for cases 0 and 2.

## 5. `setup_paxos_leader_callbacks()` / `setup_paxos_follower_callbacks()`

### 5.1 Leader Callbacks

```cpp
// mako.hh:488-494
static void setup_paxos_leader_callbacks(
    vector<pair<uint32_t, uint32_t>>& advanceWatermarkTracker) {
    if (!isReplicated) return;
    for (int i = 0; i < nthreads; i++) {
        register_paxos_leader_callback(advanceWatermarkTracker, i);
    }
}
```

This iterates over all partition threads and registers a leader watermark
callback via `register_for_leader_par_id_return()`.  The callback
(`register_paxos_leader_callback`, `mako.hh:400-486`) handles:

1. **Advancer marker** (`len == ADVANCER_MARKER_NUM`): Returns
   `STATUS_REPLAY_DONE` and starts the advancer on partition 0.
2. **End signal** (`len == 0`): Sets `local_timestamp_` to max, increments
   end-received counter.
3. **NO-OP entries** (`isNoops(log, len) != -1`): Increments `noops_cnt`,
   on partition 0 computes local watermark, publishes via NFSSync, updates
   stable timestamp.
4. **Regular logs** (`len > 0` and not NO-OP): Extracts `CommitInfo`,
   stores `timestamp` in `local_timestamp_[par_id]`, updates latency
   tracking.

Returns encoded value: `timestamp * 10 + status`.

### 5.2 Follower Callbacks

```cpp
// mako.hh:496-502
static void setup_paxos_follower_callbacks(
    TSharedThreadPoolMbta& replicated_db) {
    if (!isReplicated) return;
    for (int i = 0; i < nthreads; i++) {
        register_paxos_follower_callback(replicated_db, i);
    }
}
```

The follower callback (`register_paxos_follower_callback`, `mako.hh:240`)
replays committed logs into the `replicated_db` instance, which is a
thread-pool-backed Masstree database.

### 5.3 Protocol Agnosticism

Despite the "paxos" naming, these callbacks work identically with Raft.
The dispatch layer routes `register_for_leader_par_id_return()` to either
`paxos_impl` or `raft_impl`, and both implementations cache and apply the
callback to their respective worker instances.

The `thread_id` parameter maps to `par_id` (partition ID), matching the
convention where each Mako thread owns one partition.

## 6. Other Callback Setup Functions

### 6.1 `setup_sync_util_callbacks()`

```cpp
// mako.hh:585-601
static void setup_sync_util_callbacks() {
    register_sync_util([&]() {
        return isReplicated ? get_epoch() : 0;
    });
    register_sync_util_sc([&]() { /* same */ });
    register_sync_util_ss([&]() { /* same */ });
}
```

Registers three epoch-query callbacks used by the sync utility system
(watermark exchange between shards).  These are protocol-agnostic because
`get_epoch()` dispatches through the replication helper.

### 6.2 `setup_transport_callbacks()`

```cpp
// mako.hh:604-639
static void setup_transport_callbacks() {
    register_fasttransport_for_dbtest([&](int control, int value) {
        switch (control) {
            case 4: {
                upgrade_p1_to_leader();
                // send NO-OPs, notify workers
            }
        }
        return 0;
    });
}
```

Handles the transport-level leadership transfer for datacenter failure
scenarios.  `upgrade_p1_to_leader()` dispatches to the replication helper
and is a no-op for Raft (Raft handles leadership transfer internally via
the preferred leader mechanism).

### 6.3 `cleanup_and_shutdown()`

```cpp
// mako.hh:763-773
static void cleanup_and_shutdown() {
    if (isReplicated) {
        sleep_for(2s);
        pre_shutdown_step();     // → raft_impl::pre_shutdown_step()
        shutdown_paxos();        // → raft_impl::shutdown_paxos()
    }
    sync_util::sync_logger::shutdown();
}
```

The 2-second sleep allows in-flight RPCs to complete before initiating the
teardown sequence.  Both `pre_shutdown_step()` and `shutdown_paxos()`
dispatch to the correct implementation.

## 7. YAML Config Files

### 7.1 `config/occ_raft.yml`

```yaml
mode:
  cc: occ         # concurrency control: optimistic
  ab: raft        # atomic broadcast: Raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1      # per client
```

This is the Raft-mode equivalent of `config/occ_paxos.yml`.  The critical
difference is `ab: raft` which:
1. Is detected by `detect_replication_type_from_config()` for auto-detection
2. Is parsed by `Config::InitMode()` which calls `Frame::Name2Mode("raft")`
   to set `replica_proto_ = MODE_RAFT`

### 7.2 Raft-Specific Cluster Configs

For each thread count and shard index, Raft uses its own cluster topology
files in `config/1leader_2followers/`:

| Raft Config | Paxos Equivalent |
|------------|------------------|
| `raft3_shardidx0.yml` | `paxos3_shardidx0.yml` |
| `raft3_shardidx1.yml` | `paxos3_shardidx1.yml` |

The Raft configs use port ranges 27xxx to avoid collisions with Paxos on
17xxx.  The topology (1 leader + 2 followers per partition) is identical.

## 8. Launch Scripts: `shard.sh` vs `shard_raft.sh`

### 8.1 `bash/shard.sh` — Unified Launcher

The main launch script (`bash/shard.sh`, 62 lines) supports both protocols
via a 7th argument:

```bash
replication_type=${7:-paxos}  # Default to paxos if not specified

if [ "$is_replicated" == "1" ]; then
    if [ "$replication_type" == "raft" ]; then
        OCC_CONFIG="config/occ_raft.yml"
    else
        OCC_CONFIG="config/occ_paxos.yml"
    fi
    CMD="$CMD -F config/1leader_2followers/paxos${trd}_shardidx${shard}.yml \
         -F $OCC_CONFIG --is-replicated --replication=$replication_type"
fi
```

**Key features**:
- Selects `occ_raft.yml` or `occ_paxos.yml` based on `$replication_type`
- Passes `--replication=$replication_type` to `dbtest`, which calls
  `set_replication_type_from_string()` in `main()`
- Uses `GDB_PREFIX` from `util.sh` for optional debugging
- Sets `LD_LIBRARY_PATH` for shared libraries

### 8.2 `bash/shard_raft.sh` — Raft-Specific Launcher

The Raft-specific script (`bash/shard_raft.sh`, 39 lines) is a simpler
alternative that hardcodes the Raft configuration:

```bash
CMD="./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-index $shard \
     --shard-config $path/config/local-shards$nshard-warehouses$trd.yml \
     -F config/1leader_2followers/raft${trd}_shardidx${shard}.yml \
     -F config/occ_raft.yml -P $cluster --replication raft"
```

**Key differences from `shard.sh`**:

| Aspect | `shard.sh` | `shard_raft.sh` |
|--------|-----------|----------------|
| Protocol selection | `$7` arg (default paxos) | Hardcoded raft |
| Config file naming | `paxos${trd}_shardidx${shard}` | `raft${trd}_shardidx${shard}` |
| OCC config | Dynamic selection | Always `occ_raft.yml` |
| `--replication` flag | `=$replication_type` | `raft` (always) |
| GDB support | Yes (`util.sh` sourced) | No |
| Lines of code | 62 | 39 |
| Primary use | CI tests, production | Quick Raft-only testing |

Both scripts pass the same core arguments to `dbtest`:
`--num-threads`, `--shard-index`, `--shard-config`, `-F` (config files),
`-P` (process name), `--replication`, and optionally `--is-micro` and
`--is-replicated`.

### 8.3 Port Separation

The Raft cluster configs use port ranges starting at 27xxx to avoid
collisions with Paxos on 17xxx.  This separation is essential for CI
testing where both Raft and Paxos tests may run on the same machine:

| Protocol | Base Port Range | Heartbeat Port Range |
|----------|----------------|---------------------|
| Paxos | 17001-17999 | 27001-27999 |
| Raft | 27001-27999 | 37001-37999 |

The heartbeat port is always `base_port + CtrlPortDelta` where
`CtrlPortDelta = 10000`.

## 9. Functions That Do NOT Need Raft Changes

Several Mako functions work with Raft without any modification because
they only call the unified replication API:

| Function | Location | Why No Changes Needed |
|----------|----------|----------------------|
| `register_paxos_leader_callback()` | `mako.hh:400` | Calls `register_for_leader_par_id_return()` which dispatches |
| `register_paxos_follower_callback()` | `mako.hh:240` | Calls `register_for_follower_par_id_return()` which dispatches |
| `wait_for_termination()` | `mako.hh:546` | Polls `benchConfig.getEndReceived()`, protocol-independent |
| `cleanup_and_shutdown()` | `mako.hh:763` | Calls `pre_shutdown_step()` / `shutdown_paxos()` which dispatch |
| `prepare_paxos_args()` | `mako.hh:818` | Builds argc/argv consumed by `Config::CreateConfig()` |
| `setup_sync_util_callbacks()` | `mako.hh:585` | Calls `get_epoch()` which dispatches |

This validates the dispatcher architecture's design goal: Mako's transaction
engine is completely protocol-agnostic.

## 10. Summary of Mako-Side Raft Changes

| Change Type | Location | Lines | Description |
|-------------|----------|-------|-------------|
| Auto-detection | `mako.hh:779-816` | 38 | `detect_replication_type_from_config()` scans YAML for `ab: raft` |
| Guard clause | `mako.hh:650` | 3 | `is_using_raft()` in FAIL_NEW_VERSION case 0 |
| Guard clause | `mako.hh:662` | 3 | `is_using_raft()` in FAIL_NEW_VERSION case 2 |
| Guard clause | `mako.hh:700` | 3 | `is_using_raft()` in !FAIL_NEW_VERSION case 0 |
| Guard clause | `mako.hh:722` | 3 | `is_using_raft()` in !FAIL_NEW_VERSION case 2 |
| Config file | `config/occ_raft.yml` | 10 | Raft-mode OCC configuration |
| Launch script | `bash/shard_raft.sh` | 39 | Raft-specific shard launcher |
| Launch script | `bash/shard.sh:36-41` | 6 | Protocol-switching in unified launcher |

**Total Mako-side changes**: ~105 lines (5 guard clauses + 1 function + 2 scripts + 1 config).
The vast majority of the Raft integration lives in `src/deptran/`, not in
Mako.
