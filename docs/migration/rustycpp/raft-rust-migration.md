# Raft: C++ to Canonical Rust

## Objective

Raft follows the same source-provenance path that succeeded for SRPC:

```text
existing C++
  -> inline Rust DSL with adjacent generated C++
  -> exact mechanical Rust extraction
  -> whole-provider dual-compile canary
  -> canonical .rs source with build-tree-generated C++
```

The native Rust and generated-C++ builds must consume the same Rust source.
An independently maintained Rust port is not a migration stage.

## Invariants

1. Phase 1 preserves existing files, declarations, field order, call graph,
   vtables, wire/storage formats, and production C++ provider ownership.
2. Rust DSL is authoritative inside each `#if RUSTYCPP_RUST` block.  The
   adjacent `RUSTYCPP:GEN` region is committed, unedited transpiler output.
3. A source reshape, when genuinely required, lands separately from the DSL
   ownership change and has its own behavior and performance evidence.
4. C++, C, or assembly remains only as a narrow bridge for operations the
   current DSL cannot faithfully express, such as RPC, filesystem/RocksDB,
   threading, raw-pointer surgery, or platform context switching.
5. Promotion swaps one complete provider.  It never adds a parallel leaf
   implementation or links both providers into production.
6. Each tranche proves generated-output drift, C++ ABI/layout, focused runtime
   behavior, and the Raft integration lane before expanding scope.

## Stage gates

### Stage 0: baseline and ownership map

- Enumerate Raft declarations and intentional C++ boundaries on current main.
- Pin public layouts, symbols, wire/storage formats, correctness tests, and a
  representative performance baseline.
- Treat the safety annotations as an input to the map, not as DSL coverage.

### Stage 1: inline Rust DSL

- Keep each declaration in its existing standard-Raft, main-helper, or
  FPGA-Raft carrier and keep production compilation in the existing C++ target.
- Replace an existing declaration or body in place with one Rust DSL block and
  its adjacent generated C++.
- Run `bash scripts/raft_dsl.sh --check`; it verifies the pinned emitter,
  rewrites a temporary mirror byte-for-byte, and compiles every carrier's
  extracted payload with `rustc -D warnings`. No regeneration post-pass is
  allowed.
- Complete value/POD and plain-control-flow work before stateful, inherited,
  threaded, RPC, storage, or snapshot boundaries.

The current Stage-1 pass owns 40 blocks in 22 carriers. A fixed
nonblank/non-comment census extracts 897 Rust lines, about 10.77% of the 8,330
meaningful lines in the standard-Raft baseline used for this migration. This
is 59.0% of PR #79's approximately 1,520-line/18.25% DSL surface, while
excluding its state-core, container, interface, and behavior changes.

The owned surface now includes fixed-representation values, all 15
scalar-only RPC request/reply records, pure quorum/storage/snapshot/worker
decisions, append-rejection backoff arithmetic, coordinator guards, log-entry
ordering and wire-boolean conversion, test-harness index math, and the full
CRC32 update loop. Tests and adjacent assertions pin aggregate status, exact
member types, field offsets, size, alignment, plain/value/positional
initialization, enum discriminants, snapshot bytes, signed and wrapping edge
cases, and legacy handling of unknown values.

### Stage 2: exact Rust extraction

- Add a Raft Cargo crate and a schema-1 extraction manifest. Stage 1 already
  proves that each carrier's raw extracted payload is accepted by `rustc`;
  Stage 2 adds module order, crate dependencies, and Rust-native tests.
- Generate `.rs` files by concatenating the owned inline blocks in manifest
  order; do not translate or hand-edit a second implementation.
- Fail on source/hash drift, missing or duplicate blocks, changed ordering,
  orphan output, unexpected files, or emitter-pin mismatch.
- Make rustc tests, clippy, extractor contract tests, and the inline DSL drift
  check one source gate.

Production still uses the adjacent generated C++ during this stage.

### Stage 3: whole-provider canary

