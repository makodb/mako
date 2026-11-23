# Original Silo transaction engine: removed

The original Silo transaction engine has been removed from this directory.
It is not a supported Mako backend.

Production Mako uses this path:

```
dbtest -> mbta_wrapper -> MassTrans -> Sto::start_transaction()
       -> Sto::try_commit()
```

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
support remains live and is used by STO/MassTrans:

- `SiloRuntime`
- allocator, core, RCU, thread, and ticker support
- `masstree_btree.h` and the current Masstree library
- `src/mako/sto` files in the configured production dependency closure
- `src/mako/storage/mbta_wrapper.hh`

Paxos and Raft select replication protocols only; neither selects the old
transaction engine.
