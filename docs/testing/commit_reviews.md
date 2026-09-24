# Commit Reviews

This file contains code review findings for commits on the mako-dev branch.
Each commit may have multiple issues tracked with severity levels:
- S2 = medium
- S3 = high
- S4 = critical

---

# Summary

## Open Issues (requiring action)

| Issue ID | Severity | Commit | Category | Brief Description |
|----------|----------|--------|----------|-------------------|

## Addressed Issues

| Issue ID | Severity | Commit | Addressed By |
|----------|----------|--------|--------------|
| ISSUE-db176a90-1 | S2 | db176a90 | (next commit) |
| ISSUE-db176a90-2 | S2 | db176a90 | (next commit) |
| ISSUE-232ba3b0-1 | S3 | 232ba3b0 | db176a90 |
| ISSUE-232ba3b0-2 | S3 | 232ba3b0 | db176a90 |
| ISSUE-1886cab7-1 | S3 | 1886cab7 | 7a6a5847 |
| ISSUE-1886cab7-2 | S3 | 1886cab7 | 844e6c99 |
| ISSUE-1886cab7-3 | S2 | 1886cab7 | fb6d9d92 |
| ISSUE-33b02756-1 | S2 | 33b02756 | 7a6a5847 |
| ISSUE-33b02756-2 | S2 | 33b02756 | 69f8ba0e |
| ISSUE-131c2bff-1 | S2 | 131c2bff | 1886cab7 |
| ISSUE-6a5f8ad0-1 | S2 | 6a5f8ad0 | 131c2bff, 1886cab7 |

---

*Last updated: 2026-02-20 (25c87ef5 reviewed)*

---

## Commit 25c87ef5 - "Expand borrow checking coverage, fix 3 safety annotations"
**Date**: 2026-02-20
**Author**: Shuai Mu

**Changes**: CMakeLists.txt (+2 files to borrow checking, +6 documented exclusions), server.h (AtomicFlag @safe→@unsafe), replication_helper.cc (set_replication_type @safe→@unsafe), recovery_manager.hpp (success_fresh @safe→@unsafe)

**Verdict**: No issues found (correct safety annotation fixes expanding borrow check coverage)

Good changes:
- All 3 annotation fixes are correct: `mutable std::atomic` (interior mutability), `std::cerr` (unchecked I/O), `std::string` construction (unchecked STL)
- 6 excluded files properly documented with violation counts from header code (not the source files themselves)
- Brings borrow-checked file count to 12

---

## Commit dae2bc06 - "commit TODO.md"
**Date**: 2026-02-20

**Verdict**: No issues found (TODO.md timestamp update only)

---

## Commit 754cc0b2 - "Fix CI test infrastructure: shebang, python3, borrow check"
**Date**: 2026-02-20
**Author**: Shuai Mu

**Changes**: ci/ci.sh (shebang fix + python3), simple_transaction_rep_port_utils.sh (python3), masstree_key.hh (@safe→@unsafe annotation fix)

**Verdict**: No issues found (correct infrastructure fixes)

Good changes:
- Shebang fix: removed leading newline before `#!/bin/bash` in ci.sh that caused fallback to `sh` (which lacks process substitution)
- `python` → `python3` in 4 call sites across 2 files (necessary since `python` binary not available)
- `unparse_ikey()` correctly re-annotated from `@safe` to `@unsafe` since it calls `unparse()` which writes to raw buffer via `memcpy`

---

## Commit c253cc36 - "docs: add code-verified user manual"
**Date**: 2026-02-20
**Author**: Shuai Mu

**Changes**: docs/user-manual.md (430 lines, new file)

**Verdict**: No issues found (documentation only — user manual with no code changes)

---

## Commit db176a90 - "Fix 2 Raft async persistence bugs from commit 232ba3b0"
**Date**: 2026-02-10

**Changes**: server.cc (59 insertions, 22 deletions), server.h (29 insertions, 13 deletions)

**Verdict**: Two medium-severity issues found

### ISSUE-db176a90-1 (S2 medium): Mutex held during thread join in destructor
- Destructor holds `async_threads_mtx_` while calling `join()` on all threads
- If an in-flight RPC handler (past `stop_` check) tries to `emplace_back`, it deadlocks
- **Fix**: Swap vector out under lock, join without lock