- Define stable Raft module/provider identities before swapping output.
- Generate C++ from the extracted crate into the build tree.
- Retain an independently compiled inline reference provider.
- Compare generated, inline-reference, and selected-production providers for
  exact symbols, layouts, imports, hand slots, wire behavior, and runtime tests.
- Keep the generated partial crate root compile-only; it must not become a
  second production provider.

### Stage 4: canonical Rust promotion

- Move the exact extracted bytes to layout-mirroring canonical `.rs` files.
- Replace the extraction manifest entry with a canonical module manifest entry.
- Delete one complete inline carrier only after its generated child is the sole
  production provider and all dual-provider gates pass.
- Repeat until native Rust can link canonical SRPC plus canonical Raft while the
  C++ Mako build consumes C++ generated from those same sources.

## Tranche ordering

1. Scalar value types and pure methods, with initialization and ABI audited per
   type.
2. Primitive request/reply POD families after default-member-initializer
   emission support exists.
3. Command/string-bearing messages after their canonical Rust type map exists.
4. Serialization, snapshot, and log values with byte-for-byte format fixtures.
5. Stateful Raft classes in place where the DSL can preserve their shape.
6. RPC, filesystem/RocksDB, scheduler, thread, and test-harness boundaries last;
   retain narrow bridges where a faithful DSL spelling does not yet exist.

Historical PR #79 is useful as a catalog of probes and candidate conversions,
but its changes must be re-authored against current main and the current pinned
emitter.  In particular, generated-C++ post-processing and helper/core reshapes
are not inherited into this migration.

The catalogue was applied by category:

- Scalar records, fixed-representation enums, copied-scalar decisions, stream
  arithmetic, and the CRC32 loop were re-authored where the current emitter can
  reproduce the incumbent C++ contract exactly.
- Mixed records were split at their boundary: scalar message families and leaf
  methods moved, while command/string/array-bearing containers stayed C++.
- State cores, traits/vtables, providers, ownership/container substitutions,
  callback-readiness assertions, and generated-output patching were rejected.
  Those are preparation or provider migrations, not source-ownership changes.

## Current status (2026-08-23)

- The behavior- and structure-preserving PR-catalogue candidate set in
  Raft-owned code is exhausted at 40 Rust DSL blocks in 22 carriers (897
  extracted Rust lines, approximately 10.77% of the fixed standard-Raft
  baseline). Production still compiles only the adjacent generated C++.
- All 15 scalar-only RPC request/reply records are Rust-owned. The emitter's
  exact inert `#[cfg_attr(any(), cpp_value_init)]` field marker generates the
  incumbent C++ `{}` default member initializer while retaining aggregate,
  layout, trivial-copy, plain-default-zero, value-init, and positional-init
  contracts. Command- and string-bearing records remain C++-owned.
- Value ownership includes `RecoveryMode`, `NotifyRestartStatus`,
  `StepDownReason`, `CommitStatus`, `AckType`, both snapshot mode enums,
  `ReplicatedDBOp`, `RaftGroupMode`, and the six-word disk diagnostic
  `RaftData`.
- Pure copied-scalar decisions now cover the server, worker, coordinator,
  communicator, quorum, channel transport, storage, recovery, snapshot,
  service, frame, and test-harness leaves. Locks, atomics, RPC callbacks,
  container traversal, persistence, filesystem/RocksDB calls, logging, and
  branch sequencing remain at their original C++ call sites.
- The CRC32 hot loop is Rust DSL, using raw pointers so each byte is observed
  before the accumulator mutation even when input aliases the CRC object's
  representation. Its generated optimized kernel is instruction-for-
  instruction identical to the incumbent. The production lookup table contains
  legacy non-IEEE entries; tests pin its existing checksum bytes rather than
  silently changing persisted snapshots. Correcting that table requires a
  separately versioned format change. The argument case-fold helper keeps its
  narrow C `tolower` import and legacy unsigned-byte/process-locale domain.
