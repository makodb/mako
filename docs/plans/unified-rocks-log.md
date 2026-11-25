# Unified RocksDB transaction log

Unify transaction logging after the OCC + 2PC cutover at `c3938df78`. The runtime
must submit the same commands through one interface in both deployment modes.
Raft orders replicated commands; a single-disk driver orders local commands.
Both persist the same versioned log entries and recovery metadata in RocksDB.
The applied row, transaction and receipt format remains shared as well.

Implementation branch: `unified-rocks-log` in `/var/tmp/mako-shard-rust`.

## Starting point

`Journal` already names the upper Begin/Prepare/Decide/Complete operations.
`ReplicatedLog` and `LocalLog` nevertheless duplicate publication paths and encode
commands differently. The local path has no separately replayable command log.
`raft-host::Disk` writes a custom segmented WAL, then folds it into a RocksDB
base whose WAL is disabled in synchronous mode. Production configuration with
neither persistence nor replication silently selects an in-memory journal.

## Design and invariants

- One runtime log adapter owns the application command codec, journal transitions,
  receipts, state-machine application and recovery. Its ordering driver is local
  or Raft. The upper protocol continues to depend on its backend-independent
  journal contract.
- A shared RocksDB log stores identity/format policy, ordered index/term/payload
  entries, committed prefix, durable sequence and snapshot boundary. Both drivers
  use the same keys and codecs. Raft additionally uses the term/vote fields.
  RocksDB WAL and synchronous atomic batches supply the persistence barrier.
- Append is not application. Raft acknowledges application only after consensus
  commitment; local mode acknowledges only after a durable local decision and
  application. Recovery replays committed entries above the applied prefix.
  Uncommitted Raft suffixes never become local commits by reopening a directory.
- Physical mode, membership/group, schema and durability identities remain
  fenced. Sharing a format does not permit switching authority by editing a
  configuration file. Existing formats must be imported safely or rejected
  explicitly without modifying their data.
- Prepare remains invisible, decisions remain immutable, and completion publishes
  rows and receipts atomically. A failure after persistence is uncertain and
  requires replay; it cannot become an invented Abort.
- Production nodes require Raft or a local RocksDB data directory. Volatile mode
  is explicit and limited to development/test configuration. Existing migration
  restrictions remain until durable migration has its own recovery design.

## Stages and checks

| Stage | Work | Completion evidence |
| --- | --- | --- |
| 1 | Shared durable log codec, storage and identity rules | Independent crash/refusal tests plus real RocksDB reopen/format checks |
| 2 | Move Raft host persistence to that store | Raft persistence, restart, snapshot, failover and policy regressions |
| 3 | Local ordered driver using the same store and application callback | Append/application interruption, committed replay and parity checks |
| 4 | One runtime adapter and explicit production configuration | Identical command bytes across modes; node and client/process regressions |
| 5 | Qualification and documentation | Fast isolated upper gate, focused native/release and sanitizer checks, book and contracts updated |

Keep the upper gate independent of RocksDB, Masstree, SRPC and Raft. Put storage
format tests in the log component and actual runtime/recovery tests in their
separate native gate. Run all compilation and tests in the build container.
Performance remains weekly/manual.

## Progress

The shared log and both ordering drivers are implemented. The runtime now uses
one `log::Log` adapter, one command codec and one application callback. Native
configuration requires persistence or Raft unless volatile development mode is
explicit. The example launcher defaults to local RocksDB.

All five stages are complete. Six shared-log cases passed, including actual
local/Raft entry parity, crash recovery, corruption and refusal checks. The full
store and host suites passed, as did all 54 native runtime cases in debug and
release. Eight focused cases passed under each of ASan and TSan. The isolated
upper gate passed in 156.026 seconds on 32 CPUs, with all positive proofs and 20
semantic negative controls. Strict Clippy, doctests and source/dependency audits
passed. The default local launcher reopened its data and continued its counter
from 40 to 80. The book and contracts describe the new implementation.

[Qualification evidence](../testing/unified-rocks-log-2026-11-25/README.md) records
the commands, scope, source hashes, tests and known boundaries.

This is a versioned disk-format cutover. New local and Raft nodes share
`<data>/log/base` and `<data>/rows`. Old segmented Raft directories and applied-only
local stores are refused intact; automatic conversion is outside this change.
Reinitializing or deleting old data is never part of startup recovery.
