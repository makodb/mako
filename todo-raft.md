# Raft decouple — remaining Phase 8 tasks

Phase 8.0 landed at commit `cf5db3fef`. `docs/dev/raft_decouple_plan.md`
captures the design; this file is the executable TODO list for the
remaining phases, written so a fresh session can pick any phase and
execute without re-reading the full plan.

## Ground rules (apply to every phase)

- **Rusty-safe** per `CLAUDE.md`. No `std::shared_ptr`/`std::unique_ptr`/
  `std::function`/`std::thread`/`std::vector`/`std::mutex`/`std::optional`
  in new code. Use `rusty::Arc`/`rusty::Box`/`rusty::Function`/
  `rusty::thread::spawn`/`rusty::Vec`/`rusty::Mutex`/`rusty::Option`.
  Touch-as-you-go migration for adjacent std constructs. Boundary std
  types at rrr / rocksdb interfaces stay std and are annotated
  `@unsafe`.
- **Every new function has `@safe` or `@unsafe`** annotation.
- **Every commit gates on**: phase gtests (`test_raft_*`) green +
  `./build/deptran_server -f config/raft_lab_test.yml` green through
  the same test range as the baseline (cf5db3fef: tests 1-60 pass,
  mid-TEST 63 preexisting crash). If a commit regresses any test the
  baseline passes, fix before pushing.
- Required CMake options for the lab test: `cmake -DMAKO_USE_RAFT=ON
  -DRAFT_TEST=ON .` (then `cmake --build . --target deptran_server`).
- Keep `deptran_server` + `shardXReplicationRaft` green through every
  phase — production path must not regress.

## Phase 8.1 — route RaftServer outbound through TransportProxy

**Goal**: every outbound RPC on `RaftServer` goes through
`transport_->send_*` instead of `commo()->Send*`. Deletes
`RaftVoteQuorumEvent` and `SendAppendEntriesResults`. Election and
replication fiber loops stop waiting on `QuorumEvent` and instead
count sub-fiber returns.

### 8.1.a — Add a `RaftQuorum<Reply>` primitive

- [ ] Create `src/deptran/raft/quorum.hpp` (new file).
  - `template<typename Reply> class RaftQuorum` with:
    - `rusty::Arc<rrr::IntEvent> ready_;`
    - `rusty::Mutex<std::vector<std::pair<siteid_t, Reply>>> replies_;`
      (or `rusty::Vec` if available in that namespace).
    - `int n_total_`, `int n_needed_`, `rusty::sync::atomic::Atomic<int> n_received_`.
    - `void on_reply(siteid_t from, Reply r)` — appends + bumps counter
      + sets `ready_` once `n_received_ >= n_needed_`.
    - `bool wait_until_quorum(uint64_t timeout_us)` — `ready_->wait(timeout_us)`,
      returns whether quorum reached before timeout.
    - `std::vector<std::pair<siteid_t, Reply>> collect()` — drain
      under mutex.
  - Unit test `tests/raft_quorum_test.cc`: exercises add-replies,
    wait-with-timeout, collect.
  - Add `add_executable(test_raft_quorum ...)` to `CMakeLists.txt`
    alongside other `test_raft_*` targets (around line 1799-1841).
- [ ] Gate: `test_raft_quorum` passes + all existing `test_raft_*`
  targets green.
- [ ] **Commit**: `raft: phase 8.1a — RaftQuorum<Reply> primitive + unit test`.

### 8.1.b — Give `RaftServer` a `TransportProxy transport_`

- [ ] `src/deptran/raft/server.h`: add private member
  `janus::raft::TransportProxy transport_;` and an accessor
  `TransportProxy& transport()`. Include `transport.hpp` at the top.
- [ ] `src/deptran/raft/server.cc` (or wherever `RaftServer` is
  initialized — likely in `Setup()` or the constructor):
  construct `transport_ = make_rrr_transport(commo_, site_id_,
  partition_id_);` once `commo_` is non-null. `commo_` stays live —
  `RrrTransportAdapter` holds a non-owning pointer into it.
- [ ] No outbound call-site changes yet; this step just plumbs the
  member so the rest of 8.1 can reference it.