### ISSUE-db176a90-2 (S2 medium): Unbounded thread handle accumulation
- `async_threads_` vector is append-only; completed thread handles never pruned
- Over thousands of operations, accumulates finished `std::thread` objects (minor resource leak)
- **Fix**: Prune completed threads (via atomic done flag) at each insertion

---

## Commit 7a75d1af - "Hourly CI check"
**Date**: 2026-02-10

**Verdict**: No issues found (timestamp update only)

---

## Commit 55ea7d19 - "Stabilize CI ports and cleanup checks"
**Date**: 2026-02-10

**Verdict**: No issues found (CI infrastructure hardening - user-scoped cleanup, random port allocation, retry logic)

---

## Commit 321a6db9 - "Hourly CI check"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit e923a180 - "Hourly CI check"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit 096018bc - "Hourly CI check"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit 6e4625c6 - "Harden simpleTransaction ports"
**Date**: 2026-02-09

**Verdict**: No issues found (dynamic port allocation for simpleTransaction via MAKO_CONFIG env var)

---

## Commit 4e847f89 - "Fix RPC test port collisions"
**Date**: 2026-02-09

**Verdict**: No issues found (test infrastructure - migration to test_ports::get_port())

---

## Commit 1000c96c - "Record hourly CI check"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit 934dc023 - "Fix rpc_metrics test port collisions"
**Date**: 2026-02-09

**Verdict**: No issues found (test infrastructure - migration to test_ports::get_port())

---

## Commit b3ac71cf - "Update hourly and daily CI check notes"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit 96ff9cf9 - "Update hourly CI check"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit aefdc2fa - "Update daily recurring task timestamps"
**Date**: 2026-02-09

**Verdict**: No issues found (timestamp update only)

---

## Commit 232ba3b0 - "Zeyu raft disk (#53)"
**Date**: 2026-02-09
**Author**: Zeyu

Large commit adding Raft disk persistence with sync/async modes, speculative replication protocol, parallel heartbeat dispatch, atomic service pointer for kill/restart, and 21 new tests.

### ISSUE-232ba3b0-1 [S3 - high]
**Category**: Memory safety / use-after-free
**Evidence**: `src/deptran/raft/server.h` (doVote async path) and `src/deptran/raft/server.cc` (OnAppendEntries async path)
**Problem**: In the async persistence path, `std::thread(...).detach()` captures `this` (the `RaftServer*`). If the server is destroyed (e.g., during Kill/Restart in tests or shutdown) while the thread is running, the detached thread will access a dangling `this` pointer causing use-after-free. Example:
```cpp
std::thread([this, term_copy, voter_copy, can_id_copy, par_id_copy]() {
    PersistState(term_copy, can_id_copy, "doVote: async vote persist");
    auto c = commo();
    if (c != nullptr) {
        c->SendVoteDurable(can_id_copy, par_id_copy, term_copy, voter_copy);
    }
}).detach();
```
**Action**: Use `shared_from_this()` / weak references, or track spawned threads and join them in the destructor.

### ISSUE-232ba3b0-2 [S3 - high]
**Category**: Correctness / quorum logic
**Evidence**: `src/deptran/raft/server.cc` - `OnPeerRestart()` method
**Problem**: `durableVoters_` already contains `site_id_` (inserted during `ResetSpeculativeState()` and election win), but `OnPeerRestart()` computes `durable_vote_count = durableVoters_.size() + 1` adding another `+1` "for self". This double-counts self, making quorum checks off-by-one in the leader's favor. With 5 nodes (quorum=3), the leader could think it has 3 durable votes when it only has 2. Same issue for `specVoters_.size() + 1`.
**Action**: Remove the `+1` since self is already in the set, or verify the set doesn't contain self before adding.

---

## Commit 36e4f8ee - "Fix rpc_chaos_test CI flakiness and update daily checks"
**Date**: 2026-02-09

**Verdict**: No issues found (test infrastructure - increased timing margins for CI CPU contention)

---

## Commit c84909cc - "Fix race condition in GetOrCreateClient causing intermittent segfault"
**Date**: 2026-02-03

