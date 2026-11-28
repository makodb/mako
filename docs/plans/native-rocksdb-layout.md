# Native RocksDB user-data layout

Historical qualification of the native-layout introduction. The subsequent
[current-format cleanup](drop-old-mako-formats.md) removes the
old Mako format readers and converter described below.


Status: complete. Native user-family storage, adoption, runtime wiring, local
and replicated log identity, and offline conversion are implemented and qualified.

Final checks passed: 47 storage cases, 27 Raft host cases, 73 focused runtime/API
recovery cases, strict Clippy, dependency boundaries and component accounting.
The broader runtime/API run passed 130 cases before the final LocalLog identity
and adoption-retry fixes; the 73-case run covers those final changes. The final
engine-free upper gate passed in 160.741 seconds on 32 CPUs. No benchmark was run.
See the [qualification record](../testing/native-rocksdb-layout/README.md).

Qualification found and fixed snapshot-generation lease accounting, a recursive
lock in initial-state import, ambiguous digest boundaries, local-journal seed
binding and failed-preflight retry behavior. Native upstream fixtures have an
explicit integration-test feature, keeping them outside the upper gate. Earlier
failed diagnostics remain in the qualification directory.

Use native column families, unchanged user keys and unchanged user values for
both direct RocksDB and optional Masstree. Keep distributed STO metadata in an
internal family, updated atomically with rows. LocalLog and ReplicatedLog retain
their common journal. A RocksDB sequence number is not a distributed commit time.

## Stages

1. Add native column-family batches and snapshots to the existing safe storage
   adapter. Implement the versioned row layout and preserve isolated component
   tests.
2. Adopt stopped native databases with a bounded scan that validates supported
   keys/values and initializes metadata without rewriting user records. Persist
   adoption progress and identity before publication; retry interrupted setup.
   Keep the native database path usable across checkpoint generation changes.
3. Pass schema names through the runtime, open existing databases through the
   Rust facade, and bootstrap consistent LocalLog/Raft authority. Load adopted
   rows in Masstree and include them in migration and checkpoints.
4. Upgrade existing Mako formats with the offline converter, preserving decided
   transactions, pending work, tombstones and replay identities.
5. Qualify component and runtime behavior, update the book and component ledger.

## Acceptance

- Native RocksDB fixtures with multiple families, binary and empty keys survive
  adoption and Mako updates; native readers see unchanged application encoding.
- Reserved-family collisions, incompatible options and oversized records fail
  explicitly. Adoption never treats damaged Mako metadata as a fresh database.
- Row/version/recovery effects remain atomic across families.
- Interrupted adoption, checkpoint installation and replay recover coherently.
- Both engines reopen the same layout; Masstree materialization gaps recover
  from the shared local/replicated journal.
- Initial state is available to every replica before transaction admission.
- The old-format converter retains transaction/replay state and publishes only
  a verified destination.
- Upper-layer proofs remain independent of concrete engines. Native adapter and
  integration tests run separately, inside the container. No performance run is
  required for this format change.

## Compatibility contract

Existing files must be a stopped RocksDB database with supported options and
bytewise ordering. Initial adoption may scan all rows and write metadata, but
does not re-encode user records. Once adopted, mutations must go through Mako.
Native inspection sees the materialized prefix; a current export must drain
Masstree's committed gap first. Native tools must account for the internal
column family. Custom comparator, merge-operator and TTL semantics remain outside the supported
contract. Oversized rows and reserved-family collisions fail preflight.
