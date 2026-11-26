# Use the maintained RocksDB crate

Replace the handwritten row, Raft-log and sharding-metadata bindings with `rocksdb = 0.25.0`.
Use its pinned `librocksdb-sys` native build, RocksDB 11.8.1, for all three consumers.
The existing environment provides 8.9.1, which cannot satisfy the current Rust
wrapper's complete API. Keep one native RocksDB implementation per executable.

1. Save a fixture written by the existing 8.9.1 adapters, including SST data,
   checkpoints, tombstones, empty/binary values and unflushed WAL suffixes.
2. Replace raw handles, FFI declarations and manual cleanup with the maintained
   crate. Keep Mako's byte-store contract, durability policy and failure hooks.
   Retain owned snapshots through a safe ownership wrapper. Preserve log formats.
3. Remove custom link scripts and native-presence test skipping. Update native
   dependency audits, sanitizer library selection and the component inventory.
4. Verify the old fixture, storage/log recovery, both runtime engine modes,
   one-shard behavior and upper protocol proofs. Update the book and agents.md.

The native engine upgrade is forward-qualified. Opening files written by the
new engine with an older RocksDB release is not a promised downgrade path.
Performance qualification remains separate from development correctness checks.

Status: complete. All three adapters use the maintained crate; custom FFI,
native link scripts and native-presence test skipping are removed. Container
setup uses the upstream build. Recovery, metadata, both runtime profiles,
production links, upper proofs and the TSan native dependency build passed.
See [qualification results](../testing/maintained-rocksdb/README.md).