**Verdict**: No issues found (correct fix - clone Arc while holding lock before unlocking)

---

## Commit 31eda945 - "Update daily checks and add CI failure investigation"
**Date**: 2026-02-03

**Verdict**: No issues found (timestamp update + investigation notes)

---

## Commit a6bed72c - "Reorganize documentation structure"
**Date**: 2026-02-03
**Author**: Shuai Mu

**Verdict**: No issues found (documentation reorganization only - no code changes)

Major cleanup moving docs to organized subdirectories (getting-started/, architecture/, developer/, rpc/, migration/, plans/, etc.). Removed stubs and outdated docs. Updated index.md navigation.

**CI Note**: Run #21649096526 failed with intermittent segfault in shardNoReplication test. Investigation shows this is NOT caused by this commit (no C++ changes). Test passes consistently locally (4/4 runs). The failure is an intermittent race condition in SrpcRpcBackend::Stop during shutdown, likely triggered by CI environment timing.

---

## Commit a5d3c01c - "Move doc/ to docs/ and enable CI on PRs"
**Date**: 2026-02-03
**Author**: Shuai Mu

**Verdict**: No issues found (directory rename + CI trigger addition)

---

## Commit ff7de45d - "Update daily CI check timestamps [2026-02-01]"
**Date**: 2026-02-01
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit 183413a9 - "Update daily CI check timestamps [2026-01-31]"
**Date**: 2026-01-31
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit de5b9ec9 - "Update daily CI check timestamps [2026-01-29]"
**Date**: 2026-01-29
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit 92c58460 - "Unify client-server interfaces with RunMode enum [issue-1.md]"
**Date**: 2026-01-25
**Author**: shenweihai1

**Verdict**: No issues found (clean API unification)

Good implementation:
- Added `ClientConfig` struct to `mako::Options` in db.hh with proper @safe annotations
- Added `RemoteDB::Connect(const Options&, int, RemoteDB**)` overload with comprehensive validation
- Properly deprecated `RemoteOptions` instead of removing it (backward compat)
- Added `RunMode` enum for cleaner mode handling in simpleTransactionRep.cc
- Clean delegation pattern: new API converts to old RemoteOptions internally
- Well-documented with usage examples in plan file

---

## Commit 5226e4c3 - "Update submodules and hourly CI check [2026-01-24]"
**Date**: 2026-01-24
**Author**: Shuai Mu

**Verdict**: No issues found (submodule update + timestamp only)

---

## Commit 9b5b7357 - "Update daily check timestamps [2026-01-24]"
**Date**: 2026-01-24
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit f79fd97e - "Updated commit_reviews"
**Date**: 2026-01-24
**Author**: shenweihai1

**Verdict**: No issues found (review log update only)

---

## Commit 69f8ba0e - "Unify client mode test path with local mode [ISSUE-33b02756-2]"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (unified test path + docs plan)

---

## Commit fb6d9d92 - "Add unit tests for MakoClientService [ISSUE-1886cab7-3]"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (unit tests + CMake hook)

---

## Commit 844e6c99 - "Fix auto-commit semantics bug in Commit/Rollback handlers [ISSUE-1886cab7-2]"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (rollback fix + semantics documentation)

---

## Commit 7a6a5847 - "Fix transaction ID collision risk in MakoClientService [ISSUE-1886cab7-1, ISSUE-33b02756-1]"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (txn_id encoding implemented)

---

## Commit 8aae1d75 - "Add tasks from commit review Open Issues [2026-01-23]"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (TODO updates only)

---

## Commit d725da71 - "Add a daily task to evaluate open issues"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (TODO update only)

---

## Commit 7e3cc0a1 - "Update daily check timestamps [2026-01-23]"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (timestamp update only)

---

## Commit b5802c0a - "Update reviews and prepare to fix"
**Date**: 2026-01-23
**Author**: shenweihai1

**Verdict**: No issues found (review log update only)

---

## Commit 7051bafd - "Update daily check timestamps [2026-01-22]"
**Date**: 2026-01-22
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit 33b02756 - "Unify IDatabase/ITable interfaces and consolidate client-server docs"
**Date**: 2026-01-21
**Author**: Shuai Mu

