# Standalone Raft Test Framework

## 1. Overview

The standalone Raft test framework validates the correctness of the Raft
consensus implementation independently of Mako's transaction processing.
It runs 11 sequential test cases covering leader election, log agreement,
network partitions, concurrency, and the Figure 8 scenario from the Raft
paper.  The framework is compiled conditionally via the `RAFT_TEST_CORO`
flag and executes inside the existing coroutine reactor, requiring no
external test runner.

**Key source files:**

| File | Lines | Purpose |
|------|-------|---------|
| `src/deptran/raft/test.h` | 44 | `RaftLabTest` class declaration (11 test methods) |
| `src/deptran/raft/test.cc` | 741 | All 11 test case implementations |
| `src/deptran/raft/testconf.h` | 184 | `RaftTestConfig` class with constants, helpers, network control |
| `src/deptran/raft/testconf.cc` | 586 | `RaftTestConfig` implementation |
| `src/deptran/raft/frame.cc` | lines 141-186 | Test coroutine bootstrap and replica synchronisation |
| `config/raft_lab_test.yml` | 55 | YAML config defining 5-server cluster for tests |

## 2. Compile-Time Activation

The test framework is gated behind the `RAFT_TEST_CORO` preprocessor
macro, controlled by the CMake option `RAFT_TEST`:

```cmake
# CMakeLists.txt:227-229
if(RAFT_TEST)
  add_compile_definitions(RAFT_TEST_CORO=1)
endif()
```

When `RAFT_TEST_CORO` is not defined, all test classes, helper macros,
and the bootstrap coroutine in `frame.cc` are compiled out.  This ensures
zero overhead in production builds.

## 3. Configuration: `raft_lab_test.yml`

The test cluster is defined in `config/raft_lab_test.yml`:

```yaml
mode:
  cc: none        # No concurrency control (pure Raft replication)
  ab: raft        # Atomic broadcast using Raft
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

Key design choices:
- **`cc: none`**: Disables Mako's concurrency control layer.  Tests
  exercise only the Raft consensus protocol.
- **5 servers**: Matches `NSERVERS` constant (tolerates 2 failures).
- **All localhost**: Single-machine execution for deterministic testing.

## 4. Test Constants

Constants are defined as preprocessor macros in `testconf.h:12-24`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `NSERVERS` | 5 | Number of Raft replicas in the test cluster |
| `ELECTIONTIMEOUT` | 5,000,000 us (5 s) | Generous timeout for leader election |
| `MAXSLOW` | 27 ms | Maximum simulated network latency |
| `DOWNRATE_N / DOWNRATE_D` | 1 / 10 | 10% chance of server disconnection per period |
| `ELECTIONRPCS` | `3 * NSERVERS` = 15 | Expected upper bound RPCs for one election |
| `COMMITRPCS(n)` | `(n+1) * NSERVERS` | Expected RPCs for `n` agreement rounds |

## 5. Architecture

### 5.1 Bootstrap Sequence

The test framework is bootstrapped inside `RaftFrame::CreateCommo()`
(`frame.cc:141-186`).  Only the frame instance with `locale_id == 0`
manages the test coroutine:

```
1. RaftFrame::CreateCommo() called for each of 5 replicas
2. Site 0 creates raft_test_coro_ (Fiber::create_run)
3. Coroutine immediately yields (yield_())
4. Site 0 spin-waits until n_commo_created_ == 5
5. ContinueCoro(raft_test_coro_) resumes test execution
6. RaftTestConfig is constructed with all 5 frames
7. RaftLabTest::Run() executes 11 tests sequentially
8. Cleanup() calls Shutdown(), reactor loop is stopped
```

Static members coordinate across frames:

| Static Member | Type | Purpose |
|---------------|------|---------|
| `frames_` | `map<siteid_t, RaftFrame*>` | All 5 frame pointers |
| `n_replicas_` | `uint16_t` | Expected replica count (5) |
| `n_commo_created_` | `uint16_t` | Counter for synchronisation barrier |
| `raft_test_coro_` | `shared_ptr<Fiber>` | The test coroutine |
| `raft_test_mutex_` | `mutex` | Guards `n_commo_created_` |

### 5.2 Class Hierarchy

```
RaftLabTest                    RaftTestConfig
├── config_ : RaftTestConfig*  ├── replicas (static)       : map<siteid_t, RaftFrame*>
├── index_  : uint64_t         ├── commit_callbacks (static): map<siteid_t, function>
├── init_rpcs_ : uint64_t      ├── committed_cmds (static) : map<siteid_t, vector<int>>
│                               ├── rpc_count_last (static) : map<siteid_t, uint64_t>
├── Run()                       ├── disconnected_           : map<siteid_t, bool>
├── Cleanup()                   ├── disconnect_mtx_         : mutex
│                               │
├── testInitialElection()       ├── OneLeader() / NoLeader()
├── testReElection()            ├── OneTerm() / TermMovedOn()
├── testBasicAgree()            ├── NCommitted()
├── testFailAgree()             ├── Start() / DoAgreement() / Wait()
├── testFailNoAgree()           ├── Disconnect() / Reconnect()
├── testRejoin()                ├── SetUnreliable() / Shutdown()
├── testConcurrentStarts()      ├── RpcCount() / RpcTotal()
├── testBackup()                ├── ServerCommitted()
├── testCount()                 └── netctlLoop() [background thread]
├── testUnreliableAgree()
└── testFigure8()
```

`RaftLabTest` owns the test sequencing logic.  `RaftTestConfig` owns the
cluster manipulation primitives (disconnect, reconnect, commit tracking,
RPC counting).

### 5.3 Commit Tracking (`SetLearnerAction`)

`SetLearnerAction()` (`testconf.cc:30-48`) registers a callback on each
`RaftServer` via `RegLearnerAction()`.  When a server commits a log entry,
the callback:

1. Verifies the command is a `TpcCommitCommand`
2. Extracts `tx_id_` (used as the command value in tests)
3. Appends `tx_id_` to `committed_cmds[svr]`

This provides a per-server commit log that tests inspect via
`NCommitted(index)` to verify how many servers have committed a given
log index and whether they agree on the value.

The `committed_cmds` map is initialised with `[-1]` at index 0 for
every server, so test indices start at 1.

## 6. Network Simulation

### 6.1 Disconnect / Reconnect

`RaftTestConfig::Disconnect(svr)` and `Reconnect(svr)` (`testconf.cc:320-332`)
simulate network partitions by calling `RaftServer::Disconnect()` and
`RaftServer::Reconnect()` (`server.cc:409-441`).

The server-level disconnect works by swapping the RPC proxy maps:

```
Disconnect:
  rpc_par_proxies_[partition][site] → saved in static _proxies map
  rpc_par_proxies_[partition][site] = {} (empty)

Reconnect:
  rpc_par_proxies_[partition][site] = _proxies[partition][site] (restored)
```

A disconnected server can still process local operations but cannot send
or receive RPCs.  Its `disconnected_` flag is checked by the election and
heartbeat timers to suppress outgoing messages.

### 6.2 Unreliable Network (`netctlLoop`)

`netctlLoop()` (`testconf.cc:415-482`) is a background thread that
simulates an unreliable network when activated by `SetUnreliable(true)`:

```
Loop (100ms periods):
  For each non-Disconnect()ed server:
    With 1/10 probability → disconnect(svr)
    Otherwise → reconnect(svr) + slow(svr, rand() % 27 ms)
