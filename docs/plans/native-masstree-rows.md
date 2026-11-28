# Native Masstree rows and thin Rust adapter

Status: complete. Implementation and qualification results are recorded in the
[test-port and evidence record](../testing/native-masstree-rows/README.md).

## Intended boundary

Masstree owns native row allocations containing committed binary values, version
history, lock words and publication state. Its leaf values point to those rows.
C++ performs storage-specific lookup, snapshot selection, validation, publication
and reclamation. Rust owns safe handles and forwards STO callbacks. Distributed
STO remains the sole transaction manager, with one read/write set and one OCC+2PC
driver. RocksDB remains the durable row/checkpoint and log store.

The existing production direct-token path already avoids an ID-to-registry lookup.
This change removes Rust ownership and alternative row layouts, rather than
claiming a measured lookup improvement.

## Sequence

- [x] Specify native row/snapshot/write-intent lifetimes and failure behavior.
- [x] Implement native byte rows, retained snapshots, lock/version checks,
      preallocated publication, scans and replay. Test the native contract alone.
- [x] Add safe Rust handles and a small STO adapter. Eliminate the Rust committed
      row registry, alternate layouts and redundant committed-value allocation.
- [x] Switch the application wrapper and retained participant path. Preserve the
      public supported storage behavior and remove obsolete optimization paths.
- [x] Port and run contract tests, native safety tests and transaction integration
      tests. Keep transaction-manager tests independent of real storage.
- [x] Update proof adapter mappings, component boundaries, book and inventory.

## Contracts to preserve

- Binary keys and values, empty values, tombstones, read-your-writes, insert/remove
  results and repeated mutation of a key.
- Point-read conflicts, absence validation, ordered scans in both directions,
  bounded/limited scans and phantom detection.
- Write locks before global validation, retained participant locks across RPC
  waits, immutable prepared intents, infallible publication after durable Commit,
  and explicit fencing for uncertain installed outcomes.
- Distributed version metadata, visibility selection, history retention,
  idempotent replay, newer-version protection, checkpoints including tombstones,
  and migration range coordination.
- Native handle validation, thread affinity, table lifetime, no escaping short
  RCU guards, allocation failure before irreversible publication, and quarantine
  on invariant failures.
- Existing negative controls, independent upper-layer proofs and storage
  conformance. Retired layout-specific tests must be identified explicitly;
  deleting a target is not a correctness port.

## Completion evidence

The production Masstree path must use native-owned values without a Rust row
registry or alternative storage path. Record executed gates and their results
here. No throughput improvement is claimed without a separate benchmark.

## Current implementation

The native row owner is `src/mako/storage/mtree_rows.cc`. Its private ABI is
wrapped by `crates/masstree/src/native_rows.rs`. Native leaf words contain row
addresses. The native ownership list is for reclamation, not lookup. Immutable
value handles retain the same native buffer across reads and publication.

`sto-masstree` forwards the `TObject` contract and keeps observations in STO.
`sto-masstree-api` now directly exposes its safe Rust API. The redundant Rust C
ABI, resolved-record caches, registry layouts, fixed-u64 alternate store and
associated comparison/PGO recipes are retired. The runtime obtains frozen redo
from STO's authoritative items; it no longer encodes and decodes a legacy frame
or maintains a second mutation ledger. Node workloads remain the performance
entry point, separate from normal component checks.

Initial validation passed the existing concurrent serializability oracle in
ordered and record-local modes, native buffer lifetime/history tests, storage
conformance and the safe application API tests. A sealing regression exposed an
incorrect error mapping that poisoned STO; this was fixed and the regression
passes. The final isolated component gate passed 53 cases and 8 doctests,
the runtime regression passed 117 cases, and each native sanitizer passed 53
cases. The upper-layer gate passed in 161.905 seconds on 32 CPUs, including
all 29 negative controls. Component accounting includes the C++ rows and safe
FFI module in the same transaction component. The book and boundary documents
now describe the implemented ownership model and its qualification limits.