### ISSUE-33b02756-1 [S2 - medium]
**Category**: Docs / correctness
**Evidence**: `docs/client_server_architecture.md:100-104` claims txn_id encodes client_id (upper 32 bits) + per-client counter (lower 32 bits). Current server implementation uses `client_id` directly as `txn_id` in `src/mako/client_service.cc:65-68`.
**Problem**: Documentation describes a transaction ID scheme that is not implemented. This is already an open correctness gap in the RPC service and the doc makes it appear resolved.
**Action**: Either implement the documented txn_id encoding (per-client counter) or update docs to match current behavior and explicitly call out the limitation.
**Status**: Addressed by commit 7a6a5847

### ISSUE-33b02756-2 [S2 - medium]
**Category**: Implementation / partial implementation
**Evidence**: `examples/simpleTransactionRep.cc:894-969` still routes `--client` mode through a dedicated `run_client_mode` path and returns early, rather than reusing the unified IDatabase/ITable test path.
**Problem**: The change introduces IDatabase/ITable but does not actually remove the client-only code path or run the same test suite through the unified interface, leaving the original duplication and leaving client mode with only a demo flow.
**Action**: Refactor `main()` to construct an `IDatabase*` (local or remote) and run the same test flow for both modes, or document why client mode must remain separate.
**Status**: Addressed by commit 69f8ba0e

---

## Commit ccf067d0 - "Add a subtask"
**Date**: 2026-01-21
**Author**: shenweihai1

**Verdict**: No issues found (TODO update only)

---

## Commit dffefe39 - "Update daily check timestamps [2026-01-21]"
**Date**: 2026-01-21
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit bc20cf50 - "Consolidate makoServer.cc into simpleTransactionRep.cc with --server flag"
**Date**: 2026-01-20
**Author**: Shuai Mu

**Verdict**: No issues found (feature consolidation with docs/CI updates)

---

## Commit a9612351 - "Add a task to avoid duplication"
**Date**: 2026-01-21
**Author**: shenweihai1

**Verdict**: No issues found (TODO update only)

---

## Commit 429e47f9 - "Update daily check timestamps [2026-01-20]"
**Date**: 2026-01-20
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit 5764543d - "Update daily check timestamps [2026-01-19]"
**Date**: 2026-01-19
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit 747d8596 - "Add signal handlers for graceful shutdown in FastTransport"
**Date**: 2026-01-19
**Author**: Shuai Mu

**Verdict**: No issues found (graceful shutdown fix)

---

## Commit f4d078c4 - "Update TODO.md: Mark shard2Replication race condition bug as fixed"
**Date**: 2026-01-19
**Author**: Shuai Mu

**Verdict**: No issues found (TODO update only)

---

## Commit e00d802f - "Fix race condition in FastTransport causing intermittent segfault during shutdown"
**Date**: 2026-01-19
**Author**: Shuai Mu

**Verdict**: No issues found (race fix with locking)

---

## Commit 2e9d9417 - "Refactor RPC test port allocation to prevent parallel test collisions"
**Date**: 2026-01-19
**Author**: Shuai Mu

**Verdict**: No issues found (test infrastructure refactor)

---

## Commit 3de005ca - "Fix shard1ReplicationRaft CI flakiness by lowering replay_batch threshold"
**Date**: 2026-01-19
**Author**: Shuai Mu

**Verdict**: No issues found (test threshold adjustment with justification)

---

## Commit 2fb665e0 - "Add a fix task"
**Date**: 2026-01-19
**Author**: shenweihai1

**Verdict**: No issues found (TODO update only)

---

## Commit 7c65518a - "Add reviews via judge"
**Date**: 2026-01-18
**Author**: shenweihai1

**Verdict**: No issues found (docs/judge bootstrap)

---

## Commit c0cb648b - "Update daily check timestamps [2026-01-18]"
**Date**: 2026-01-18
**Author**: Shuai Mu

**Verdict**: No issues found (timestamp update only)

---

## Commit dca5b3e8 - "don not commit hourly check; add judge role"
**Date**: 2026-01-17
**Author**: shenweihai1

**Verdict**: No issues found (docs/config only)

---

