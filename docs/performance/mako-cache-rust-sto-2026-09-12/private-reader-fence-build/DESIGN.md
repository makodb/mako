# Concurrent read-only commit cuts

Private experiment. Production is unchanged by this directory.

The cache currently holds `CommitFence::read_only_serial` across its packed
order cut, scan of all 460 writer slots, unknown-outcome check, and native
read-only commit. Writers never acquire this mutex. Each reader's cut and
observed slot generations are local state, so the mutex appears unnecessary.

The candidate removes only that mutex and its guard. It preserves the packed
word's Acquire `fetch_add(0)`, every writer-slot Acquire load, exact active
generation draining, the unknown-outcome check, and engine validation.
It does not shorten the scan or introduce an attachment high-water mark.

## Ordering argument

Each writer publishes its odd slot generation with Release before entering
packed order. Each reader's Acquire RMW reads the last preceding modification
of that same word. Other readers' RMWs remain in the writer's release sequence.
The following slot load therefore cannot miss a prefix writer's active
generation unless it sees a later generation. The writer publishes its outcome
before clearing its slot with Release. Observing that clear, or draining the
exact active generation until it changes, acquires the outcome independently
for each reader.

A post-cut writer may be absent from the scan. Its locked writes still cannot
be installed before ordering and validation. Engine validation rejects a
conflicting read set or predicate, or admits the reader before that writer.
Concurrent reader cuts do not change this existing argument.

The runtime permits an already-admitted transaction to overlap a later runtime
failure. Concurrent read-only commits do not strengthen that promise. New begin
and commit admission checks, and unknown-outcome checks, remain intact.

## Private checks

The candidate passes the four component fence tests. Restoring the old mutex
fails the new overlap assertion after both child threads have been released and
joined. `base-negative.log` is an expected failure, not a passing qualification.

`reader-loom-reduced.log` passes all three Loom models without a preemption
bound. The two-reader model keeps the three protocol atomics in one shared Arc
owner. It omits outcome reads when the writer is outside the reader's prefix,
or when the reader must follow the separately modeled exact-generation wait.
This avoids enumerating unrelated Arc ownership interleavings. The earlier
three-Arc run was interrupted, and its log is retained as incomplete evidence.

`reader-sto-lib.log` passes 203 library tests without the explicit engine test
hook feature. Full hook-enabled and two-reader unknown-outcome checks are
separate required follow-ups before performance promotion.

## Measurement

`build-main.sh` builds matched baseline and candidate binaries for each engine
with Rust 1.95, Zen 1, non-LTO, one release codegen unit, and the same native
archives and RocksDB library. Both copies use the current exact per-key sample
and fresh-process recovery verifier. This main benchmark includes client key
allocation, latency recording, cold measured-thread attachment, and thread
joins. Its rates are not interchangeable with `engine_compare` loop rates.

Required comparison is read tx1 with 1, 4, and 32 workers on both engines,
three alternating repetitions, boost disabled on zoo-002. A speedup here does
not establish a write-path improvement or Rust STO parity with C++ STO.
