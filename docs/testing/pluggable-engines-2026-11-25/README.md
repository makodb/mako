# Pluggable distributed engine qualification

The implementation is qualified at `c79ba2617`. The final documentation commit
records these results without changing executable or proof inputs. Work is on
`pluggable-distributed-engines`, based on `mako-dev` at `2b0387b91`.

Both data engines use the same Rust STO/OCC + 2PC participant contract and
LocalLog or ReplicatedLog. Masstree recovers from checkpoints and committed log
entries without a RocksDB user-row mirror. Direct RocksDB atomically applies
rows, logical versions, protocol metadata and applied progress. Cross-engine
transactions and durable sharding/migration use those same contracts.

| Final gate | Result |
| --- | --- |
| Combined debug runtime, with crash hooks | 83 fresh-process cases passed |
| Combined release runtime, production features | 80 fresh-process cases passed |
| RocksDB-only runtime | 22 fresh-process cases passed |
| Upper tests and Verus | 175.663 seconds, 32 CPUs, 25 semantic negative controls |
| ASan / TSan | 18 selected cases each passed; no findings or startup retries |
| Native quality | Strict Clippy and doctests passed |
| Native source / link audits | 16 storage sources; 13 programs and 47 Rust archives |
| RocksDB-only dependency / symbol audit | All 7 executables passed; no Masstree |
| Raft host | 11 unit and 15 protocol cases passed |
| Direct RocksDB components | 19 STO cases and the shared adapter conformance case passed |
| Shared RocksDB log store | All selected component and crash targets passed |

The production features omit crash hooks. The debug matrix covers interrupted
Prepare, Freeze, partial final copy, saved Commit and partial cleanup; local
restart and Raft leader replacement; both cross-engine directions; raw and lazy
warehouse tables; stale grants and changed admission terms. Conversion covers
trimmed history, suffix replay, unresolved prepares, receipts, tombstones,
source leases, corrupt images and destination refusal. Real process cases cover
both engines, migration control recovery, and distinct shard/Raft-group IDs.

The upper gate checks protocol models and production Verus cores without native
storage, SRPC or Raft. Native serialization, checkpoint export/publication,
filesystem durability and backend conflict behavior remain adapter obligations.
The native and component gates exercise those obligations separately. ASan uses
instrumented Rust/Masstree with system RocksDB; TSan also uses an instrumented
RocksDB build and the existing scoped Masstree suppressions. Native kernel inputs
are unchanged from the sanitizer archive build. Performance remains a separate
weekly/manual campaign; the workload tests here assert correctness.

## Evidence

- [Final gate record](10-final.json), [upper source/input record](10-upper.json).
- [ASan](10-address.json), [TSan](10-thread.json), [components](10-components.json).
- [Native link audit](10-native-audit.json), [RocksDB-only audit](10-rocks-runtime-audit.json).
- [Offline conversion](09-conversion.json), [checkpoint retention](08-retention.json).
- [Durable migration](07-durable-migration.json), [build profiles](06-build-profiles.json).
- Stages `01` through `05` retain the interface, independent Masstree recovery,
  direct RocksDB adapter, engine/log matrix and migration adapter evidence.

## Operational bounds

Every replica in a Raft group uses the same data engine. Engine conversion is an
offline group operation into a new directory; it retains the source and preserves
log authority. The converter supports the shared RocksDB log format, including
legacy `DurableRows`, rather than retired segmented WALs or missing history.
It preserves existing migration control state but does not enable migration on
an old fixed-placement schema or import external legacy control journals.

Checkpoints pause committed publication. The protocol index and logical Masstree
conversion image are held in memory. Full replay identities, unresolved intent,
decisions and receipts are retained; physical log/checkpoint cleanup does not
provide protocol-record garbage collection. Process-kill tests do not prove
power-loss durability. No throughput improvement is claimed.

See the [completed plan](../../plans/pluggable-distributed-engines.md) and the
[book's conversion procedure](../../mako-book.md#offline-engine-conversion).
