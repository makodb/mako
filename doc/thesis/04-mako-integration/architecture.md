# Mako-Raft Integration Architecture

## 1. The Integration Challenge

Mako was originally designed to use Multi-Paxos as its sole atomic broadcast
(replication) protocol.  The entire transaction pipeline---from log submission
by worker threads to commit-point callbacks on followers---was wired directly
to a `paxos_main_helper.cc` module that manages `PaxosWorker` instances.

Adding Raft as an alternative replication backend required solving three
problems simultaneously:

1. **API parity**: Mako worker threads call ~30 replication functions
   (`setup`, `add_log_to_nc`, `register_for_leader_par_id_return`, etc.).
   Each must work identically regardless of which consensus protocol is
   active.

2. **Runtime switching**: Operators must be able to choose the protocol via a
   CLI flag (`--replication raft`) or a YAML config field (`ab: raft`) without
   recompiling.

3. **Zero Mako-side changes**: The Mako storage engine, OCC layer, and
   benchmark harness must remain protocol-agnostic.  All Raft-specific logic
   must live behind the replication helper.

The solution is a three-layer dispatcher architecture: a thin **dispatch
layer** (`replication_helper.{h,cc}`) sits between Mako and two parallel
**implementation namespaces** (`paxos_impl` in `paxos_main_helper.cc`,
`raft_impl` in `raft_main_helper.cc`).

## 2. Dispatcher Architecture

### 2.1 Layer Diagram

```
+------------------------------------------------------------------+
|                        Mako Application                          |
|  (dbtest.cc, mako.hh, worker threads, TPC-C benchmark)          |
+------------------------------------------------------------------+
         |                                           |
         | setup(), add_log_to_nc(),                 | --replication raft
         | register_for_leader_par_id_return(), ...   | (CLI flag)
         v                                           v
+------------------------------------------------------------------+
|               replication_helper.h / .cc                         |
|  +------------------------------------------------------------+  |
|  | rusty::Cell<ReplicationType> g_replication_type {PAXOS}    |  |
|  +------------------------------------------------------------+  |
|  | DISPATCH_RAFT_OR_PAXOS(func, ...)                          |  |
|  | DISPATCH_VOID_RAFT_OR_PAXOS(func, ...)                     |  |
|  +------------------------------------------------------------+  |
|  | 30+ dispatch functions: setup(), add_log_to_nc(), ...      |  |
|  +------------------------------------------------------------+  |
+------------------------------------------------------------------+
         |                                 |
         | paxos_impl::func(...)           | raft_impl::func(...)
         v                                 v
+----------------------------+  +----------------------------+
|   paxos_main_helper.cc     |  |   raft_main_helper.cc      |
|   namespace paxos_impl     |  |   namespace raft_impl      |
|                            |  |                            |
|   PaxosWorker instances    |  |   RaftWorker instances     |
|   ElectionState            |  |   ElectionState            |
|   leader_replay_cb map     |  |   leader_replay_cb map     |
|   follower_replay_cb map   |  |   follower_replay_cb map   |
+----------------------------+  +----------------------------+
         |                                 |
         v                                 v
+----------------------------+  +----------------------------+
|   Paxos Protocol Layer     |  |   Raft Protocol Layer      |
|   (Multi-Paxos/copilot)    |  |   (RaftServer, RaftCommo)  |
+----------------------------+  +----------------------------+
```

### 2.2 Global State: `rusty::Cell<ReplicationType>`

The runtime protocol selection is stored in a single global variable using
RustyCpp's `Cell<T>` for safe interior mutability:

```cpp
// replication_helper.cc
namespace janus {
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};
}
```

`Cell<T>` is appropriate here because `ReplicationType` is a trivially
copyable enum (backed by `int`), the value is set once during initialization
and read many times, and `Cell::get()` / `Cell::set()` are safe operations
that do not require runtime borrow checking.

**Source**: `src/deptran/replication_helper.cc:12`

### 2.3 The `ReplicationType` Enum

```cpp
// replication_helper.h
enum class ReplicationType : int {
    PAXOS = 0,
    RAFT = 1
};
```

