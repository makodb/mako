# Raft Potential Issues and Improvement Opportunities

Date: 2026-04-11

Scope reviewed:
- `docs/raft-book.md`
- `src/deptran/raft/*`
- Related runtime/shutdown paths in `src/deptran/server_worker.cc`, `src/deptran/s_main.cc`, and `src/srpc/rpc/client.{cpp,hpp}`

Notes:
- This is a risk audit, not a claim that every item is an active production bug.
- Items are prioritized by likely correctness/reliability impact first, then performance and maintainability.

## High-priority risks

1. Shutdown synchronization depends on non-atomic flags and timing sleeps
- Evidence:
  - `stop_`, `in_applying_logs_`, `looping_` are plain `bool` fields (`src/deptran/raft/server.h:115`, `:124`, `:134`).
  - Election timer loop reads `stop_` from a fiber (`src/deptran/raft/server.cc:1527-1570`).
  - Destructor sets `stop_` and uses a fixed 100ms sleep to avoid teardown races (`src/deptran/raft/server.cc:1121-1167`).
- Why it matters:
  - Cross-thread unsynchronized bool access is a data-race risk.
  - Time-based teardown (`sleep_for(100ms)`) is brittle under load and scheduler variance.
- Improvement:
  - Make lifecycle flags atomic or fully lock-protected.
  - Replace sleep-based teardown with explicit cancellation + join/await of all Raft background loops.

2. Leadership transfer monitor is detached on stop
- Evidence:
  - `StopLeadershipTransferMonitoring()` detaches `leadership_monitor_thread_` (`src/deptran/raft/server.cc:1994-2002`).
- Why it matters:
  - Detached thread can continue running after object lifetime transitions; this complicates UAF-proof teardown.
- Improvement:
  - Use cooperative stop signal + `join()` in all teardown paths.
  - If deadlock is a concern, use bounded join plus explicit state transitions rather than detach.

3. RPC client reconnect/retry model uses detached `std::thread`
- Evidence:
  - Auto-reconnect spawns detached thread (`src/srpc/rpc/client.cpp:823-845`).
  - `request_with_options()` spawns one detached thread per request (`src/srpc/rpc/client.hpp:1121-1123`, `:1250`).
- Why it matters:
  - Can create high thread churn under failures/timeouts.
  - Detached workers complicate shutdown determinism and resource accounting.
- Improvement:
  - Route reconnect/retry work through a shared executor (or poll thread job queue) with lifecycle ownership.
  - Ensure `close()`/destructor drains or cancels pending async work deterministically.

4. Durable-ack processing is O(lastLogIndex) per RPC
- Evidence:
  - On each `AppendEntriesDurable`, code loops from index 1..`lastLogIndex` and inserts into `durableAcks_` (`src/deptran/raft/server.cc:1474-1478`).
- Why it matters:
  - This scales poorly as logs grow and can dominate CPU/lock time.
- Improvement:
  - Track per-follower durable match index and apply only the delta range.
  - Periodically prune ack-tracking state at/below `securedLogIndex_`.

## Medium-priority risks and gaps

5. Raft hot path logs at `Log_info` frequency
- Evidence:
  - Frequent commit and batch logs in heartbeat loop (`src/deptran/raft/server.cc:769-777`, `:843-886`).
- Why it matters:
  - Logging overhead can materially affect throughput and election stability in noisy runs.
- Improvement:
  - Move repetitive per-heartbeat/per-follower logs to debug/trace level or sample them.

6. Snapshot/compaction path appears incomplete in runtime integration
- Evidence:
  - Snapshot manager field exists (`src/deptran/raft/server.h:92`), `CompactLog()` exists (`src/deptran/raft/server.cc:206+`), but no obvious call sites in Raft runtime flow.
- Why it matters:
  - Without active compaction policy, persistent logs may grow unbounded and recovery costs rise.
- Improvement:
  - Wire snapshot trigger policy into commit/application pipeline.
  - Add end-to-end tests for snapshot creation, restart-from-snapshot, and post-snapshot catch-up.