## Commit 4db65422 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 1168ab9e - "Update daily CI test timestamp [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 0a9b5250 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 45fe0c5c - "Mark shard2ReplicationErpc investigation as complete"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit 89419e73 - "Add task: fix bug in shard2ReplicationErpc"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit c3249572 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 0be2ac09 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 7628fed8 - "Investigate shard2Replication intermittent failure"
**Date**: 2026-01-17

**Verdict**: No issues found (investigation notes only)

---

## Commit 4980169d - "Add task: fix bug in shard2Replication"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit 2cc50028 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 9b1c4f47 - "Fix rusty-cpp submodule pointer"
**Date**: 2026-01-17

**Verdict**: No issues found (submodule update only)

---

## Commit a6ad38e0 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 0b00f6df - "Add test_coroutine target to fragile.toml"
**Date**: 2026-01-17

**Verdict**: No issues found (config update only)

---

## Commit ce05e4ce - "Fix test_reactor_minimal to use new PollThread API"
**Date**: 2026-01-17

**Verdict**: No issues found (test fix)

---

## Commit 7ed76ff1 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit f35306dc - "Fix asio include path in fragile.toml"
**Date**: 2026-01-17

**Verdict**: No issues found (config fix only)

---

## Commit 6a807a5b - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 7109424c - "Add test_transport_integration to fragile.toml"
**Date**: 2026-01-17

**Verdict**: No issues found (config update only)

---

## Commit 57e2e0e4 - "Add bench_future and rpcbench to fragile.toml"
**Date**: 2026-01-17

**Verdict**: No issues found (config update only)

---

## Commit 31852170 - "Add three RPC tests: chaos, partition, stress_crash"
**Date**: 2026-01-17

**Verdict**: No issues found (test additions)

---

## Commit 4b05a4b3 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 61658817 - "Add missing STL includes and update simpleTransaction paths"
**Date**: 2026-01-17

**Verdict**: No issues found (build fix)

---

## Commit 049a403e - "Enable test_sto_transaction (13 tests)"
**Date**: 2026-01-17

**Verdict**: No issues found (test enablement)

---

## Commit 97fe81d1 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit a98ab7ef - "Add masstree_perf benchmark target"
**Date**: 2026-01-17

**Verdict**: No issues found (benchmark addition)

---

## Commit edc63c7e - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 66b6a980 - "Add stress_transport_backend - 49 executables, 802 tests"
**Date**: 2026-01-17

**Verdict**: No issues found (test addition)

---

## Commit 2e08262c - "Add test_txn_timeout - 48 executables, 789 tests"
**Date**: 2026-01-17

**Verdict**: No issues found (test addition)

---

## Commit 51ec8933 - "Add rpc_combined_reliability_test"
**Date**: 2026-01-17

**Verdict**: No issues found (test addition)

---

## Commit f45575c6 - "Add 3 RPC integration tests"
**Date**: 2026-01-17

**Verdict**: No issues found (test additions)

---

## Commit 6e734a44 - "Add 4 RPC tests"
**Date**: 2026-01-17

**Verdict**: No issues found (test additions)

---

## Commit 44e6a447 - "Add 2 RPC tests"
**Date**: 2026-01-17

**Verdict**: No issues found (test additions)

---

## Commit b94e9c06 - "Add 3 RPC/integration tests"
**Date**: 2026-01-17

**Verdict**: No issues found (test additions)

---

## Commit 23d47f1f - "Add test_rpc and test_future"
**Date**: 2026-01-17

**Verdict**: No issues found (test additions)

---

## Commit 1c8dffdf - "Add fragile.toml with 33 test executables"
**Date**: 2026-01-17

**Verdict**: No issues found (config addition)

---

## Commit 4d3090e3 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit f6790562 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 260699da - "Fix checkbox for decouple client task"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO fix only)

---

## Commit a33a803d - "Clean up Test 4 in test_client_server.sh"
**Date**: 2026-01-17

**Verdict**: No issues found (test cleanup)

---