## Follow-up: original STO techniques and the 80% target

Status: complete for the revised 80% target and the measured workload matrix.
The user revised acceptance on November 27 to at least 80% of the original C++
string wrapper, single-thread first and then multithread. The original 100%
parity goal is not claimed. The [reproducible performance record](../testing/sto-80/README.md)
contains the pinned reference, exact commands, raw trials and failed checkpoints.

- [x] Build the original `0d26e2e0b` string MassTrans reference with unchanged
      transaction/index/value code and separately built, pinned Rusty support.
      Keep it benchmark-only and abort if excluded RPC/log effects are reached.
- [x] Adapt packed native values, bounded scratch reuse, direct Masstree cursors,
      leaf-version absence/scan observations and own-insertion/split adjustment.
      Missing reads allocate no row; scans no longer copy intermediate keys.
- [x] Preserve native snapshot ownership, preallocation before publication and
      all final read/write validation after acquiring every write lock. Retained
      participants still use owning guards across 2PC waits. The local plan uses
      core's existing table-bound lock tickets and one authoritative item set.
- [x] Port original mutation composition and scan/split behavior. Add lifetime,
      copy-on-write, lock-ticket, RCU error/unwind and stale-leaf regressions.
      Keep the existing serializability oracles and facade assertions intact.
- [x] Qualify single-thread performance. The first five-pair pass reached 81.8%
      reads and 80.2% mixed updates before multithread tuning began.
- [x] Qualify 1, 2, 4, 8, 16 and 32 threads using five predetermined pairs per
      workload. Final ratios are 91.5% to 95.8% for reads and 84.7% to 89.9% for mixed
      updates. Every process checks committed-write conservation.
- [x] Pass 70 native cases and 8 doctests, 135 runtime cases, all 70 cases under
      each native sanitizer, and the upper proof gate in 164.893 seconds. Archive
      the evidence and refresh the book and component census.

The generic byte-row implementation embeds up to 16 bytes beside its version,
lock, pinning latch and publication flags in one cache line. An unshared buffer
may change in place using original STO's atomic byte-copy/version-validation
pattern. Shared snapshots force a replacement buffer and can retain the original
row allocation after table destruction. Large values and timestamped histories
remain immutable packed allocations. Scratch pools hold at most 64 unreferenced
small-value blocks and 64 publication-node blocks per thread and drain at exit.

The safe Rust owner validates each token's table incarnation and passes that
owner through private row ABI version 4. There is no per-row table backpointer.
Local small-value updates avoid the second line containing history links and
allocation ownership. A regression switches between local and timestamped
updates, prunes history and checks retained snapshots after table destruction.

`get_into` reuses the caller's buffer. `Transaction::with_access` bounds a short
synchronous RCU region and closes it before commit, error or unwind. The final
benchmark uses this mode and full Rust LTO/native CPU compilation. Ordinary
per-operation RCU remains supported; its performance is not qualified here.
The reference uses the same logical byte values and operation/retry semantics.

The runtime suite exposed an early-conflict change in the existing facade.
Missing/range reads now retain the first leaf observation when it becomes stale.
Own insertions only advance an exactly matching observation. Later staging can
complete, and final commit rejects the foreign phantom. Existing assertions
were preserved, with an additional native regression for this sequence.

Failed atomic-word copying, write-at-acquisition certification, broad inlining
and runtime-clock padding experiments were removed. Every writing observation
is still certified after all write locks. No specialized numeric engine,
resolved-record cache, second write set or new sanitizer suppression was added.
The earlier 32-thread mixed result of 75.5% is retained in the evidence record;
the full matrix was repeated after compacting the row's frequently written state.

This qualification covers eight-byte point reads and mixed updates on 100,000
keys with ten operations per transaction. Larger values, inserts, scans,
checkpoints and distributed RPC/log throughput retain functional coverage and
separate manual/weekly performance work. Upper proofs assume native storage
contracts; they do not prove C++ memory safety.