- The source gate pins emitter commit
  `77c3ad5a9ab69190ee361986caf579afa2eae570`, ratchets the exact block
  inventory, rejects source or generated-output drift, performs a clean
  temporary rewrite, and compiles every extracted carrier with real `rustc`.
  CMake production/test targets and CI depend on this gate.
- The earlier standalone canonical-Rust quorum slice remains an experimental
  Stage-2 build-plumbing reference only; it is not part of this lineage.

## Validation evidence (2026-08-23)

- `scripts/raft_dsl.sh --check` accepts the frozen 40-block/22-carrier
  inventory with zero failures. The RRR provenance gates report five
  drift-free files, 37 manifest modules, and 79 passing extraction tests.
- The pinned emitter's focused `cpp_value_init` suite passes five
  unit/golden cases plus a Clang compile-and-run ABI smoke test; `cargo check`,
  clippy, and the generated-diff check are clean. Its full suite has 2,364
  passing and one ignored test; the three remaining failures are pre-existing,
  unrelated marker-free direct-CodeGen fixtures.
- A fresh Clang 22/libc++ Release build with `MAKO_USE_RAFT=ON` and
  `RAFT_TEST=ON` compiles the final Raft, replicated-DB, txlog, disk-reader,
  and nine focused test targets. Focused CTest is 9/9 green, including all six
  standalone three-node, disconnect, partition, durable-write, and unknown-op
  lab scenarios.
- `rrr_goal0_dual_compile` compiles 38 modules with zero hand slots, links the
  combined generated/production importer, passes every listed layout/runtime
  contract, and matches 1,961 provider-owned strong ABI symbols.
- Strict ASan+UBSan is green for all eight snapshot/CRC tests and for the
  message and quorum suites. The unsuppressed broad lane is 4/9: its other
  five executables stop only at the pre-existing `operator new` to
  `rusty::Box`/`free` allocator mismatch; the involved call sites and runtime
  headers are byte-identical to the baseline. Suppressing only that known
  mismatch, while retaining leak detection and UBSan halt-on-error, makes the
  broad lane 9/9 green. The migration does not hide or repair that independent
  runtime defect.
- Six retained matched baseline/candidate five-process `testNoOps` pairs show
  98.57 versus 98.29 mean logs/s (candidate -0.28%); the mean paired delta is
  -0.21% with 5.24% standard deviation. Preferred localhost completes 12/12;
  the incomplete cluster results on both builds have the same pre-existing
  peer fast-exit shape at 9/10 logs. This detects no regression, but the
  harness is capped at ten 10ms-spaced logs and is therefore coarse. A longer
  one-shard workload cannot be produced from the matched artifacts without a
  broad module rebuild, so no stronger end-to-end claim is made. Independently,
  Clang 22 `-O3 -march=native` emits identical 44-byte/14-instruction kernels
  for the incumbent and generated CRC loop.

## Conservative Stage-1 stopping and promotion boundary

The declarations left in C++ require a representation, API, ownership, or
call-graph decision rather than another mechanical source-ownership change:

- The new initializer marker is deliberately limited to named ordinary-struct
  fields whose type is exactly a built-in bool or integer. It rejects active,
  malformed, qualified, duplicate, wrong-placement, alias, float, character,
  pointer, reference, array, and container uses. Extending it to non-scalars
  would require a separate ABI contract and emitter change.
- Generated free functions are not currently authenticated as C++ `noexcept`.
  `RecoveryConfig` and `SnapshotConfig` therefore retain their literal default
  member initializers: routing those literals through helpers changes both
  implicit default constructors from nothrow to potentially throwing. Tests
  pin the incumbent construction trait.
- The duplicated guarded `slotid_t`/`ballot_t` fallback aliases are overridden
  in production by macros from `constants.h` (notably signed `int64_t` for
  `ballot_t`), while their fallback declarations spell unsigned `uint64_t`.
  Inline extraction cannot encode that preprocessor-selected type policy, so
  those aliases remain C++-owned until the macro/type boundary is unified.
