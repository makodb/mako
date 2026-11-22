# Native Rust STO / C++ protocol alignment

## Decision and scope — 2026-10-24

The approved implementation aligns **native Rust STO with the live Mako
protocol**, not with a generated C++ STO backend. Rust stays compiled by rustc;
the existing C++ backend remains a separate implementation. The former
shared-canonical-source/generation proposal and C++-safety-first sequencing are
superseded. Broad [C++ rusty-safety conversion](cpp-sto-rusty-safety.md) is
**deferred**, not a prerequisite or a completed part of this work. Narrow rusty
ownership migrations in touched bridges do not establish broad checker coverage.

The protocol is implemented in the real core, storage, FFI, coordinator,
participant, SRPC and replication/replay paths. The local/native gates and all
11 listed distributed topology scenarios have passed functional qualification.
See [the current architecture contract](../architecture/rust-sto.md) and the
source entry points below for authoritative APIs, lifetimes and qualification limits.

Keep Rust's `Transaction`, `TObject`, `TransItem`, `TransProxy`, `TxnCell` and
`TxnCounter` vocabulary, typed ownership, worker affinity, and useful batch/direct
specializations. There is no requirement to adopt C++ object layout, unsafe
lifetimes, virtual hierarchy, or a generated-provider source graph.

## Implemented protocol milestones

| Boundary | Implemented contract and source |
| --- | --- |
| Datatype locking and publication | [`TObject::lock`](../../crates/sto-core/src/object.rs) and `LockContext::try_lock` / `try_unique_lock` acquire synchronously in access order, not a globally sorted deferred plan. Core owns partial guards and reverse release. Local `TxnCell`/`TxnCounter` may publish/unlock in install; shared physical locks defer publication. Tables retain their locks. |
| Retained participants | [`Participant`](../../crates/sto-core/src/participant.rs) uses the same transaction frame and phase driver: incremental `lock_writes`, sealing `validate`, irrevocable `install`, terminal `finish`. Participants retain locks until finish, including reference datatypes that publish early locally. They are worker-affine, `!Send` and `!Sync`: retained synchronous network lifecycles do not permit async suspension or worker migration. |
| Separate clock domains | [`DistributedClock`](../../crates/mako-clock/src/distributed.rs) uses decimal `base * 10 + term`, separately from private native OCC encoding and cache HLC/sequence. Proposals are reserved above selected read bases; the coordinator selects the maximum proposal, not a value strictly above all proposals. FFI tables use record-local OCC versions. |
| Native table history | [`sto-masstree`](../../crates/sto-masstree/src/lib.rs) stores history in existing SharedValue/ArcSwap slots, with inline bootstrap until metadata is needed. Selected payload and metadata agree under term/stable-base/failed-shard visibility. Scan dependencies include tombstones. The monotonic retention floor retains the new head and previous prefix at or above the floor, cutting the first below-floor version and its tail. This is payload-history pruning, not completion of native directory reclamation. |
| Coordinator and remote participants | [`sto-tpcc-ffi`](../../crates/sto-tpcc-ffi/src/lib.rs), the [native wrapper](../../src/mako/storage/rust_sto_tpcc_wrapper.cc), and existing SRPC participant bridges implement real remote operations. Attempt/incarnation/sequence/term admission precedes effects; duplicate replies are cached and late attempts fenced. Every touched participant, including read-only participants, receives terminal cleanup. Missing INSTALL replies mean unknown/fail-stop, never success or workload retry. |
| Final writes and replay | The native final-write ledger owns copied caller bytes. Replication uses existing legacy framing and canonical table IDs; replay routes to the selected backend without relogging. Replay applies a version iff its term is higher OR its base is higher, matching legacy behavior. Record/footer clock floors precede watermark effects. Historical INSERT/REMOVE meaning follows the selected version; physical cardinality follows the actual/staged head. |
| Runtime and topology | Colocated native wrappers retain the process-global `SiloRuntime` owner required by the native ABI and initialize its pinned arenas before loaders. Replicated foreign facades route to their owning RPC participant. Full colocated replication requires identical shard-ordered full YAML lists at every peer and global `shard * workers_per_shard + local_worker` partitions. Standalone legacy local mapping remains; full-topology Raft uses per-partition groups. |
| Transport progress | Paxos retains each serialized request/frame through bounded cooperative channel admission; it never resends an accepted frame. Peer locks are released before yielding. Poll workers dispatch finite ready generations so fibers and stackless self-wakes cannot starve TCP writes. Learners apply a contiguous slot prefix. |
| Shutdown | Fresh-nonce coordinator acknowledgements precede client/helper teardown. Replication remains alive until every configured member, including learners, acknowledges its delivered END after replay drain. Persistence shuts down before RocksDB background-pool destruction. Fixed propagation sleeps and stale records cannot satisfy these fences. |

