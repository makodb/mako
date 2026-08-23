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

The conservative Stage-1 pass owns 12 blocks in eight carriers. It covers nine
fixed-representation enums, the disk-diagnostic `RaftData` record, the pure
preferred-leader predicate, and two startup argument helpers. The scalar RPC
records remain handwritten because the pinned emitter cannot preserve their
C++ default member
initializers: emitting bare fields would change `T value;` from zeroed to
indeterminate and change default-construction traits. Tests and adjacent
assertions pin aggregate status, exact member types, field offsets,
size, alignment, both C++ initialization forms, enum discriminants, snapshot
bytes, the legacy handling of unknown snapshot bytes, and raw
replicated-operation values.

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

## Current status (2026-08-23)

- The behavior- and structure-preserving Stage-1 candidate set in Raft-owned
  code on the current tree is exhausted: 13 declarations or bodies are owned by
  12 Rust DSL blocks in eight existing Raft carriers. Production still compiles
  only their adjacent generated C++.
- No RPC message record is claimed as Rust-owned yet. Even the scalar records
  have a C++ plain-default-initialization contract the pinned emitter cannot
  reproduce; focused tests now pin that stopping boundary.
- Value ownership includes `RecoveryMode`, `NotifyRestartStatus`,
  `StepDownReason`, `CommitStatus`, `AckType`, both snapshot mode enums,
  `ReplicatedDBOp`, `RaftGroupMode`, and the six-word disk diagnostic
  `RaftData`.
- The three owned function bodies are `IsPreferredLeaderConfigured`,
  `equals_ignore_case`, and `is_raft_group_mode_arg`. They retain their
  anonymous-namespace C++ linkage and byte behavior. The case-fold helper uses
  one narrow `tolower` C import so extracted Rust and emitted C++ both preserve
  the legacy unsigned-byte, process-locale contract.
- The source gate pins emitter commit
  `a1f8fef85e8d43bb00f85f8ef32e5ecc69408642`, ratchets the exact block
  inventory, rejects source or generated-output drift, performs a clean
  temporary rewrite, and compiles every extracted carrier with real `rustc`.
  CMake production/test targets and CI depend on this gate.
- The earlier standalone canonical-Rust quorum slice remains an experimental
  Stage-2 build-plumbing reference only; it is not part of this lineage.

## Conservative Stage-1 stopping and promotion boundary

The declarations left in C++ require emitter support or a representation, API,
ownership, or call-graph decision rather than a mechanical source-ownership
change:

- The scalar RPC records use default member initializers. Removing them leaves
  layout and value initialization unchanged, but changes plain default
  initialization (`T value;`) from zeroed to indeterminate and changes
  `is_trivially_default_constructible`. The pinned emitter has no authenticated
  field-initializer marker, while constructor and macro substitutes change
  aggregate semantics or obscure the source contract. They remain handwritten.
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
- `SnapshotHeader`, metadata/config records, `LogEntry`, the standard
  `RaftData`, `FpgaRaftData`, envelopes, pending contexts, and worker records
  have default member initializers, arrays, strings, commands, callbacks,
  pointers, atomics, guarded type aliases, or methods whose exact shape the
  pinned inline emitter cannot preserve.
- The remaining FPGA-Raft records and its nested status/phase enums contain
  commands, raw pointers, anonymous or unscoped nested enums, methods, or
  thread state. Crossing them requires a provider-level tranche.
- Existing class methods cannot be projected independently: an `impl` for a
  hand-written C++ class emits an orphan placeholder, and generated header
  definitions are not C++ `inline`. Whole inherited/vtable, RPC, storage,
  RocksDB/filesystem, scheduler, thread, and test-harness providers therefore
  wait for provider-level migration support.
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