- [ ] Gate: deptran_server links, lab test passes tests 1-60.
- [ ] **Commit**: `raft: phase 8.1b — wire TransportProxy onto RaftServer`.

### 8.1.c — Migrate `BroadcastVote` (election path)

Location: `src/deptran/raft/server.cc:2013`.

Current:

```cpp
sp_quorum = ((RaftCommo *)(this->commo_))->BroadcastVote(
    par_id, lst_idx, lst_term, loc_id, term);
sp_quorum->wait(1000000);
if (sp_quorum->yes()) { ... specVoters_ = sp_quorum->GetSpecVoters(); ... }
else if (sp_quorum->no()) { ... }
```

- [ ] Replace with:
  - Build a `RaftQuorum<VoteReply>` with `n_total` = peers-1,
    `n_needed` = majority count (quorum size – 1 for self-vote).
  - For each peer in the partition (skip self), spawn
    `Fiber::create_run` that calls
    `transport_->send_vote(peer, VoteReq{lst_idx, lst_term, loc_id, term})`,
    then `quorum.on_reply(peer, reply)`.
  - `quorum.wait_until_quorum(1000000)` yields the election fiber.
  - On success, iterate `quorum.collect()`:
    - yes-count / no-count bookkeeping (replaces
      `sp_quorum->yes()/no()`)
    - populate `specVoters_` with peers that replied
      `vote_granted == true` (replaces `GetSpecVoters()`)
    - highest-term tracking across all replies (replaces
      `sp_quorum->Term()`)
- [ ] Delete the helper branches (`yes()`, `no()`, `n_voted_yes_`,
  `n_voted_no_`, `Term()`, `timeouted_`, `GetSpecVoters()`) now that
  nothing calls them on the election path.
- [ ] Gate: lab test tests 1-11 still pass (these exercise initial
  election + re-election). Watch TEST 1 + TEST 2 carefully.
- [ ] **Commit**: `raft: phase 8.1c — migrate BroadcastVote to
  per-peer send_vote via RaftQuorum`.

### 8.1.d — Migrate `SendAppendEntries` (hot replication path)

Location: `src/deptran/raft/server.cc:3164` (main HeartbeatLoop),
line 1647 (`SendAppendEntries2`, speculative path).

Current: `commo()->SendAppendEntries(..., shared_ptr<cmd>, ...)`
returns `shared_ptr<SendAppendEntriesResults>`. Callers read `res->done`,
`res->ok`, `res->followerTerm`, `res->followerLastLogIndex`,
`res->followerAckType` after `res->event->wait()`.

- [ ] Convert to per-peer `transport_->send_append_entries(peer, req)`
  returning `AppendEntriesReply` directly. Build an
  `AppendEntriesReq` from the same fields.
- [ ] In HeartbeatLoop: each peer's replication sub-fiber
  (`Fiber::create_run`) calls `send_append_entries` synchronously,
  consumes the reply, updates `next_index_[peer]` / `match_index_[peer]`
  under `mtx_`.
- [ ] For the speculative path at 1647 (`SendAppendEntries2`): if the
  semantics are identical to the standard path (just a different
  result shape), consolidate. Otherwise add a
  `transport_->send_append_entries_spec` variant — but first confirm
  the spec path is actually distinguishable on the wire.
- [ ] Delete `SendAppendEntriesResults` from `commo.h` +
  `commo.cc` + every include site. Delete `SendAppendEntries2` /
  `SendAppendEntries` member definitions from RaftCommo (the
  `*Cb` variants stay as the rrr-side callback entry).
- [ ] Gate: lab test tests 1-60 all pass. Watch TEST 3 (Basic
  agreement), TEST 7 (Concurrent starts), TEST 11 (Figure 8),
  TEST 60 (HeartbeatLoop triggers InstallSnapshot).
- [ ] **Commit**: `raft: phase 8.1d — migrate SendAppendEntries /
  SendAppendEntries2 to per-peer transport_->send_append_entries`.

### 8.1.e — Migrate the remaining outbound sites

- [ ] Line 1530 `SendInstallSnapshot` → `transport_->send_install_snapshot`.
- [ ] Line 2589 `SendAppendEntriesDurable` → `transport_->send_append_entries_durable`
  (fire-and-forget).
