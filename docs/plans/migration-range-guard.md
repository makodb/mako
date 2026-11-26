# Explicit migration range guards

Migration requests exclusive access to a logical range through a defined
transactional-storage interface. The shared implementation must work with both
RocksDB and optional Masstree, without moving migration policy into either index.

1. Define the range, generation/ownership identity and guard lifecycle. Preserve
   freeze-before-drain ordering, retained prepared/uncertain transactions,
   authorized migration effects and fencing after ownership handoff.
2. Implement the contract in the shared transactional layer using admission
   epochs. Keep the existing conservative drain of earlier shard work initially.
   Test range boundaries, pending/drained acquisition, cancellation, stale
   handles and ownership transitions without native engines.
3. Replace sharding's loose cut/drain callbacks with the explicit contract.
   Connect both storage modes through the common runtime adapter and retain
   existing transaction locking, journaling and recovery behavior.
4. Run component checks, upper protocol proofs and relevant migration/runtime
   regressions in the container. Update the book, interface inventory, component
   accounting and qualification notes with the actual guarantees and limits.

Status: complete. Shared guards and the common runtime adapter serve both
engines. Final-copy waits for destination drainage without aborting migration.
The upper gate passed in 167.923 seconds on 32 CPUs. Native qualification passed
104 runtime cases, followed by 24 focused cases after the retry change;
the RocksDB-only runtime/API passed 50 cases and its dependency/symbol audit.
Strict Clippy, CI selection, component accounting and final proof-input checks
passed. The book and agents.md record the contracts and updated ownership/LOC.
See the [qualification record](../testing/migration-range-guard/README.md).