7. Test-mode ownership semantics remain brittle
- Evidence:
  - In `RAFT_TEST_CORO`, `ServerWorker::ShutDown()` intentionally skips deleting `rep_sched_` to avoid stale-pointer double-free (`src/deptran/server_worker.cc:363-369`).
- Why it matters:
  - Avoids crash, but signals unresolved ownership boundaries between worker lifecycle and test kill/restart path.
- Improvement:
  - Refactor to a single explicit owner (`unique_ptr`-based) with clear transfer/replacement semantics.

8. Test-mode scheduler has an explicit dead-loop workaround
- Evidence:
  - `RAFT_TEST_CORO` path uses `Reactor::loop(true, true)` with comment "TODO, figure out a better approach" (`src/deptran/server_worker.cc:319-324`).
- Why it matters:
  - Behavior diverges from production and can mask lifecycle defects.
- Improvement:
  - Replace with explicit scheduler startup/shutdown contract per replica.

## Test completeness and reliability opportunities

9. Large parts of test suite are intentionally not run
- Evidence:
  - Runner executes grouped subsets; comments state speculative/integration/stress tests are intentionally disabled (`src/deptran/raft/test.cc:30-62`).
  - Persistence test 15 is explicitly disabled (`src/deptran/raft/test.cc:48-57`).
- Why it matters:
  - Regression surface remains untested in regular runs.
- Improvement:
  - Define CI tiers:
    - Tier 1: always-on deterministic core tests.
    - Tier 2: persistence/failure matrix.
    - Tier 3: long-running stress/flake-detection jobs.

10. Tests rely heavily on fixed sleeps and randomness
- Evidence:
  - Randomized crash selection seeded by wall clock (`std::srand(std::time(nullptr))`) in test 15 (`src/deptran/raft/test.cc:483`).
  - Extensive `Fiber::sleep(...)`-based synchronization throughout tests and harness; kill path uses fixed `usleep(450000)` (`src/deptran/raft/testconf.cc:579-583`).
- Why it matters:
  - Time-dependent tests are flakier across machines/load.
  - Failures are harder to reproduce without deterministic seeds.
- Improvement:
  - Use event/predicate waits with bounded deadlines instead of fixed delays.
  - Log and/or inject deterministic RNG seeds for reproducible failing runs.

11. Harness includes `verify(0)` stubs in callable paths
- Evidence:
  - `verify(0)` fallthrough/stub points in test config (`src/deptran/raft/testconf.cc:227`, `:309`).
- Why it matters:
  - If exercised unexpectedly, process aborts without graceful diagnostics.
- Improvement:
  - Replace with structured error returns and explicit test failure messages.

## Operational and code-health opportunities

12. Main shutdown path still uses fixed sleeps and `exit(0)`
- Evidence:
  - Hard sleeps before shutdown (`src/deptran/s_main.cc:650-653`).
  - Explicit `exit(0)` after `server_shutdown()` plus TODO around pending futures (`src/deptran/s_main.cc:753-757`).
- Why it matters:
  - Fixed delays hide readiness bugs.
  - `exit(0)` bypasses normal unwinding/RAII cleanup and can hide leaks.
- Improvement:
  - Replace sleeps with explicit quiescence checks and bounded waits.
  - Return normally from `main` after deterministic teardown.

13. Documentation drift in `raft-book.md`
- Evidence:
  - Book still lists shutdown hang as tolerated and describes `applyLogs()` bottleneck workaround (`docs/raft-book.md:800-816`).
  - Code has since evolved (`apply_pending_` looping in `src/deptran/raft/server.cc:649-684`; shutdown handling changed in worker path).
- Why it matters:
  - Readers may optimize/fix the wrong things or miss current risks.
- Improvement:
  - Update "Known Issues" to separate historical issues from current status.
  - Add a "last validated commit/date" stamp for operational sections.

## Suggested next implementation order

1. Teardown lifecycle hardening: eliminate detached background work in Raft and RPC reconnect/retry paths.
2. Replace non-atomic shutdown/apply flags with atomic or lock discipline, then run TSAN on Raft test mode.
3. Optimize durable ack bookkeeping and prune old ack state.
4. Expand deterministic CI test matrix, then re-enable test 15 behind a gate.
5. Update `docs/raft-book.md` to reflect current behavior and known-open risks.
