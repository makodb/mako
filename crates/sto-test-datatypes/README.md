# STO reference datatypes

This crate contains safe Rust collection adapters used to test `sto-core`
without Masstree or C++ code. It is a correctness reference, not a collection
performance package.

The crate provides:

- `TxnHashMap`, a fixed-bucket map with one logical STO item, version, and lock
  per bucket;
- `TxnVec`, a dynamic vector represented by one immutable snapshot; and
- `TxnQueue`, a FIFO queue represented by one immutable snapshot.

All mutations build their complete replacement state while the transaction is
still reversible. Commit-time installation only swaps a prepared `Arc`. This
keeps allocation, cloning, comparison, and hashing out of the irreversible
install phase.

## Conflict domains

`TxnHashMap` allows independent commits to different buckets. A read miss is
witnessed by that bucket's version, so a later insertion into the same bucket
invalidates the read. Different keys in the same bucket may conflict, and the
bucket count cannot change after construction.

`TxnVec` and `TxnQueue` each use one version and lock for the entire collection.
This makes sequence semantics and rollback easy to audit but serializes all
writes to the same collection.

Use `sto-masstree` when the workload needs a production ordered index. These
adapters exist to exercise the public datatype trait, cross-adapter atomicity,
read-your-writes, stale-read detection, and rollback.

## Isolation checks

`tests/isolation_litmus.rs` fixes the relevant thread interleavings with
channels. It checks write-skew rejection across independent array slots,
invisibility and rollback of a staged write, and rejection of a fractured
read across a hash map and vector.

`tests/strict_serializability.rs` runs 128 seeded concurrent workloads. Each
history has three workers execute two transactions over independently
versioned map buckets. The test records transaction invocation and response
times, every operation result, commit outcomes, and final state. An independent
depth-first checker searches all legal orders of the committed transactions,
preserves real-time precedence, replays their operations against a small model,
and requires the replayed final state to match storage. Checker self-tests
include a valid history, a write-skew cycle, and a real-time-order violation.

The supported `Serializable` profile is deliberately nonopaque, so observations
from aborted transactions are not required to form a consistent snapshot and
are excluded from the serial-order search. The targeted tests separately check
that uncommitted state is never published and that a fractured view cannot
commit. These bounded tests are executable regression checks, not a proof over
all programs or machine-level schedules.
