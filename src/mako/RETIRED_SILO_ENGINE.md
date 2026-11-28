# Retired C++ transaction engines

The original Silo transaction engine has been removed from this directory.
It is not a supported Mako backend.

Production Mako uses Rust as its sole transaction engine:

```
mako-runtime / native SRPC -> sto-masstree-api -> sto-core + sto-masstree
mako-cache -> mako-engine -> mako-engine-sto -> sto-core + sto-masstree
```

`--storage-engine=rust` remains accepted; omitting it selects Rust. Requests
for `cpp` fail. The C++ application and `rust_sto_tpcc_wrapper` were removed
during the native Rust migration on 2026-11-24. See the
[retirement map](../../docs/developer/native-rust-retirement.md).
The C++ `Transaction`, `MassTrans`, `mbta_wrapper`, native
sharding storage backend, and `mako-local` / `mako-local-sys` crates were
removed during retirement on 2026-11-23. Their last pre-retirement revision is
`0d26e2e0b`. Runtime interfaces formerly colocated with that engine now live
under `src/mako/runtime/`; they do not instantiate a C++ transaction.

The older Silo removal is recorded below.

The removed transaction plane consisted of:

- `tuple.{cc,h}`
- `txn.{cc,h}`
- `txn_impl.h`
- `txn_btree.h`
- `base_txn_btree.h`
- `typed_txn_btree.h`
- `txn_proto2_impl.{cc,h}`
- `storage/ndb_wrapper.h` and `storage/ndb_wrapper_impl.h`

The adjacent archived test harnesses were `txn_btree.cc`, `test.cc`, and
`btree.cc`. The last was a raw `mbtree` test driver rather than transaction
implementation, but it had also been incorrectly compiled into `libmako` and
included `txn.h`.

The files named above were deleted on 2026-11-22, together with the guard
that kept them out of the build (`retired_silo_engine.h` and the CMake
assertion). The last commit that has them is `fd3ed37e1`; read one with
`git show fd3ed37e1:src/mako/<file>`.

Do not retire code merely because its name contains `Silo`. The following
support remains live and is used by Rust applications and Masstree:

- `SiloRuntime`
- allocator, core, RCU, thread, and ticker support
- `masstree_btree.h` and the current Masstree library
- `src/mako/runtime/thread_registration.{cc,hh}` for storage threads
- `src/mako/storage/mtree_abi` and `mtx_abi` storage bindings

Application workers, replay and RPC policy now live in Rust. Raft is the
supported replication protocol; it uses the same Rust transaction engine.

Run checks inside the build container:

```sh
cmake --build build --target rust_sto_transaction_tests  # in-memory datatypes
cmake --build build --target rust_masstree_tests         # native tree boundary
cmake --build build --target rust_application_adapter_tests
cmake --build build --target rust_cache_tests           # cache, models and recovery
bash ci/native_rust.sh runtime                          # debug/release application cases
```

Native application tests, sharding fixtures, and protocol tests use the Rust
adapter. The retained transaction corpus is replayed against an independent
ordered-map model, with no executable reference to the retired engine.
