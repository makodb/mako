# Masstree transaction component qualification

Qualified in the `mako-build:merge` development container with Rust 1.97.1,
Clang 22.1.8, CMake 4.3.3 and 32 build jobs. The initial
`CI_BUILD_JOBS=32 bash ci/masstree_tx.sh` used a fresh `build_masstree_tx` directory.
No application Cargo artifacts were reused.

- `run.log`: independent native index/library compilation, 346 local cases in
  fresh processes, four compile-fail doctests and strict wrapper Clippy passed.
  Existing native-index compiler warnings remain visible in this log.
- `dependencies.json` and `compiler-artifacts.jsonl`: 25 compiled packages;
  27 packages including declared test dependencies. The actual native libraries
  are the index/support archive, libc++, libc++abi, NUMA, pthread and jemalloc.
  No RocksDB, SRPC, Raft, application runtime or full sharding runtime appears.
- `default-build.log` and `targets.txt`: default component compilation succeeds
  without running tests; the profile exposes no application targets.
- `local-tests.log`: after wiring the audit controls into the test target,
  all four audit unit tests, 346 local cases and four doctests passed again.
  General byte-key tests remain enabled alongside the `fixed-u64` tests.
- `ctest-discovery.log`: the independent `masstree-tx` label selects the
  `masstree_transaction_component` test entry.
- `native-configure.log` and `core-index-tests.log`: the production profile
  still configures. Its separate shared core/support suites and 28 index/ABI
  cases passed. The index suite no longer selects either transaction adapter.

The selector's ten unit tests and shell/YAML syntax checks passed. Component
census regeneration, its three regression tests and snapshot ownership sums
also passed. These results cover the component build/test separation. They do
not constitute a new full distributed-runtime or performance qualification.

Logs retain the commands and diagnostic content; trailing whitespace is removed
before checking them into Git. The component has its own Cargo directory to
avoid application feature variants overwriting its native test artifacts.