```

State machine for `cv_m_`:
- **State 0**: `unreliable_ == false && finished_ == false` (idle)
- **State 1**: Waiting on `cv_` for `unreliable_` or `finished_`
- **State 2**: `unreliable_ == true && finished_ == false` (active)
- **State 3**: `finished_ == true` (terminating)

`Shutdown()` sets `finished_ = true`, signals the condition variable,
joins the thread, then reconnects all `Disconnect()`ed servers.

### 6.3 Slow Network

`slow(svr, msec)` (`testconf.cc:509-513`) introduces per-server latency
by sleeping `msec` milliseconds via `usleep()`.  In unreliable mode,
servers that are not disconnected get a random delay of 0-26 ms applied
each 100 ms period.

## 7. Key Test Utilities

### 7.1 `OneLeader(expected)` / `NoLeader()`

`waitOneLeader()` (`testconf.cc:59-102`) retries up to 10 times with
`ELECTIONTIMEOUT / 10` (500 ms) sleeps between attempts.  It iterates
all non-disconnected replicas, calls `GetState(&isleader, &term)`, and
returns the leader with the highest term.

Failure modes:
- Returns `-2` if multiple leaders exist in the same term
- Returns `-3` if `expected` is specified and the leader differs
- Returns `-1` if no leader found after 10 retries

### 7.2 `Start(svr, cmd, &index, &term)`

`Start()` (`testconf.cc:161-182`) constructs a `TpcCommitCommand` with
`tx_id_ = cmd` and calls `RaftServer::Start()`.  Returns `true` if the
server accepted the command (i.e., it believes it is the leader), along
with the assigned log `index` and current `term`.

### 7.3 `DoAgreement(cmd, n, retry)`

`DoAgreement()` (`testconf.cc:214-290`) is the primary agreement driver:

1. **Outer loop** (10 second timeout): Tries `Start()` on each
   non-disconnected server until a leader accepts
2. **Inner loop** (10 second timeout, 20 ms polling): Polls
   `NCommitted(index)` until `n` servers have committed
3. Verifies the committed value matches `cmd`
4. If `retry == true`, retries from step 1 on failure

### 7.4 `Wait(index, n, term)`

`Wait()` (`testconf.cc:184-212`) uses exponential backoff (10 ms to 1 s,
30 iterations max) to poll `NCommitted(index)`.  Returns:

| Value | Meaning |
|-------|---------|
| `>= 0` | The committed command value |
| `-1` | Timeout: not enough servers committed |
| `-2` | Term changed (stale leader) |
| `-3` | Committed values differ across servers |

### 7.5 `NCommitted(index)`

`NCommitted()` (`testconf.cc:135-159`) counts how many servers have
committed log index `index` by checking `committed_cmds[svr].size() > index`.
Also verifies all servers that committed the index agree on the value.
Returns `-1` if values disagree.

### 7.6 `RpcCount(svr, reset)` / `RpcTotal()`

`RpcCount()` (`testconf.cc:389-399`) reads `commo_->rpc_count_` under
`rpc_mtx_` and returns the delta since the last reset.  Used by
`testCount` to verify the implementation does not send excessive RPCs.

### 7.7 Server ID Helpers

Since server IDs in the config may not be 0-4, three helpers abstract
the mapping (`testconf.cc:519-581`):

| Method | Purpose |
|--------|---------|
| `mapServerId(id)` | Maps actual `siteid_t` to position 0-4 |
| `getServerIdByIndex(i)` | Returns `siteid_t` at position `i` |
| `getNextServerId(id, offset)` | Wraps around: `(pos + offset) % 5` |

These are used extensively in tests to disconnect/reconnect servers
relative to the current leader without hardcoding IDs.

## 8. Test Macros

The framework defines assertion and helper macros in `test.cc:43-89`:

| Macro | Purpose |
|-------|---------|
| `Init2(id, desc)` | Prints test header, verifies clean state (no disconnections, reliable network) |
| `Passed2()` | Prints pass message and returns 0 |
| `Assert(expr)` | Returns 1 on failure (silent) |
| `Assert2(expr, msg, ...)` | Prints failure message on false, returns 1 |
| `AssertOneLeader(ldr)` | Asserts `ldr >= 0` |
| `AssertReElection(ldr, old)` | Asserts `ldr != old` |
| `AssertNoneCommitted(index)` | Asserts `NCommitted(index) == 0` |
| `AssertNCommitted(index, n)` | Asserts `NCommitted(index) == n` |
| `AssertStartOk(ok)` | Asserts `Start()` returned true |
| `AssertWaitNoError(ret, index)` | Asserts `ret != -3` (no value disagreement) |
| `AssertWaitNoTimeout(ret, index, n)` | Asserts `ret != -1` and `ret != -2` |
| `DoAgreeAndAssertIndex(cmd, n, index)` | Calls `DoAgreement` and checks returned index |
| `DoAgreeAndAssertWaitSuccess(cmd, n)` | Calls `DoAgreement` with retry and updates `index_` |

## 9. Test Execution Flow

`RaftLabTest::Run()` (`test.cc:10-37`) executes all 11 tests in a
short-circuit OR chain:

```cpp
if (testInitialElection()
    || testReElection()
    || testBasicAgree()
    || ...
    || testFigure8()) {
  Print("TESTS FAILED");
  return 1;
}
Print("ALL TESTS PASSED");
```

Each test returns 0 on success, non-zero on failure.  The OR chain stops
at the first failure.  The `TEST_EXPAND(x)` macro is defined as just `x`
in the current build (it can be changed to `x || x || x || x || x` for
repeated-execution stress testing).

**State carried between tests:**

| Field | Carried | Purpose |
|-------|---------|---------|
| `index_` | Yes | Next expected commit index (starts at 1, incremented by each agreement) |
| `init_rpcs_` | Yes | RPC count from test 1, checked in test 9 |
| `committed_cmds` | Yes | Cumulative commit log per server |

Tests expect `NDisconnected() == 0` and `!IsUnreliable()` at the start
of each test (enforced by `Init2`).

## 10. Summary of the 11 Test Cases

| # | Name | What It Tests |
|---|------|---------------|
| 1 | `testInitialElection` | Leader elected, term agreed, stable leadership |
| 2 | `testReElection` | New election after leader disconnect; quorum break → no leader; quorum restore → leader |
| 3 | `testBasicAgree` | 3 sequential agreements with all 5 servers |
| 4 | `testFailAgree` | Agreements succeed with 2 followers disconnected (N-2 quorum) |
| 5 | `testFailNoAgree` | No agreement possible with 3 followers disconnected (no quorum) |
| 6 | `testRejoin` | Old leader rejoins after new leader commits; verifies log consistency |
| 7 | `testConcurrentStarts` | 5 concurrent `pthread` `Start()` calls; all values committed correctly |
| 8 | `testBackup` | 50 uncommitted entries on minority; swap quorum; verify backfill of 50 correct entries |
| 9 | `testCount` | RPC counts: `init_rpcs_ <= 30`, `COMMITRPCS(10)` for agreements, `<= 60` for 1 s idle |
| 10 | `testUnreliableAgree` | Unreliable network with 50 iterations x 4 concurrent threads |
| 11 | `testFigure8` | Leader completeness property: leader must not commit entries from previous terms using only the entry's replication count |

Each test case is documented in detail in `test_cases.md`.

## 11. Design Decisions

### Coroutine-Based Execution

Tests run inside the same coroutine reactor as the Raft servers.  This
means `Fiber::sleep()` yields to the reactor rather than blocking an OS
thread, allowing Raft heartbeats and election timers to fire during test
sleeps.  The only exception is `usleep()` calls in `DoAgreement()` and
`netctlLoop()`, which block the OS thread (used for timing that must be
independent of the reactor).

### Static State

`RaftTestConfig` uses static maps (`replicas`, `commit_callbacks`,
`committed_cmds`, `rpc_count_last`) because only one test configuration
exists per process lifetime.  The `committed_cmds` log is never cleared
between tests, allowing later tests to build on earlier agreements.

### `TpcCommitCommand` as Test Payload

Tests use `TpcCommitCommand` with `tx_id_` as the value field.  This
reuses Mako's existing command marshalling infrastructure rather than
creating a test-specific command type.  The `SetLearnerAction` callback
extracts `tx_id_` to record committed values.

### Disconnect via Proxy Swap

Rather than simulating network failures at the TCP level, `Disconnect()`
empties the RPC proxy map for a server.  This is equivalent to a
network partition: the server has no outbound connections.  The approach
is deterministic (no timing-dependent packet drops) and immediate (no
connection timeout delay).
