# Rust RocksDB API qualification

The Rust API subset is implemented and qualified over distributed Mako OCC +
2PC with direct RocksDB and optional Masstree. The code cut is `43bdece32` on
`rust-rocksdb-compatible-api`, following implementation commits `bac254732` and
`2616b062f`. [Source hashes](source-sha256.json) identify the exact API, runtime
changes, shared replication fixture, manifests and build gates. The
[plan](../../plans/rust-rocksdb-api.md) and
[book](../../mako-book.md#rust-rocksdb-compatibility) define the supported subset.

All compilation, tests, proofs and example execution ran inside the build
containers. Host operations were source edits, Git and evidence collection.

| Check | Result | Evidence |
| --- | --- | --- |
| Isolated upper gate, 32 CPUs | 158.959 seconds; 49 workspace and 24 sharding tests; 29 semantic negative controls; all production proofs passed | [Report](upper/report.json) and adjacent stage logs |
| Initial full debug suite with runtime fault hooks | 130 fresh-process cases passed before the replica-startup adjustment | [Log](full-debug.log) |
| Final API debug suite | 28 fresh-process cases passed, including three-replica startup and failover | [Log](final-api-debug.log) |
| Final full optimized suite | 126 fresh-process cases passed | [Log](full-release.log) |
| RocksDB-only suite and link audit | 48 fresh-process cases and eight executable audits passed; no Masstree dependencies or symbols | [Log](rocks-only.log), [audit](rocks-runtime-audit.json) |
| Shared replication fixture after correction | 15 combined cases and five RocksDB-only cases passed | [Combined](replication-debug-after-fixture-fix.log), [RocksDB-only](replication-rocks-after-fixture-fix.log) |
| AddressSanitizer | Five selected native API cases passed, no startup retries | [Report](address/report.json) |
| ThreadSanitizer | Five selected native API cases passed, no startup retries | [Report](thread/report.json) |
| Strict Clippy | API/runtime and their targets passed with warnings denied; CMake quality target also passed | [Final check](clippy.log), [CMake](cmake-quality.log) |
| Ownership documentation | Transaction cannot be Sync; native Worker cannot cross its ownership boundary | [API doc test](doc-tests.log), [CMake doc tests](cmake-quality.log) |
| Native source audit | 16 storage C++ sources; no application or generated C++ | [CMake log](cmake-quality.log) |
| Compiled example | Reopened one directory as RocksDB, Masstree, then RocksDB; account values matched each time | [Log](example.log) |

The final upper report's source-input hashes match the finished tree. The later
fixture correction changes only native replication tests, outside those inputs.
Counts from native Cargo discovery include the child-process test entry point;
its real work runs as a subprocess of the crash and distributed cases.

## Coverage and boundaries

Fourteen backend-independent tests cover binary CRUD, read-your-writes,
rollback/reuse/Drop, poisoning, CF ownership, ordered atomic batches, unsupported
options, iterator ordering/bounds/overflow/errors, multi-get ordering, conflicts
without hidden retry, unknown identities, and sticky uncertain rollback state.
The upper gate rejects native dependencies and disables C/C++ compilers.
These adapter tests complement the existing protocol proofs; they do not prove
native storage, transport or filesystem behavior.

Native tests use the production coordinator. They cover both storage modes,
thread transfer of transaction handles, stream capacity and release, absence and
range conflicts, cross-shard transactions and batches, local/Raft durability,
SIGKILL recovery, saved-ID resolution after reopening, mode switching, and
point/forward/reverse reads through a migrated range. The three-replica case
starts replicas sequentially before a quorum exists, keeps followers open,
stops the leader, and resolves the original identity on its successor.

The lost-reply case drops the coordinator's successful commit response. The API
must return UnknownCommit with the same identity and refuse stream reuse until
resolution confirms the saved decision. It never reruns the transaction body.

The five sanitizer cases cover lost-reply cleanup, bounded streams, both storage
modes' CRUD/conflict/range/reopen paths, and opening/schema refusal. They reuse
the existing instrumented native profiles and their scoped suppressions. No new
suppression was added. TSan uses instrumented RocksDB; ASan uses the profile's
system RocksDB. The [driver](sanitizer.py), profile manifests and individual case
logs are retained under `address` and `thread`. These runs are scoped checks,
not a claim that every lower component has been exhaustively checked.

## Fixture correction

The first optimized run failed in the existing replica-discovery/receipt test:
a coordinator attempt returned RouteChanged because discovery saw no healthy
leader during ongoing apply. The fixture immediately unwrapped that retryable
result. The [original output](release-before-fixture-fix.log) is retained.

The fixture now retries only RouteChanged within a 20-second deadline, using
the same Client and its fencing rules. It does not retry unknown outcomes,
conflicts or transport errors. The SDK and coordinator have no new automatic
body retry. The affected replication cases and full optimized suite passed
after this correction. [replication_only.py](replication_only.py) is the existing
fresh-process runner with its executable selection restricted to the three
replication test binaries.

Earlier API development also corrected absent-key Delete to match RocksDB and
reserved test listener ports so outgoing ephemeral sockets cannot claim them.

## Reproduction

Use the repository build container and its normal native build environment:

```sh
bash ci/upper.sh --cpus 32
cmake --build build_retired --parallel 32 --target rust_shard_integration_tests
cmake --build build_retired --parallel 32 --target rust_runtime_release_tests
cmake --build build_retired --parallel 32 --target rust_native_quality native_storage_source_audit
cmake --build build_docker/rocks-runtime-profile --parallel 32 --target rocks_runtime_audit rust_runtime_tests
```

The normal native debug target omits deliberate runtime fault hooks. The full
debug qualification enabled `mako-runtime/test-hooks` through the native runner.
Performance campaigns remain manual or weekly. This work makes no throughput
claim and did not push or publish the branch.
