# Workstream N L10 Unblock Plan

**Status**: 2026-05-03 — written after L10d-prep + L10d-cleanup landed.

## Why we're stuck

After L10c-cmds, production polymorphic command fields are
`janus::Command` (`SerializableEnvelope<MakoCommands>`) on the wire,
but Command's *internal* storage is still
`std::shared_ptr<Marshallable>` (`Command::inner_`).  The
~64 production files / ~232 callsites that hold a polymorphic
command in memory still pass it around as `shared_ptr<Marshallable>`
(via `Service::Submit`, `Coordinator::cmd_`, `LogEntry::command`,
`RaftData::log_`, etc.), and `Command::inner_marshallable()` exists
specifically to keep those callsites compiling.

To drop `Marshallable` and `MarshallDeputy` (L10f) we have to:

1. Change `Command`'s storage off `shared_ptr<Marshallable>` (e.g.
   to `SerializableProxy`).  Until production stops needing
   `cmd.inner_marshallable()`, we can't do this without
   reintroducing the dual-storage problem L10d-prep just retired.
2. Migrate all production polymorphic command fields + signatures
   from `shared_ptr<Marshallable>` to `Command`.  After this,
   `Command::inner_marshallable()` is dead and Command's storage
   can switch.

Step 2 is where the real work is — 64 files, ~232 sites — and it
must complete before step 1 is safe.

## Migration target

The end shape is: **`janus::Command` is the polymorphic
in-memory carrier in production**, just as it's already the on-wire
carrier.  Internal APIs that hold a polymorphic command hold a
`Command`; APIs that take or return a polymorphic command take or
return a `Command` (by value or const-ref — Command is cheap to
copy since it's a small object holding a `shared_ptr`).

Type recovery uses Command's typed accessors (`cmd.unpack<T>()`
returns `T*`, `cmd.unpack_shared<T>()` returns `shared_ptr<T>`)
instead of `marshallable_cast<T>(shared_ptr<Marshallable>)`.

## Migration clusters (by ownership locality)

The 64 files cluster into ~7 owners.  Each cluster can migrate
independently because their `shared_ptr<Marshallable>` traffic flows
within the cluster, with `Command` already crossing cluster
boundaries (the .rpc fields).  Within each cluster, every site that
held / passed `shared_ptr<Marshallable>` flips to `Command`, and the
boundary points use `cmd.inner_marshallable()` only as a last-resort
escape hatch for un-migrated peers.