The explicit `int` backing type ensures the enum is trivially copyable,
which is a requirement for `rusty::Cell<T>`.

Convenience accessors are provided:

| Function | Purpose | Source |
|----------|---------|--------|
| `get_replication_type()` | Read current type via `Cell::get()` | `replication_helper.cc:15` |
| `set_replication_type(type)` | Write via `Cell::set()` | `replication_helper.cc:20` |
| `set_replication_type_from_string(str)` | Parse `"paxos"` or `"raft"` | `replication_helper.cc:28` |
| `replication_type_to_string(type)` | Pure function for logging | `replication_helper.cc:40` |
| `is_using_raft()` | Inline check | `replication_helper.h:38` |
| `is_using_paxos()` | Inline check | `replication_helper.h:39` |

### 2.4 Dispatch Macros

Two macros handle the branching:

```cpp
// replication_helper.cc:57-73
#define DISPATCH_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            return raft_impl::func(__VA_ARGS__); \
        } else { \
            return paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)

#define DISPATCH_VOID_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            raft_impl::func(__VA_ARGS__); \
        } else { \
            paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)
```

`DISPATCH_RAFT_OR_PAXOS` is used for functions that return a value (the
`return` statement forwards the callee's return value).
`DISPATCH_VOID_RAFT_OR_PAXOS` is used for `void`-returning functions.

Both macros read `g_replication_type` on every call via `is_using_raft()`.
Since the type is set once at startup and never changes during execution,
the branch predictor effectively eliminates this overhead.

### 2.5 Raft-Only Functions

One function is Raft-specific and has no Paxos counterpart:

```cpp
// replication_helper.cc:217-222
void set_preferred_leader(int site_id) {
    if (janus::is_using_raft()) {
        raft_impl::set_preferred_leader(site_id);
    }
    // No-op for Paxos
}
```

This is declared in `replication_helper.h:175` and exposed to Mako as a
top-level function.  Paxos uses a fixed-leader model and does not need
dynamic leader preference.

## 3. Protocol Detection: Two Mechanisms

### 3.1 Explicit CLI Flag

The `--replication` flag (short form `-R`) is parsed in `dbtest.cc`:

```cpp
// src/mako/benchmarks/dbtest.cc:31
{"replication", required_argument, 0, 'R'},

// dbtest.cc:112-113
case 'R':
    replication_type = string(optarg);
    break;
```

After argument parsing:

```cpp
// dbtest.cc:341-343
if (!replication_type.empty()) {
    janus::set_replication_type_from_string(replication_type);
}
```

This runs **before** any replication subsystem initialisation, ensuring the
dispatch macros route to the correct namespace from the very first call.

**Example command line**:
```bash
./build/dbtest --replication raft -F config/occ_raft.yml -P p0 ...
```

### 3.2 Automatic Config Detection

If no `--replication` flag is provided, `detect_replication_type_from_config()`
scans the YAML config files for the `ab:` field:

```cpp
// src/mako/mako.hh:779-816
static void detect_replication_type_from_config(const vector<string>& config_files) {
    if (janus::is_using_raft()) {
        return;  // Don't override explicit CLI setting
    }
    for (const auto& file_path : config_files) {
        std::ifstream ifs(file_path);
        // ... scan for "ab:" line ...
        if (value == "raft" || value == "fpga_raft") {
            janus::set_replication_type(janus::ReplicationType::RAFT);
            return;
        }
    }
}
```

This is called in `init_env()` (`mako.hh:889`) just before `setup()`:

```cpp
detect_replication_type_from_config(benchConfig.getPaxosConfigFile());
std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
```

**Priority**: The CLI flag takes precedence because
`set_replication_type_from_string` runs in `dbtest.cc:main()` before
`init_env()` is called, and `detect_replication_type_from_config()` checks
`is_using_raft()` as its first guard.

### 3.3 YAML Config Format

The `ab` field in the YAML mode section selects the atomic broadcast protocol:

```yaml
# config/none_raft.yml
mode:
  cc: none          # no transaction-level concurrency control
  ab: raft          # atomic broadcast → MODE_RAFT (0x400)
  batch: false
  retry: 20
  ongoing: 1
```

The former Rule concurrency-control mode and its Rule/Raft configuration were
retired. Generic Jetpack recovery machinery still exists in the replication
stack, but it is unsupported and is being evaluated in a separate audit.

The YAML parser in `Config::InitMode()` converts the `ab` string to a mode
constant via `Frame::Name2Mode()`:

```cpp
// config.cc:496
replica_proto_ = Frame::Name2Mode(ab_name);
```

The name-to-mode mapping is defined in `frame.cc:487`:

```cpp
{"raft",       MODE_RAFT},       // 0x400
{"fpga_raft",  MODE_FPGA_RAFT},  // 0x401
```

`raft_main_helper.cc` verifies consistency at startup (`raft_main_helper.cc:258`):

```cpp
if (config->replica_proto_ != MODE_RAFT) {
    Log_warn("[RAFT-SETUP] replica_proto_=%d is not MODE_RAFT...");
}
```

### 3.4 Detection Flow

```
dbtest.cc main()
    |
    +-- parse_command_line_args()    // -R flag → replication_type string
    |
    +-- set_replication_type_from_string()  // if -R was provided
    |
    +-- init_env()
         |
         +-- detect_replication_type_from_config()  // scan YAML for "ab: raft"
         |   (no-op if type already set to RAFT)
         |
         +-- setup()                 // dispatches to raft_impl::setup()
         |                           //  or paxos_impl::setup()
         +-- setup2()                // launches workers, preferred leader
```

## 4. Unified Replication API

The dispatch layer exposes 30+ functions that Mako calls without knowing
which protocol is active.  They group into five categories:

### 4.1 Lifecycle Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `setup()` | `(int argc, char* argv[]) -> vector<string>` | Parse configs, create workers, return site names |
| `setup2()` | `(int action, int shardIndex) -> int` | Launch RPC services, configure preferred leader, start heartbeats |
| `shutdown_paxos()` | `() -> int` | Drain queues, destroy workers, release configs |
| `pre_shutdown_step()` | `() -> void` | Gracefully disconnect heartbeat RPC before full shutdown |

### 4.2 Log Submission Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `submit()` | `(const char*, int, uint32_t) -> void` | Submit log to leader's queue |
| `add_log()` | `(const char*, int, uint32_t) -> void` | Alias for `submit()` |
| `add_log_without_queue()` | `(const char*, int, uint32_t) -> void` | Compatibility stub (still uses queue in Raft) |
| `add_log_to_nc()` | `(const char*, int, uint32_t, int) -> void` | Submit with batch size hint; waits for leadership if needed |
| `wait_for_submit()` | `(uint32_t) -> void` | Block until partition's submit queue is drained |

### 4.3 Callback Registration Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `register_for_follower()` | `(cb, par_id) -> void` | Simple follower apply callback |
| `register_for_follower_par_id()` | `(cb, par_id) -> void` | Follower callback with partition ID |
| `register_for_follower_par_id_return()` | `(cb, par_id) -> void` | Full watermark follower callback |
| `register_for_leader()` | `(cb, par_id) -> void` | Simple leader apply callback |
| `register_for_leader_par_id()` | `(cb, par_id) -> void` | Leader callback with partition ID |
| `register_for_leader_par_id_return()` | `(cb, par_id) -> void` | Full watermark leader callback |
| `register_leader_election_callback()` | `(cb) -> void` | Notification when leadership changes |

The watermark callback signature is:
```cpp
std::function<int(const char*& data, int len, int par_id, int epoch,
                  std::queue<std::tuple<int, int, int, int, const char*>>& unreplayed)>
```

### 4.4 Epoch and Election Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `get_epoch()` | `() -> int` | Current epoch from `ElectionState` |
| `set_epoch()` | `(int) -> void` | Update epoch, propagate to all workers |
| `upgrade_p1_to_leader()` | `() -> void` | Force leader callback invocation |
| `get_outstanding_logs()` | `(uint32_t) -> int` | Unreplicated log count: `n_tot - commitIndex` |

### 4.5 Benchmark and Diagnostic Functions

| Function | Purpose |
|----------|---------|
| `microbench_paxos()` | Microbenchmark (stubs in Raft) |
| `microbench_paxos_queue()` | Queue microbenchmark (stubs in Raft) |
| `worker_info_stats()` | Per-partition counter dump |
| `getHosts()` | YAML host bindings parser |

## 5. Namespace Symmetry: `paxos_impl` vs `raft_impl`

Both namespaces declare identical function signatures.  This is enforced by
the forward declarations in `replication_helper.h`:

```cpp
// replication_helper.h:46-85
namespace paxos_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
// ... 30+ declarations ...
}

// replication_helper.h:90-130
namespace raft_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
// ... 30+ declarations ...
void set_preferred_leader(int site_id);  // Raft-only
}
```

The linker enforces that both namespaces provide implementations for every
declared function.  Any missing function causes a link error, not a runtime
crash.

### 5.1 Key Structural Differences

| Aspect | `paxos_impl` | `raft_impl` |
|--------|-------------|-------------|
| Source file | `paxos_main_helper.cc` | `raft_main_helper.cc` |
| Worker type | `PaxosWorker` | `RaftWorker` |
| Worker storage | `pxs_workers_g` (vector) | `raft_workers_g` (vector) |
| Leader model | Fixed leader (machine 0) | Preferred leader (election bias) |
| `setup2()` | Sets `ElectionState`, calls `server_launch_worker` | Configures per-partition preferred leader, then launches |
| `add_log_to_nc()` | Immediate enqueue | Checks `IsLeader()` per-partition |
| `shutdown_paxos()` | Drains queues, destroys config | Same pattern via `WaitForShutdown()` + `ShutDown()` |
| Callback storage | `leader_replay_cb` map only | Both `leader_replay_cb` and `follower_replay_cb` maps |
| `set_preferred_leader()` | (not present) | Iterates workers, calls `RaftServer::SetPreferredLeader()` |
| Legacy Jetpack recovery | Enabled unless `MAKO_DISABLE_JETPACK` is set; unsupported and under audit | Forced disabled (`MAKO_DISABLE_JETPACK=1`) |

### 5.2 Callback Handling Difference

In Paxos, callbacks are registered only for the current role (leader or
follower) because leadership never changes during normal operation.

In Raft, leadership can change at any time, so `raft_main_helper.cc`
stores **both** leader and follower callbacks in separate maps and
registers them both on every worker:

```cpp
// raft_main_helper.cc:209-226
void apply_callbacks_for_partition(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    auto leader_it = leader_replay_cb.find(par_id);
    if (leader_it != leader_replay_cb.end()) {
        worker->register_leader_callback_par_id_return(leader_it->second);
    }
    auto follower_it = follower_replay_cb.find(par_id);
    if (follower_it != follower_replay_cb.end()) {
        worker->register_follower_callback_par_id_return(follower_it->second);
    }
}
```

When `raft_handle_leader_change()` fires (`raft_main_helper.cc:732-740`),
it re-applies callbacks for the affected partition and notifies the
external callback:

```cpp
void raft_handle_leader_change(uint32_t partition_id, bool is_leader) {
    raft_impl::handle_leader_change_impl(partition_id);
    if (leader_callback_) {
        leader_callback_(is_leader ? 1 : 0);
    }
}
```

## 6. Initialization Sequence

The full Raft initialization sequence from `dbtest.cc` through to running
Raft workers is:

```
1. dbtest.cc::main()
   +-- parse_command_line_args()
   |   '--> -R "raft" → replication_type = "raft"
   +-- set_replication_type_from_string("raft")
   |   '--> g_replication_type.set(ReplicationType::RAFT)
   |
2. init_env()  [mako.hh:858]
   +-- detect_replication_type_from_config()  // no-op: already RAFT
   +-- prepare_paxos_args()                   // builds argc/argv for setup()
   +-- setup(argc, argv)
   |   '--> DISPATCH → raft_impl::setup()
   |        +-- Config::CreateConfig(argc, argv)
   |        |   '--> reads YAML, sets replica_proto_ = MODE_RAFT
   |        +-- for each server site:
   |        |   +-- RaftWorker::new()
   |        |   +-- worker->SetupBase()
   |        |   |   '--> Frame::GetFrame(MODE_RAFT) → RaftFrame
   |        |   |   '--> RaftFrame::CreateScheduler() → RaftServer
   |        |   +-- raft_workers_g.push_back(worker)
   |        +-- returns site names
   |
   +-- setup_paxos_leader_callbacks()         // registers watermark callbacks
   +-- setup_paxos_follower_callbacks()       // (name is legacy, works with Raft)
   |
   +-- setup2(0, shardIndex)
   |   '--> DISPATCH → raft_impl::setup2()
   |        +-- for each worker:
   |        |   +-- GetRaftServer()
   |        |   +-- find locale_id==0 site in partition
   |        |   +-- SetPreferredLeader(preferred_site_id)
   |        +-- server_launch_worker()
   |             +-- SetupService()      // register RPC handlers
   |             +-- SetupCommo()        // connect to peers
   |             +-- EnsureSetup()       // start election timer + heartbeat
   |             +-- StartSubmitThread() // background log submission
   |             +-- SetupHeartbeat()    // control-plane heartbeat RPC
   |
   +-- sleep(3)                               // wait for all nodes to start
```

## 7. Mako Call Sites

Mako itself is entirely protocol-agnostic.  It calls the top-level dispatch
functions defined in `replication_helper.h`.  Key call sites:

### 7.1 Log Submission (Hot Path)

From `mako.hh`:

```cpp
// mako.hh:953 - End-of-experiment signal
add_log_to_nc((char*)endLogInd.c_str(), 0, i);
```

Worker threads call `add_log_to_nc()` to replicate transaction logs.  The
dispatch routes to `raft_impl::add_log_to_nc()` which:
1. Finds the worker for the partition (`find_worker`)
2. Checks `IsLeader()` --- drops if not leader
3. Calls `enqueue_to_worker()` which increments `n_submit` and queues the log

### 7.2 Callback Registration (Initialization)

```cpp
// mako.hh:900-901
setup_paxos_leader_callbacks(benchConfig.getAdvanceWatermarkTracker());
setup_paxos_follower_callbacks(replicated_db);
```

These functions (defined elsewhere in Mako) call the unified API:
- `register_for_leader_par_id_return(cb, par_id)` for leaders
- `register_for_follower_par_id_return(cb, par_id)` for followers

### 7.3 Shutdown

```cpp
// mako.hh:970+
void db_close() {
    // ...
    pre_shutdown_step();
    shutdown_paxos();
}
```

Both functions dispatch to `raft_impl` and cleanly tear down workers.

## 8. Naming Conventions and Legacy Compatibility

Several function names retain "paxos" in their identifiers even when called
under Raft mode:

| Function | Reason |
|----------|--------|
| `shutdown_paxos()` | Original Paxos API; changing would break 100+ call sites |
| `setup_paxos_leader_callbacks()` | Mako-side function name |
| `prepare_paxos_args()` | Builds argc/argv for the config parser |
| `microbench_paxos()` | Placeholder for future Raft microbenchmarks |
| `getPaxosConfigFile()` | BenchmarkConfig accessor |

This is a deliberate design choice: the unified API maintains backward
compatibility by keeping the function names that Mako already uses, while
the dispatch layer transparently routes them to the correct implementation.

## 9. Safety Annotations

The dispatch layer is annotated following RustyCpp conventions:

- **Global state**: `rusty::Cell<ReplicationType>` (`@safe`, interior mutability)
- **Dispatch macros**: `@safe` (no ownership transfer, pure branching)
- **Dispatch functions**: `@unsafe` (delegate to non-borrow-checked legacy code)
- **Implementation namespaces**: `@unsafe` (direct interaction with RPC, I/O, and
  legacy worker infrastructure)

The dispatch layer itself is safe---it only reads a `Cell` and forwards
arguments by value or reference.  The unsafety is contained within the
implementation namespaces where workers interact with the RPC framework,
file I/O, and raw pointers from legacy code.
