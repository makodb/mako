# Raft Main Helper Implementation Guide

**Goal**: Replace the stubbed `raft_main_helper.{h,cc}` with a working equivalent of the Paxos helper so that Mako can run with Raft when `MAKO_USE_RAFT` is enabled.

This checklist assumes:
- `RaftWorker` already mirrors `PaxosWorker` for submit/commit callbacks.
- `MAKO_USE_RAFT` toggles inclusion of `raft_main_helper.h` (already wired).

Work through the steps in order; each builds on the previous one.  The outputs
below document what is already **implemented** versus what remains **TODO** so
you can reason about Raft ↔︎ Mako wiring at a glance.

---

## Status Snapshot

**Implemented**

- Full worker bring-up (`setup`/`setup2`) including service, commo and
  heartbeat threads.
- Callback registration for leader and follower pipelines.
- Submission path with asynchronous batching (`enqueue_to_worker`) and
  drain-aware `WaitForSubmit`.
- Utility shims (`get_hosts`, epoch getters/setters, shutdown, stats).
- NO-OP generation for watermark synchronisation and raft-aware leader
  notifications (`NotifyRaftLeaderChange`).
- Graceful shutdown fixes (submit-thread stop, epoll ignore on EBADF/ENOENT).

**Still TODO**

1. `microbench_paxos()` / `microbench_paxos_queue()` parity (currently warn).
2. Paxos-style heartbeat / failover monitors (Jetpack demo tooling).
3. Network-client helpers (`nc_*`) – presently no-ops with log warnings.
4. Rust checker build guard (update rustc or pin `indexmap`).

---

## 1. Mirror Global State

> Files: `src/deptran/raft_main_helper.cc/.h`

1. Create the same globals that `paxos_main_helper` exposes:
   - `std::vector<std::shared_ptr<RaftWorker>> raft_workers_g;`
   - `std::map<int, leader/follower callback>` containers.
   - `std::shared_ptr<ElectionState> es` (reuse the existing struct from Paxos).
2. Guard all globals inside `#ifdef MAKO_USE_RAFT` for clarity.

_Tip_: Use `paxos_main_helper.cc` as the template; we are swapping worker types, not redesigning the API.

---

## 2. Implement `setup` / `setup2`

> Files: `src/deptran/raft_main_helper.cc`

1. Parse config via `Config::CreateConfig(argc, argv)` exactly as Paxos does.
2. For each server site returned by `Config::GetConfig()->GetMyServers()`:
   - allocate a `RaftWorker`,
   - populate `site_info_`,
   - call `SetupBase()`, `SetupService()`, `SetupCommo()`, `SetupHeartbeat()`.
3. Reverse `raft_workers_g` to match Paxos ordering (leaders last).
4. Return the list of process names (used by test harnesses).
5. Implement `setup2(action, shardIndex)` to dispatch into `setup` or handle special test modes (copy the structure from Paxos, but adjust cases if Raft doesn’t support microbench features yet).

_Result_: Raft workers now come up exactly as Paxos workers did (log lines show
`RaftWorker::SetupBase completed …`).

---

## 3. Register Callbacks

> Files: `src/deptran/raft_main_helper.cc`

Implemented by iterating over `raft_workers_g` and forwarding to the matching
`RaftWorker::register_*` hook.  Callback maps (`leader_replay_cb`,
`follower_replay_cb`) are held globally so we can re-register after leader
changes.

| API | Action |
| --- | --- |
| `register_for_leader` | call `register_apply_callback` on the worker for the partition |
| `register_for_leader_par_id` | call `register_apply_callback_par_id` |
| `register_for_leader_par_id_return` | call `register_apply_callback_par_id_return` |
| Similarly for `register_for_follower*` |

Maintain the callback maps so that when leadership changes in future phases (election callbacks), we can re-register if needed.

_Hint_: The Paxos helper stores callbacks in `leader_replay_cb`/`follower_replay_cb`; mirror that structure.

---

## 4. Implement Submission Path

> Files: `src/deptran/raft_main_helper.cc`

Raft now mirrors Paxos’ fast path: `submit()` → `enqueue_to_worker()` which
backs an internal queue serviced by `SubmitLoop`.  `batch_size` values greater
than one trigger mini-batches exactly like Paxos’ `submit_pool`.  `WaitForSubmit`
waits on both the atomic counter and queue drain.

---

## 5. Utility Functions & Epoch Handling

> Files: `src/deptran/raft_main_helper.cc`

Implemented implementations:

- **`get_hosts`** loads YAML `host` mapping (same as Paxos).
- **`get_outstanding_logs`** = `worker->n_tot - raft_server->commitIndex`.
- **`shutdown_paxos`** stops submit threads, shuts down RPC / heartbeat, clears
  globals, destroys config.
- **Epoch helpers** keep `ElectionState` in sync; values propagate to each
  `RaftWorker` just like Paxos.
- **`worker_info_stats`** logs current counters for quick debugging.
- **`microbench_*`** currently warn (TODO for parity).
- **`pre_shutdown_step`** gracefully shuts down control service to match Paxos
  behaviour.
- **Leader callback** stored in `janus::leader_callback_` and triggered from the
  `RaftServer::setIsLeader()` bridge (`NotifyRaftLeaderChange`).

---

## 6. Client/Server Helpers

> Files: `src/deptran/raft_main_helper.cc`

These functions power auxiliary scripts/tests (e.g., `nc_main`):

| Function | Action |
| -------- | ------ |
| `nc_setup_server`, `nc_get_*` helpers | Either re-use Paxos implementations (copy/paste with Raft naming) or no-op if not needed immediately.|

Presently they log a warning and return `nullptr`.  Porting would require
recreating the Paxos `ServerControlServiceImpl` helpers for Raft; tracked in
TODO list.

---

## 7. Testing & Verification

Suggested test matrix (all already exercised manually):

1. Build with `-DMAKO_USE_RAFT=ON` → run `./build/deptran_server … -P localhost`
   and observe `RaftWorker::Submit` output, follower `STATUS_REPLAY_DONE`.
2. Paxos regression: build without RAFT flag to ensure helper stubs do not reg.
3. Optional: microbench once parity is added.

---

## Deferred Items

Outstanding:

1. `microbench_paxos()` / `_queue()` parity.
2. Paxos-style heartbeat/failover monitors (Jetpack demo tooling).
3. Network-client (`nc_*`) helper implementation.
4. Rust checker build fix (upgrade toolchain or pin dependency).

---

## Quick Reference

| Paxos helper function | Raft replacement |
| ----------------------| ---------------- |
| `PaxosWorker` global vector | `raft_workers_g` (`shared_ptr<RaftWorker>`) |
| `add_log_to_nc` | `RaftWorker::Submit` |
| `register_for_*` | `RaftWorker::register_*` |
| `get_outstanding_logs` | use Raft commit/executed indices |
| `setup` | same config flow, different worker |
| `set_epoch` | maintain `ElectionState` parity |

Following this guide should give you a fully operational `raft_main_helper` with the same surface API as the Paxos version, ready for higher-layer integration and testing.