| Cluster              | Files | Notes |
| -------------------- | ----- | ----- |
| **C1 — Raft**        | log_storage.hpp, server.h/cc, raft_worker.cc, replicated_db.h, messages.hpp, transport.hpp, fpga_raft/**, memory_log_storage.hpp, rocksdb_log_storage.hpp | `LogEntry::command`, `RaftData::log_`, persistent log, replicated DB apply path |
| **C2 — Paxos**       | paxos/server.cc/h, paxos/coordinator.cc/h, paxos/commo.cc/h, paxos/exec.cc/h, paxos_worker.cc/h | `MultiPaxosServer` instance.cmd_, `CoordinatorMultiPaxos::cmd_` |
| **C3 — Mencius**     | mencius/* (server, coordinator, commo, exec) | mirror of C2 |
| **C4 — Copilot**     | copilot/* | mirror of C2 |
| **C5 — Janus**       | janus/scheduler.cc/h, janus/coordinator.cc/h, janus/commo.cc/h, classic/scheduler.cc/h, classic/tpc_command.cc/h | RccTx::cmd_, JanusCoord::cmd_ |
| **C6 — Mongodb**     | mongodb/* | mirror of C2 |
| **C7 — Service**     | service.cc, communicator.h/cc, coordinator.h, command.h, RW_command.cc, procedure.h/cc, tx.h | `ClassicServiceImpl::Dispatch`, `Tx::cmd_`, `Procedure::commands_` |

C1 is the most contained (LogEntry persistence is a self-contained
data structure); start there.  C7 is the largest and intertwines
with every cluster — it goes last.

## Sub-leaves (each ~100-500 LOC, ~1 commit)

### L10f-prep1 — C1: `LogEntry::command` + raft persistence
- `LogEntry::command`: `shared_ptr<Marshallable>` → `Command`
- `LogEntry::save/load`: pass `Command` directly to
  `BinaryWriteArchive` / `BinaryReadArchive` (Command has its own
  `save`/`load`).
- `RaftServer::PersistLogEntryToLogStorage`: convert
  `RaftData::log_` (still `shared_ptr<Marshallable>`) into a
  `Command` at the boundary.
- `rocksdb_log_storage.hpp` / `memory_log_storage.hpp`: no change
  (they take/return `LogEntry` by value; the field type change
  flows through automatically).
- `rpc_log_storage_test.cc`: `entry.command == nullptr` →
  `!entry.command.has_value()`; `make_shared<TestCommand>(...)`
  argument flows through the existing
  `Command(shared_ptr<Marshallable>)` ctor.
- `rpc_rocksdb_log_storage_test.cc`: same.
- **Wire-format**: unchanged (LogEntry's save still emits
  `[v32 kind][payload]` for the command field; the only difference
  is whether the carrier in memory is `Command` or
  `MarshallDeputy(shared_ptr<Marshallable>)`, both of which produce
  identical bytes).
- Estimated: ~80 LOC across 6 files.

### L10f-prep2 — C1: `RaftData::log_` + raft server/worker
- `RaftData::log_`: `shared_ptr<Marshallable>` → `Command`.
- `FpgaRaftServer::accepted_cmd_/committed_cmd_/log_`: same.
- All `RaftServer::*Append*` paths.
- `raft/replicated_db.h::ApplyEntry(slot, cmd)` signature.
- `raft_worker.cc` dispatch.
- Estimated: ~200 LOC across 8 files.

### L10f-prep3 — C2/C3/C4: classic Paxos / Mencius / Copilot servers
- Each cluster's `instance.cmd_`, `Coordinator::cmd_*`, commo paths.
- 3 separate commits (one per cluster) to keep diffs reviewable.
- Estimated: ~300 LOC each.

### L10f-prep4 — C5: Janus scheduler/coord
- `RccTx::cmd_`, `JanusCoord::cmd_`, scheduler dispatch.
- Most subtle — Janus does the most polymorphic dispatch.
- Estimated: ~400 LOC.

### L10f-prep5 — C6: Mongodb
- Smaller cluster; mirror of C2.
- Estimated: ~150 LOC.

### L10f-prep6 — C7: Service / communicator / Tx / procedure
- Top-level `ClassicServiceImpl::Dispatch` + `Tx::cmd_` + procedure
  command vector.
- The big one — touches every protocol's entry into the scheduler.
- Estimated: ~500 LOC.

### L10f-prep7 — Drop `Command::inner_marshallable()` accessor
- Once nothing in production calls it, remove the migration shim.
- Replace `Command::inner_` storage with
  `std::shared_ptr<SerializableProxy>` (or equivalent).
- Update `Command::pack/unpack` to operate on the proxy directly.
- Estimated: ~100 LOC.

### L10f — Drop `Marshallable` + `MarshallDeputy`
- Once Command no longer holds Marshallable, the base virtuals +
  `MarshallDeputy` envelope have no users.  Delete.
- Estimated: -300 LOC (net negative).

### L10g — Drop legacy `Marshal& operator<<` for polymorphic types
- Auto-falls-out: when `MarshallDeputy` goes away, the legacy
  operators on it go with it.
- Estimated: included in L10f net.

## Per-leaf checklist

Each leaf:

1. Migrates the field/signature changes for one cluster only.
2. Preserves wire-format byte-for-byte (each commit verifies via
   the existing round-trip tests).
3. Runs the full RPC test suite green (`./docker_build.sh ci
   rrrTests`).
4. Lands as one commit; merges to mako-dev; pushes.

## Notes

- The existing `Command(shared_ptr<Marshallable>)` ctor +
  `Command::inner_marshallable()` accessor are the migration
  bridges.  They let any cluster migrate at its own pace — a
  partially-migrated cluster's boundary calls just use
  `cmd.inner_marshallable()` to feed the un-migrated side.
- After every leaf, the main worktree should build and pass tests.
  No partial commits.
- Test fixtures (`TestCommand`, `TestMarshallable`,
  `CanaryMarshallable`) migrate at the same time as the cluster
  whose APIs they test.

## Blocked extensions (post-L10)

After L10f lands, additional simplifications become possible:

- Drop `marshallable_cast<T>` overloads on `Marshallable&` /
  `Marshallable*` / `shared_ptr<Marshallable>` in `marshal.hpp`.
- Drop `as_marshallable(SerializableProxy)` and the
  `SerializableMarshallableAdapter` class itself once Command no
  longer routes through them.
- Drop `MarshallDeputy::reg_initializer` in favor of the
  L10c-cmds-prep2 runtime registry. The experimental
  `TypeList::create_at` compile-time factory was later retired after
  envelope loading standardized on that registry.