- [ ] `server.h:408` `SendVoteDurable` → `transport_->send_vote_durable`
  (fire-and-forget).
- [ ] `TimeoutNow` call sites → `transport_->send_timeout_now`.
- [ ] Line 1194 `UpdatePartitionView` — this is gossip; either drop
  it from the facade or leave the direct `commo()->UpdatePartitionView`
  call (annotate `@unsafe` and note it's out of scope for 8.x).
- [ ] Line 1408 `commo()->rpc_par_proxies_[par_id]` — this reaches
  into rrr internals. Either wrap with a helper on `RaftCommo` that
  RaftServer consumes, or leave as a documented `@unsafe` boundary.
- [ ] Delete `RaftVoteQuorumEvent` from `commo.h` + `commo.cc` now
  that no one calls `BroadcastVote`.
- [ ] Gate: full lab test + `shard1ReplicationRaft` throughput
  (≥80k ops/sec per docs/dev/raft_decouple_plan.md completion criteria).
- [ ] **Commit**: `raft: phase 8.1e — retire remaining commo() outbound
  call sites; delete SendAppendEntriesResults + RaftVoteQuorumEvent`.

### 8.1 risks

- **mtx_ re-entry**: reply handlers currently fire on rrr's callback
  thread; after 8.1 they fire on the sub-fiber's thread. Every reply
  handler that modifies `next_index_` / `match_index_` / `durableAcks_`
  / `memoryAcks_` must take `mtx_` explicitly. Use `std::lock_guard<
  std::recursive_mutex>`.
- **Speculative voting state**: `specVoters_` / `durableVoters_` /
  `specCommitIndex_` / `securedLogIndex_` have subtle invariants — see
  `VerifySpeculativeInvariants`. Run `testSpeculativeLeaderElection`
  and `testSpecCommitIndexAdvances` specifically.
- **Timeout semantics**: current `sp_quorum->wait(1000000)` is 1s
  timeout. `RaftQuorum::wait_until_quorum(1000000)` must match.
- **Self-vote counting**: election path assumes self votes yes
  implicitly. Current `BroadcastVote` excludes self from the N peers
  but `n_needed = N/2` counts self implicitly. The `RaftQuorum` MUST
  use `n_needed = quorum_size - 1` (majority minus self).

## Phase 8.2 — `RaftServerDispatcher`

**Goal**: a thin adapter that makes `RaftServer` satisfy
`DispatcherFacade`. Every `handle_*` method allocates a local `Reply`,
calls the existing `RaftServer::OnX(...)` with output-pointer args,
and returns the filled `Reply`.

- [ ] Create `src/deptran/raft/raft_server_dispatcher.hpp`:
  - `class RaftServerDispatcher { RaftServer* svr_; public: 8 handle_*
    methods }`.
  - Each `handle_*`:
    - If `svr_ == nullptr` or `svr_->IsDisconnected()`: return a
      default `Reply` with the same failure-default values the current
      `RaftServiceImpl` uses when `svr == nullptr`.
    - Otherwise: allocate the output struct fields as locals, call
      `svr_->OnX(req.f1, req.f2, ..., &locals.f1, &locals.f2, ...)`,
      pack locals into `Reply`, return it.
  - Factory:
    `inline DispatcherProxy make_raft_server_dispatcher(RaftServer*)`.
- [ ] Unit test `tests/raft_server_dispatcher_test.cc`: construct a
  minimal RaftServer (or mock), wrap in dispatcher, exercise each
  handle_*.
- [ ] Gate: `test_raft_server_dispatcher` + all existing
  `test_raft_*` green.
- [ ] **Commit**: `raft: phase 8.2 — RaftServerDispatcher + factory`.

### 8.2 risks

- Building a RaftServer outside the full deptran stack may require
  stubs. Consider reusing `RaftTestConfig`-minus-everything or
  keeping the test narrow (just check the adapter dispatches
  correctly given a pre-built svr).

## Phase 8.3 — `RaftServiceImpl` forwards to `DispatcherProxy`

**Goal**: `RaftServiceImpl`'s fiber-RPC overrides stop calling
`svr->OnX` directly and instead call
`dispatcher_->handle_x(req)`.

- [ ] `src/deptran/raft/service.h`: add member
  `rusty::Option<DispatcherProxy> dispatcher_;` (Option because the
  dispatcher is set after the server is registered).
- [ ] `src/deptran/raft/service.cc`:
  - In the constructor or `UpdateServer()`: call
    `dispatcher_ = rusty::Some(make_raft_server_dispatcher(svr))`
    when svr is set.
  - Each override method (`Vote`, `VoteDurable`, `AppendEntries`,
    `EmptyAppendEntries`, `AppendEntriesDurable`, `TimeoutNow`,
    `NotifyRestart`, `InstallSnapshot`, `AddServer`, `RemoveServer`):
    replace the body's `svr->OnX(...)` calls with
    `return Result<Resp, i32>::Ok(dispatcher_->handle_x(req))`.
  - The null/disconnected guard stays — if `dispatcher_.is_none()`,
    return `Ok(default_reply)` with the same shape current code uses.
- [ ] Delete the `#include "server.h"` header if no longer needed
  (the dispatcher adapter references RaftServer internally).
- [ ] Gate: lab test tests 1-60 all pass. Pay attention to
  `NotifyRestart` — it has side effects (calls `commo->ReconnectToSite`
  + `svr->OnPeerRestart`).
- [ ] **Commit**: `raft: phase 8.3 — RaftServiceImpl forwards to
  DispatcherProxy`.

### 8.3 risks

- `NotifyRestart` is the odd one — it's currently a service-level
  method that reconnects the rrr client. In `RaftServerDispatcher`
  the dispatcher has no `commo_` to call `ReconnectToSite` on. Either
  keep `NotifyRestart` as a service-level concern (no dispatcher) or
  thread the commo reference through.

## Phase 8.4 — storage proxies (optional)

**Goal**: `LogStorageProxy` / `SnapshotManagerProxy` facades replace
the virtual `LogStorage` / `SnapshotManager` interfaces at
`RaftServer`'s boundary.

- [ ] Create `src/deptran/raft/log_storage_facade.hpp` mirroring every
  method of `LogStorage` (get / put / get_range / put_batch /
  remove / remove_range / first_index / last_index / get_term / size /
  empty / get_metadata / set_metadata / sync / close / is_open / clear).
- [ ] Same for `src/deptran/raft/snapshot_manager_facade.hpp`
  (BeginSnapshot / TakeSnapshot / BeginLoad / LoadLatestSnapshot /
  GetLatestSnapshot / ListSnapshots / HasSnapshotAtOrAfter /
  PruneSnapshots / DeleteAllSnapshots / GetStoragePath).
- [ ] Switch `RaftServer::log_storage_` to `LogStorageProxy` and
  `RaftServer::snapshot_manager_` to `SnapshotManagerProxy`. Existing
  virtual impls (`RocksDBLogStorage`, `InMemoryLogStorage`,
  `FileSnapshotManager`, `MemorySnapshotManager`) wrap in proxies via
  factory functions.
- [ ] Gate: lab test tests 1-60 + all snapshot tests pass.
- [ ] **Commit**: `raft: phase 8.4 — proxy LogStorage/SnapshotManager`.
- [ ] Skip if time is short; the existing virtual interfaces work
  fine.

## Phase 8.5 — `TestCluster` with real `RaftServer`s

**Goal**: replace `DummyDispatcher` inside `RaftNode` with a real
`RaftServer` wrapped via `RaftServerDispatcher`. Each node uses
`ChannelTransportAdapter` pointing at a shared `ChannelSwitchboard`.

- [ ] `src/deptran/raft/raft_node.hpp`:
  - Replace `rusty::Arc<DummyDispatcher> dispatcher_impl_` with
    `rusty::Box<RaftServer> server_`.
  - Constructor: build a `RaftServer` with `transport_ =
    make_channel_transport(sw_, self, par)`, `log_storage_` =
    `InMemoryLogStorage`, `snapshot_manager_` = `MemorySnapshotManager`.
    Wrap with `make_raft_server_dispatcher(server_.get())` and store
    the resulting `DispatcherProxy`.
  - Wire the server into its Raft timers/fibers:
    `server_->StartElectionTimer()`, `server_->HeartbeatLoop()`,
    `server_->StartApplyThread()` / `StartApplyFiber()` — exactly as
    `deptran_server` does today but without a `deptran_server` binary.
- [ ] Delete `DummyDispatcher` once nothing references it.
- [ ] `TestCluster::with_in_memory_transport(n)`: keep the existing
  wiring but ensure each node's RaftServer is in a state ready to
  accept the first `HeartbeatLoop` tick.
- [ ] New gtest cases in `tests/raft_test_cluster_test.cc`:
  - Election converges: construct 3-node cluster, step until
    exactly one `node(i).is_leader()` is true.
  - `DoAgreement` equivalent: the leader appends a log entry, every
    node observes the entry's `commit_index()` advance.
  - `disconnect(follower)` prevents the follower from catching up
    until `reset_faults`.
- [ ] Gate: the above gtests + `raft_lab_standalone` still runs its
  4 legacy cases.
- [ ] **Commit**: `raft: phase 8.5 — TestCluster runs real RaftServers`.

### 8.5 risks

- RaftServer's startup path expects a full deptran environment
  (Config, Frame, rep_frame_, tx_sched_ etc.). Need to either:
  - (a) Teach RaftServer to accept a minimal "test mode" init that
    skips tx_sched_ wiring, OR
  - (b) Build just enough of the surrounding scaffolding in
    TestCluster.
  Probably (a) — add a `RaftServer(/*test_mode*/)` constructor that
  skips `tx_sched_` setup.
- Fiber scheduling: RaftServer's timers use `Fiber::create_run` +
  `Fiber::sleep` — depends on `rrr::Reactor` running. In a test
  binary that doesn't use `deptran_server`, a `rrr::PollThread` must
  still be created to drive the reactor. `rusty::thread::spawn` a
  PollThread per node.

## Phase 8.6 — port `RaftTestConfig` to `TestCluster`

**Goal**: `RaftTestConfig` can operate on a `TestCluster` instead of
on the 5-server deptran topology.

- [ ] `src/deptran/raft/testconf.h`: add a new constructor
  `RaftTestConfig(TestCluster& cluster)` alongside the existing
  `RaftTestConfig(std::vector<Frame*>)`.
- [ ] `src/deptran/raft/testconf.cc`: when constructed from a
  TestCluster, route every operation:
  - `Kill(i)` → destroy `nodes_[i]`'s RaftServer, switchboard drops
    its outbound by default.
  - `Restart(i)` → rebuild the server in place, re-register its
    dispatcher.
  - `Disconnect(i)` → `sw_.drop_direction(i, *)` +
    `sw_.drop_direction(*, i)`.
  - `Reconnect(i)` → per-direction undrop (small switchboard API
    addition: `undrop_direction(from, to)` or rebuild faults minus
    this one).
  - `Partition(a, b)` → `sw_.partition({a, b})`.
  - `DoAgreement(cmd, n, wait)` → call the leader's log-append path
    (see `RaftServer::Submit` or equivalent), poll `commit_index()`
    across nodes.
  - `OneLeader()` → scan nodes for `is_leader()`.
- [ ] Keep the existing rrr-based `RaftTestConfig(std::vector<Frame*>)`
  constructor intact so `deptran_server -f raft_lab_test.yml` keeps
  working.
- [ ] Switchboard API additions (likely in
  `src/deptran/raft/channel_transport.hpp`):
  - `undrop_direction(siteid_t from, siteid_t to)`: remove from
    `ChannelFaults::dropped`.
- [ ] Gate: subset of `RaftLabTest` runs against the new
  constructor (see 8.7 for the full driver). Minimally: `testInitialElection`,
  `testReElection`, `testBasicAgree`, `testFailAgree`.
- [ ] **Commit**: `raft: phase 8.6 — port RaftTestConfig to TestCluster`.

## Phase 8.7 — `raft_lab_standalone` runs the full `RaftLabTest::Run()`

**Goal**: replace the 4 Phase-7 skeleton cases in
`src/deptran/raft/raft_lab_standalone.cc` with a full RaftLabTest
driver. Completion of the decouple plan.

- [ ] Edit `src/deptran/raft/raft_lab_standalone.cc`:
  - Build a 5-node `TestCluster`.
  - Construct `RaftTestConfig(*cluster)` (the Phase 8.6 constructor).
  - Construct `RaftLabTest testconfig` and call `test.Run()` +
    `test.Cleanup()`.
- [ ] Exit with non-zero on any failed test case.
- [ ] Gate:
  - `./build/raft_lab_standalone` runs tests 1-60 (at minimum) end-to-end.
  - `ss -lntp | grep raft_lab_standalone` → empty (no sockets bound).
  - No `rocksdb` files on disk (MemoryLogStorage + MemorySnapshotManager).
- [ ] **Commit**: `raft: phase 8.7 — raft_lab_standalone runs full
  RaftLabTest via TestCluster`.

### 8.7 risks

- Some `RaftLabTest` cases poke at `RaftServer` / `RaftServiceImpl`
  internals directly (see mentions of `svr_` access, static
  registry). Those test bodies may need edits to go through
  `TestCluster::node(i)` instead.
- Speculative tests (63, 65, 70) exercise state that requires full
  leader election + log replication + fsync path. If
  `MAKO_RAFT_PERSISTENCE` is unset, those might not fire correctly
  in-process. Set the env var when launching the binary or configure
  `MemoryLogStorage` to notify durable-acks synchronously.

## Phase 8.8 (deferred) — `RaftClock` abstraction

Not required for the core decouple goal. Add `RaftClock` + `ManualClock`
if deterministic testing (advance-time-by-N-ms) becomes valuable.

---

## Preexisting raft bugs surfaced during Phase 8.0 verification

These are independent of the decouple plan — own commits, own
verification. Listed here so they don't get lost.

### P1 — TEST 60 `CompactLog` no-op without `log_storage_`

- Location: `src/deptran/raft/server.cc:497` (`CompactLog`).
- Current behavior: if `log_storage_ == nullptr`, `CompactLog`
  returns 0 without trimming `raft_logs_` or advancing
  `min_active_slot_`. TEST 60 asserts `leader_min_active > 1` after
  a snapshot triggers compaction and fails with
  `Leader min_active_slot_ should be > 1 after compaction, got 1`.
- Fix: make `CompactLog` trim the in-memory `raft_logs_` +
  advance `min_active_slot_` even when `log_storage_` is absent,
  AND make `HeartbeatLoop`/`AppendEntries` fall back to
  `InstallSnapshot` when `prevLogIndex < min_active_slot_` (the
  current leader fabricates an empty `RaftData` via
  `GetRaftInstance(id)` when a slot is missing — that returns
  `term=0` which breaks the consistency check).
- See commit `31dc57a37` message for what was tried and why it
  cascaded into replication breakage.

### P2 — TEST 63 mid-test crash at `server.cc:1420`

- Location: `src/deptran/raft/server.cc:1420` (UnsecuredFailure
  step-down path, inside `testSpeculativeLeaderElection` or a
  related speculative test).
- Observed during Phase 8.0 verification: lab test completes
  TEST 1-60 + enters TEST 63 (UnsecuredFailure step-down rolls
  back all entries), then aborts at the `verify(...)` on
  `server.cc:1420`.
- Fix: read the assertion context at that line, reproduce with
  the minimum speculative test, trace the invariant. Likely
  related to specVoters_ / durableVoters_ bookkeeping when
  leader steps down mid-election.
- Independent of the decouple plan. Should be fixed before anyone
  relies on speculative voting correctness.

---

## Tracking

- [x] Phase 8.0 — fiber-sync facades (cf5db3fef)
- [ ] Phase 8.1a — RaftQuorum primitive
- [ ] Phase 8.1b — TransportProxy member on RaftServer
- [ ] Phase 8.1c — migrate BroadcastVote
- [ ] Phase 8.1d — migrate SendAppendEntries / SendAppendEntries2
- [ ] Phase 8.1e — retire remaining commo() outbound sites
- [ ] Phase 8.2 — RaftServerDispatcher
- [ ] Phase 8.3 — RaftServiceImpl → DispatcherProxy
- [ ] Phase 8.4 — storage proxies (optional)
- [ ] Phase 8.5 — TestCluster with real RaftServer
- [ ] Phase 8.6 — port RaftTestConfig to TestCluster
- [ ] Phase 8.7 — raft_lab_standalone full driver
- [ ] Phase 8.8 — RaftClock (deferred)
