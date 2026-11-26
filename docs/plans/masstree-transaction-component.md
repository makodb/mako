# Isolate the Masstree transaction component

The component owns `sto-masstree` and `sto-tpcc-ffi`. The shared OCC engine stays
in `sto-core`, and Masstree itself stays a separate component. Runtime adapter
`mako-storage-masstree` remains the consumer boundary.

1. Add a component-only CMake profile and container command. Compile only these
   two packages and their shared core/index dependencies. Audit normal, build
   and test dependencies to exclude RocksDB, SRPC, Raft and the application.
2. Give this component its own fresh-process local tests, doctests, existing
   wrapper quality checks and CI selection. Keep index/core tests separate.
3. Update ownership, interfaces, component counts and the book. Verify the
   component from a fresh target directory and record the actual built packages.

The component remains usable by the distributed runtime; this changes ownership
and build/test boundaries without inventing a second transaction protocol.

Status: complete. `MASSTREE_TX` and `ci/masstree_tx.sh` provide the independent
build and gate; CI selects it separately. A fresh build passed its dependency
audit, 346 local cases, four doctests and strict wrapper Clippy. Shared core and
index suites still pass separately. Ownership, interfaces and counts are updated
in `agents.md` and the book. See the
[qualification record](../testing/masstree-transaction-component/README.md).