## Commit f42b275e - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 99ed9715 - "Remove legacy Coroutine/Event API, use Fiber/WaitAll/WaitAny/WaitN"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit 87fc9020 - "Add a fix task in TODO.md"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit f9ee09c5 - "Mark 'decouple client' parent task as complete"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit 157c83d1 - "Mark decoupled client rusty safe code tasks as complete"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit 43a0f263 - "Update repeated task timestamps [2026-01-17]"
**Date**: 2026-01-17

**Verdict**: No issues found (timestamp update only)

---

## Commit 465efbf4 - "Convert std::unique_ptr to rusty::Option<rusty::Box> in remote_db.hh"
**Date**: 2026-01-17
**Author**: Shuai Mu

**Verdict**: No issues found (proper RustyCpp migration with documented exceptions for WorkerSlot)

---

## Commit 2feaa2ef - "updated TODOs for client-server channel"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO update only)

---

## Commit c816b472 - "Fix build issues from RRR RPC refactoring"
**Date**: 2026-01-17

**Verdict**: No issues found (build fix)

---

## Commit 1a049ce3 - "Fix RPC partition test flakiness with ephemeral port allocation"
**Date**: 2026-01-17

**Verdict**: No issues found (test fix)

---

## Commit fc5129da - "merge TODO-weihai.md to TODO.md"
**Date**: 2026-01-17

**Verdict**: No issues found (TODO consolidation)

---

## Commit 1886cab7 - "Refactor client-server RPC from raw TCP sockets to RRR RPC framework"
**Date**: 2026-01-17
**Author**: shenweihai1

### ISSUE-1886cab7-1 [S3 - high]
**Category**: Implementation / partial implementation
**Evidence**: `src/mako/client_service.cc:65-72`
```cpp
// Generate transaction ID (same logic as ShardReceiver)
// Use client_id as the transaction ID since we're in single-client mode
// For multi-client support, we'd need a counter in receiver_
uint64_t txn_id = static_cast<uint64_t>(client_id);
```
**Problem**: `HandleBeginTxn` uses `client_id` directly as `txn_id`. In multi-client scenarios, if two clients have the same ID or call BeginTxn multiple times, transaction IDs will collide.
**Action**: Implement proper transaction ID generation using an atomic counter in `MakoClientService` or delegate to `ShardReceiver::begin_txn()`.
**Status**: Addressed by commit 7a6a5847

### ISSUE-1886cab7-2 [S3 - high]
**Category**: Implementation / not implemented
**Evidence**: `src/mako/client_service.cc:89-127`
```cpp
void MakoClientService::HandleCommit(...) {
    srpc::i64 txn_id;
    req->m >> txn_id;
    srpc::i32 status = ErrorCode::SUCCESS;  // Always returns SUCCESS!
    // No actual commit logic
}

void MakoClientService::HandleRollback(...) {
    // Same pattern - no actual rollback logic
}
```
**Problem**: `HandleCommit` and `HandleRollback` are no-ops that always return SUCCESS without performing any actual transaction management. Transaction state is never tracked or cleaned up.
**Action**: Either implement actual commit/rollback logic in `MakoClientService` or delegate to `ShardReceiver` methods that handle transaction state.
**Status**: Addressed by commit 844e6c99

### ISSUE-1886cab7-3 [S2 - medium]
**Category**: Testing / missing unit tests
**Evidence**: No unit tests found for `MakoClientService` in `test/` directory
**Problem**: New RPC service lacks dedicated unit tests. Only integration tests via CI scripts exist.
**Action**: Add unit tests for `MakoClientService` covering: BeginTxn ID generation, Put/Get/Delete operations, error handling paths, and concurrent client scenarios.
**Status**: Addressed by commit fb6d9d92

---

## Commit 4df28a33 - "Add real benchmark numbers to evaluation documentation"
**Date**: 2026-01-17

**Verdict**: No issues found (docs update)

---

## Commit bf6ba554 - "Enhance client-server CI test with full end-to-end testing"
**Date**: 2026-01-17

**Verdict**: No issues found (test enhancement)

---

## Commit 8c6f3d3c - "Add client-server integration test to CI pipeline"
**Date**: 2026-01-17

**Verdict**: No issues found (CI addition)

---

## Commit 73b29d20 - "Add worker pool for multiple concurrent clients support"
**Date**: 2026-01-17

**Verdict**: No issues found (feature addition)