Local commit performs datatype lock/predicate work, required clock work, final
read certification, preinstall acceptance, irreversible install, remaining
publication/unlock, then cleanup. Local cache acceptance and hook ordering are
preserved. Definite abort, committed cleanup failure, and unknown publication
remain distinct. Native unknown status is 7 (wire 5); committed cleanup failure
is native 8. Uncertain destruction quarantines rather than retrying cleanup.
Same-OS-thread detach preserves worker identity/counters; a new incarnation in
the same term is rejected.

Ordinary writes to a history-bearing table without decision metadata fail
closed. FFI no longer exports the unsupported `trusted_scan_value_generation`
field; general `TableConfig` ordered-generation support remains for other
callers, as do FFI bounded atomic values. Remote scalar median uses the existing
value-only scan wire, not arbitrary distributed range scans. Replay receipts
prove successful backend apply-or-version-skip, not payload digests or a
post-loading recovery proof. Core opacity and native directory reclamation
remain deferred.

## Observed functional qualification — 2026-10-24

All execution used isolated Docker on `zoo-002`. Receipts below are relative to
`/home/users/shuai/mako-sto-protocol-alignment-20261024/`.

- **Actual topology matrix: 11/11 PASS.** Separately selected C++ and Rust
  backends commit remote NewOrder/Payment transactions across two shards.
  Both cover unreplicated execution and separate/colocated Paxos and Raft;
  Rust additionally covers unreplicated colocation. Replicated cases require
  nonempty backend replay receipts and clean terminal drain
  (`commands/1792883868957276087-actual-cross-language-protocols`).
- **Core/clock/reference datatypes: PASS**, including retained participants,
  partial-lock cleanup, publication/alias boundaries, failure outcomes and
  trybuild (`commands/1792879329755017906-core-clock-datatypes`).
- **Full native integration: PASS**, including the five FFI protocol cases for
  copied final writes, historical mutation/cardinality, remote INSERT→PUT,
  source replay metadata and log uncertainty
  (`commands/1792883845466116657-native-rust-integration`).
- **Native compatibility: PASS** — metadata 14, runtime 37, and real
  participant/admission/learner-prefix 18 cases
  (`commands/1792883818377900684-cpp-metadata`,
  `commands/1792883818410632049-cpp-runtime`,
  `commands/1792883818974300719-cpp-participant-rpc`).
- **Poll-worker starvation: failing-before/passing-after.** Both production-poller
  loopback cases fail on the saved old reactor and pass on finite ready turns
  (`commands/1792883123596713686-poll-io-fairness-before`,
  `commands/1792883817589218486-poll-io-fairness`). Existing manual event-chain
  and nested-fiber cases also pass
  (`commands/1792883822741523431-manual-reactor-compatibility`).
- **Local consumer coverage: PASS.** Four fixed-read/scan/slow-exit smokes
  (`commands/1792883845601147549-local-wrapper-smokes`); native storage
  local-version cases 13, Rust engine cases 51, and cache cases 232
  (`commands/1792884007865128347-native-cache-consumers`). Only the existing
  LSan negative control is ignored in this unsanitized cache run.