- `janus::KeyValue` is defined token-identically in standard Raft, FPGA Raft,
  and Copilot. Converting only one leaf changes its emitted type spelling and
  violates C++'s one-definition rule even when the platform aliases have the
  same underlying type. It remains C++-owned until the duplicates are moved to
  one shared definition or migrated together in a separate preparation change.
- `AppendEntriesReq` depends on `janus::Command`. Snapshot and membership
  records depend on `std::string`; snapshot data is arbitrary bytes, not UTF-8.
  Inline mode does not yet apply a Rust-to-exact-C++ type map, so pseudo types
  may render C++ but fail the mandatory extracted-Rust gate, while canonical
  Rust `String` renders a different, move-only C++ type.
- Snapshot-size guards deliberately use wrapping addition at the incumbent
  `size_t` or `uint64_t` width. This retains malformed-input behavior, including
  cross-width C++ promotions; overflow hardening must be a separate correctness
  change before native Rust executes this parser.
- The test-cluster index helper deliberately retains signed `int32_t` addition
  and remainder. Its existing callers supply a nonzero server count and values
  whose sum is representable. Native-Rust promotion must pin or redefine those
  preconditions rather than accidentally introducing debug-only panics.
- Stage 1 owns the `ReplicatedDBOp` declaration because its emitted C++ still
  accepts every raw `u8`, including unnamed values, with the same bytes and
  switch behavior. It is not ready for native-Rust promotion: a Rust enum
  cannot validly hold those unnamed values, and `KVOperation` is transiently
  value-created with zero. Stage 2 must validate decoding or use a transparent
  byte newtype before Rust executes this path.
- The group-mode argument helpers currently accept arbitrary
  `std::string_view` bytes. Their Stage-1 C++ output preserves that domain,
  including locale-sensitive folding of bytes above ASCII, but canonical Rust
  must use an OS-string or byte-slice boundary rather than construct `&str`
  from unvalidated `argv` or environment bytes.
- `SnapshotHeader`, full metadata/config records, the full `LogEntry`, the
  standard `RaftData`, `FpgaRaftData`, envelopes, pending contexts, and worker
  records still have arrays, strings, commands, callbacks, pointers, atomics,
  guarded type aliases, or mixed methods whose exact shape the inline emitter
  cannot preserve. Their scalar leaf methods are already owned where doing so
  does not move the containing object or alter construction traits.
- The remaining FPGA-Raft records and its nested status/phase enums contain
  commands, raw pointers, anonymous or unscoped nested enums, methods, or
  thread state. Crossing them requires a provider-level tranche.
- Existing class methods cannot generally be projected independently: an
  `impl` for a hand-written C++ class emits an orphan placeholder. The pass
  therefore takes free scalar helpers and one audited raw-pointer loop, while
  whole inherited/vtable, RPC, storage, RocksDB/filesystem, scheduler, and
  thread providers wait for provider-level migration support.
- The remaining timeout/environment helpers call C++-owned dependencies or use
  static environment caches. Foreign declarations could make isolated wrappers
  compile, but would expand the bridge and call-graph surface while migrating
  only shells; moving the dependency closure changes parsing, random-number,
  initialization, or call boundaries.
- `replication_helper.{h,cc}` is the shared Paxos/Raft dispatcher, not a
  Raft-owned provider. Its `ReplicationType` enum and duplicate
  `equals_ignore_case` leaf are mechanically convertible, but claiming either
  would expand this tranche into shared-provider ownership. They remain
  unclaimed; migrating them belongs to a separately inventoried
  shared-dispatcher tranche.
- `CoordinatorRaft::Phase`, `CoordinatorFpgaRaft::Phase`, and the standard/FPGA
  server status enums are embedded in existing classes. The phase enums also
  rely on unscoped-enum integer behavior; a generated scoped Rust enum is not a
  mechanical replacement.

Crossing any of these boundaries starts a new, explicitly tested preparation
tranche. It should not be folded into this structure-preserving pass.