---

## Commit 131c2bff - "Implement full TCP-based client-server RPC for Mako"
**Date**: 2026-01-16
**Author**: shenweihai1

### ISSUE-131c2bff-1 [S2 - medium]
**Category**: Reinventing wheels
**Evidence**: `src/mako/lib/client_tcp_server.h` - Custom TCP server implementation
**Problem**: This commit introduced a custom TCP server that was later replaced by SRPC RPC in commit 1886cab7. While this worked, it duplicated functionality that already existed in the SRPC framework.
**Action**: N/A - Already addressed in subsequent commit 1886cab7.
**Status**: Addressed by commit 1886cab7

---

## Commit 6a5f8ad0 - "Implement client-server decoupling for Mako database"
**Date**: 2026-01-16
**Author**: shenweihai1

### ISSUE-6a5f8ad0-1 [S2 - medium]
**Category**: Implementation / partial implementation (by design)
**Evidence**: Commit message states: "Full RPC integration uses stub implementations - actual RPC communication will be added in a future iteration."
**Problem**: Initial commit was intentionally stub-only. Subsequent commits (131c2bff, 1886cab7) completed the implementation.
**Action**: N/A - Addressed in subsequent commits.
**Status**: Addressed by commits 131c2bff and 1886cab7

---

## Commit 039a90f4 - "Fix RPC partition test flakiness due to port collisions"
**Date**: 2026-01-16

**Verdict**: No issues found (test fix)

---

## Commit 1d358657 - "Complete Fiber API Phase 5: Documentation update"
**Date**: 2026-01-16

**Verdict**: No issues found (docs update)

---

## Commit 22c57fee - "Rename Coroutine to Fiber internally (Fiber API Phase 4)"
**Date**: 2026-01-16

**Verdict**: No issues found (internal rename with backward compat preserved at time)

---

## Commit c60c23ab - "Add memory limit for shard2SingleProcessReplication CI test"
**Date**: 2026-01-16

**Verdict**: No issues found (CI configuration)

---

## Commit 8626d209 - "Add Future/Promise API for fiber-based async programming (Phase 3)"
**Date**: 2026-01-16

**Verdict**: No issues found (feature addition with tests)

---

## Commit 674ee42d - "Update daily check timestamps (2026-01-14)"
**Date**: 2026-01-14

**Verdict**: No issues found (timestamp update only)

---

## Commit 9a1f4d1d - "Mark shard2Replication CI timeout issue as fixed"
**Date**: 2026-01-14

**Verdict**: No issues found (TODO update only)

---

## Commit 064b0bf1 - "Mark Masstree RustyCpp Safety Migration as complete"
**Date**: 2026-01-14

**Verdict**: No issues found (TODO update only)

---

## Commit 4c307892 - "Remove Phase 5 from Masstree RustyCpp migration"
**Date**: 2026-01-14

**Verdict**: No issues found (plan adjustment)

---

## Commit a3228ef2 - "Enable borrow checking for Masstree core files (Phase 4)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp integration)

---

## Commit d8c8cbd3 - "Update TODO.md: Mark Masstree Phase 3 complete"
**Date**: 2026-01-14

**Verdict**: No issues found (TODO update only)

---

## Commit 24dd8ebd - "Add @unsafe { reason } block comments to Masstree (Phase 3.4)"
**Date**: 2026-01-14

**Verdict**: No issues found (safety annotations)

---

## Commit 0b7233ba - "Convert simple getters to @safe in masstree_struct.hh (Phase 3.1)"
**Date**: 2026-01-14

**Verdict**: No issues found (safety annotations)

---

## Commit 83d7e0d4 - "Add rusty::MutPtr to value_versioned_array (Phase 2.9)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit 28af968e - "Add rusty::Ptr/MutPtr to kvrow.hh (Phase 2.8)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit 2cbba87f - "Add rusty::MutPtr to masstree_scan.hh (Phase 2.7)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit c1c0e471 - "Add rusty::MutPtr to masstree_insert.hh (Phase 2.6)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit bf7cd914 - "Add rusty::MutPtr to tcursor and masstree_get.hh (Phase 2.5)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit f35f1c1b - "Add rusty::MutPtr to basic_table (Phase 2.4)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit 825262d2 - "Add rusty::MutPtr to kvthread.hh (Phase 2.3)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit 8677fb64 - "Add rusty::MutPtr to MasstreeContext (Phase 2.1-2.2)"
**Date**: 2026-01-14