- **Missing INSTALL reply: PASS for both engines.** A private source overlay
  drops a reply only after actual successful INSTALL. Coordinators fail-stop;
  Rust reports UNKNOWN, and neither engine reports workload success or retries
  the uncertain decision (`commands/1792884364140759364-dropped-install-rust`,
  `commands/1792884375708817173-dropped-install-cpp`). No fault hook is shipped.
- **Shutdown freshness: PASS.** Both engines complete two uneven-duration runs
  in the same synchronization namespace. Rust also rejects a missing peer
  despite old acknowledgements (`commands/1792883869198015395-stale-drain-rust`,
  `commands/1792883962836986348-stale-drain-cpp`).
- **Strict four-crate rustdoc: PASS**
  (`commands/1792883845910105562-strict-rustdoc`).

The private fault overlay was removed and normal binaries rebuilt
(`commands/1792884414839892118-native-restore-build`). A fresh actual Rust
two-shard workload then committed successfully
(`commands/1792884543157560669-actual-rust-distributed`).

These final runs include the repairs found by real execution: rejected TCP
admission, reactor I/O starvation, learner-prefix ordering, stale/early teardown,
late RocksDB destruction, colocated runtime/arena initialization, and reservation
of replication control ports. Earlier failed diagnostics remain failed records.

The qualification is functional and bounded. Replay receipts prove nonempty
backend apply-or-version-skip, not payload-digest equality or post-loading-only
recovery. Mixed-backend clusters, live failover, distributed crash/restart,
new sanitizers, performance parity and a backend-default switch are not claimed.
Native logging uses valid singleton transaction frames while C++ batches them;
protocol alignment does not imply identical batching costs.

**Performance qualification remains user-stopped.** The
[shape migration record](rust-sto-cpp-shape.md) retains failed/stopped campaigns
and historical measurements. No new timings, throughput parity, non-inferiority
approval, backend-default change, or restarted campaign follow from this work.
The old generated-backend or three-way performance plan is not an active task.

## Historical rusty-cpp investigation — 2026-10-16

This section preserves the executed probe, not the current implementation or
compiler requirement. All compilation/execution ran in an isolated Docker
container on `zoo-002`, using read-only source/cache mounts, no network and a
fresh output directory. No production code, host clocks, services or benchmark
campaign changed.

- At the time, the source gate, gitlink, submodule HEAD and executable build-info
  agreed on `20707e98a371fee772729ae0a6b516e2d360ddd7`, with
  `git_dirty=false`. This is a dated identity, not a current pin instruction.
- Raw `--crate crates/sto-core/Cargo.toml --package sto-core --frozen` emitted
  **21 files**, zero emitter errors and **97 attention markers in 15 files**:
  82 `todo`, 2 `todo_tagged`, 13 `skipped_rust_only`. These were not 97 proved
  independent bugs; skipped tests and derives needed classification.
- Three `TypedTerminalReadBatch` inherent/trait collisions preserved renamed
  bodies without routed dispatch. Other observed gaps included unexpanded
  ID/error macros, unresolved `DirectTokenLock::try_acquire_token`, a skipped
  constrained direct-body adapter, unhandled match arms and no `arc_swap`
  provider. These names describe that snapshot, not current exported APIs.
- The raw root **failed C++ compilation** at invalid default module name
  `sto-core`; the source walk included test-only and benchmark files. This
  showed that invocation was insufficient, not that a prepared graph could
  never compile.
- A positive control compiled the actual `sto-core/src/legacy_tid.rs` with
  rustc and, via a named rusty-cpp module, Clang 22 and a linked C++ consumer.
  Both passed ten ownership/version/opacity-helper checks and printed
  `legacy-helper checks=10 next=50176`.

Evidence remains at
`/home/users/shuai/mako-sto-rustycpp-investigation-20261016/`: `summary.json`,
commands, stdout/stderr, generated files and source identities. That helper
smoke proves neither whole-core readiness nor concurrency, opacity, allocation
or performance parity. SRPC's existing dual-compilation workflow was precedent
for the abandoned proposal, not a mandate to generate STO. No production
C++ STO cutover was established by this investigation.
