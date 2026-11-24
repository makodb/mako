# Native storage support

This directory contains the C++ storage support used by Rust applications.
Production application code lives in [`crates/mako-runtime`](../../crates/mako-runtime),
and SRPC builds directly through Cargo. The C++ application, transaction
engines and Paxos implementation have been removed.

[`cmake/native-storage-sources.txt`](../../cmake/native-storage-sources.txt)
lists every retained production C++ source. These implement the Masstree
storage ABI, allocation, thread registration and reclamation. RocksDB uses
its external C ABI. See the [retirement map](../../docs/developer/native-rust-retirement.md)
for the native replacements and supported combinations.

Use the [root build instructions](../../README.md). From the repository root
inside the build container, run transaction, tree and application-adapter
tests separately with:

```sh
bash ci/native_rust.sh components
bash ci/native_rust.sh storage
bash ci/native_rust.sh runtime
```

The original OSDI'25 experiment notes and launch commands are historical.
They remain available at `git show f7183cf72:src/mako/README.md` and do not
apply to the native Rust application. The older transaction-engine retirement
history is recorded in [RETIRED_SILO_ENGINE.md](RETIRED_SILO_ENGINE.md).
