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

- Keep each source under `src/deptran/raft/` and keep production compilation in
  the existing C++ target.
- Replace an existing declaration or body in place with one Rust DSL block and
  its adjacent generated C++.
- Run `bash scripts/raft_dsl.sh --check`; no regeneration post-pass is allowed.
- Complete value/POD and plain-control-flow work before stateful, inherited,
  threaded, RPC, storage, or snapshot boundaries.

The first tranche owns only `VoteReq`, `VoteReply`, `VoteDurableReq`, and
`VoteDurableReply` in `messages.hpp`.  They are leaf aggregates with primitive
fields, so the conversion changes source ownership without changing any Raft
class or runtime call edge. Because Rust cannot express C++ in-class member
defaults, their existing call sites were audited and the move to explicit
value initialization was isolated in the preceding preparation commit.
Tests pin aggregate status, exact member types, field offsets, size, alignment,
zero initialization, positional construction, and behavior.

### Stage 2: exact Rust extraction

- Add a Raft Cargo crate and a schema-1 extraction manifest.
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

1. Primitive request/reply POD families.
2. Remaining scalar value types and pure methods, with initialization and ABI
   audited per type.
3. Command/string-bearing messages after their canonical Rust type map exists.
4. Serialization, snapshot, and log values with byte-for-byte format fixtures.
5. Stateful Raft classes in place where the DSL can preserve their shape.
6. RPC, filesystem/RocksDB, scheduler, thread, and test-harness boundaries last;
   retain narrow bridges where a faithful DSL spelling does not yet exist.

Historical PR #79 is useful as a catalog of probes and candidate conversions,
but its changes must be re-authored against current main and the current pinned
emitter.  In particular, generated-C++ post-processing and helper/core reshapes
are not inherited into this migration.

## Current status (2026-08-22)

- Stage 1 started with the four vote PODs in `messages.hpp`.
- The preceding preparation commit isolates this tranche's required reshape:
  it removes in-class zero defaults after proving all repository call sites use
  brace or value initialization.
- Native extraction will retain public fields, `repr(C)`, and standard POD
  derives; rustc-only attributes keep that metadata projection-neutral in C++.
- The dedicated Raft drift check regenerates read-only-safe temporary mirrors
  linked to the exact Cargo/path-dependency context and proves the checked-in
  generated block is byte-identical to current emitter output.
- Its exact carrier/block inventory ratchet rejects deleted, moved, duplicate,
  or unreviewed ownership blocks as the tranche list grows.
- The earlier standalone canonical-Rust quorum slice remains an experimental
  Phase-2 build-plumbing reference only; it is not part of this lineage.