**Verdict**: No issues found (RustyCpp migration)

---

## Commit 3224b194 - "Update daily CI test status - all tests passing"
**Date**: 2026-01-14

**Verdict**: No issues found (status update)

---

## Commit 1dbbb7f9 - "Mark Masstree RustyCpp Phase 1 (Audit & Annotate) complete"
**Date**: 2026-01-14

**Verdict**: No issues found (TODO update only)

---

## Commit 7b5d8ca7 - "Add RustyCpp safety annotations to Masstree core headers (Phase 1.3-1.10)"
**Date**: 2026-01-14

**Verdict**: No issues found (safety annotations)

---

## Commit 1f8691c8 - "Add RustyCpp safety annotations to kvthread.hh (Phase 1.2)"
**Date**: 2026-01-14

**Verdict**: No issues found (safety annotations)

---

## Commit 97fe4271 - "Add RustyCpp safety annotations to masstree_context.h/cc (Phase 1.1)"
**Date**: 2026-01-14

**Verdict**: No issues found (safety annotations)

---

## Commit 9b52a72f - "Mark Dynamic Range-Based Sharding task as complete"
**Date**: 2026-01-14

**Verdict**: No issues found (TODO update only)

---

## Commit 7454a724 - "Complete Task 9.3: TPC-C sharding integration tests"
**Date**: 2026-01-14

**Verdict**: No issues found (test addition)

---

## Commit 6e3ef719 - "Add CI verification for TPC-C sharding policy (Task 9.3.2)"
**Date**: 2026-01-14

**Verdict**: No issues found (CI addition)

---

## Commit 16cf190e - "Add sharding policy startup tests (Task 8.4)"
**Date**: 2026-01-14

**Verdict**: No issues found (test addition)

---

## Commit 40c8d00e - "Add CI failure fix task for commit 1b98df69"
**Date**: 2026-01-14

**Verdict**: No issues found (TODO update only)

---

## Commit 1d74c2e5 - "Fix test_rpc_stress_crash port overflow when running many iterations"
**Date**: 2026-01-13

**Verdict**: No issues found (test fix)

---

## Commit a41e1da3 - "Reduce PaxosWorker all_coords pre-allocation to fix memory explosion"
**Date**: 2026-01-13

**Verdict**: No issues found (performance fix)

---

## Commit 959efcea - "Update daily repeated task timestamps"
**Date**: 2026-01-13

**Verdict**: No issues found (timestamp update only)

---

## Commit 6f4a0d77 - "Add sharding policy integration tests (Task 9.2)"
**Date**: 2026-01-13

**Verdict**: No issues found (test addition)

---

## Commit 1b98df69 - "Implement sharding policy startup flow integration (Task 8)"
**Date**: 2026-01-13
**Author**: Shuai Mu

**Verdict**: No issues found (proper implementation with error handling and tests)

---

## Commit cc710f0a - "Add Fiber API with this_fiber namespace and event combinators"
**Date**: 2026-01-12
**Author**: Shuai Mu

**Verdict**: No issues found (new API with comprehensive tests - 20 unit tests added)

---

## Commit 7bf82033 - "nits"
**Date**: 2026-01-12

**Verdict**: No issues found (minor fixes)

---

## Commit 4edf7974 - "Fix ci.sh self-termination when running simpleTransaction test"
**Date**: 2026-01-12

**Verdict**: No issues found (CI fix)

---

## Commit a73b8719 - "Fix test_sharding_policy test expectations for TPC-C 1-indexed w_id"
**Date**: 2026-01-12

**Verdict**: No issues found (test fix)

---

## Commit c44004b4 - "Add masstree RustyCpp safety migration plan"
**Date**: 2026-01-12

**Verdict**: No issues found (planning doc)

---

## Commit c6707e77 - "Fix race condition in threadinfo registration"
**Date**: 2026-01-12

**Verdict**: No issues found (bug fix)
