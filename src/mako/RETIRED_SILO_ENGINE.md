# Original Silo transaction engine: retired

The original Silo transaction engine is preserved in this directory as
source-only history. It is not a supported Mako backend and must not be
compiled or linked into a Mako target.

Production Mako uses this path:

```
dbtest -> mbta_wrapper -> MassTrans -> Sto::start_transaction()
       -> Sto::try_commit()
```

The retired transaction plane consists of:

- `tuple.{cc,h}`
- `txn.{cc,h}`
- `txn_impl.h`
- `txn_btree.h`
- `base_txn_btree.h`
- `typed_txn_btree.h`
- `txn_proto2_impl.{cc,h}`
- `storage/ndb_wrapper.{h,impl.h}`

The adjacent archived test harnesses are `txn_btree.cc`, `test.cc`, and
`btree.cc`. The last is a raw `mbtree` test driver rather than transaction
implementation, but it was also incorrectly compiled into `libmako` and
included `txn.h`.

`txn.h` and `tuple.h` include `retired_silo_engine.h`, which deliberately
stops direct compilation with `#error`. The root CMake configuration also
asserts that none of the retired translation units appears in any build
target, including through `target_sources()` or a helper library.

Do not retire code merely because its name contains `Silo`. The following
support remains live and is used by STO/MassTrans:

- `SiloRuntime`
- allocator, core, RCU, thread, and ticker support
- `masstree_btree.h` and the current Masstree library
- `src/mako/sto` files in the configured production dependency closure
- `src/mako/storage/mbta_wrapper.hh`

Paxos and Raft select replication protocols only; neither selects the old
transaction engine.
