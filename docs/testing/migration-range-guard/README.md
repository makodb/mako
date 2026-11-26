# Migration range-guard qualification

The sharding component requests logical range exclusion through
`RangeLocks`/`RangeGuard`. The shared transaction layer supplies the same
implementation to RocksDB and optional Masstree. Its engine-free tests run in
the upper protocol gate. Physical storage and recovery are checked separately
by runtime integration tests.

All builds and tests ran in the `mako-build:merge` container with Rust 1.97.1
and a 32-job budget. The two runtime feature profiles ran sequentially because
they share a Cargo target directory.

| Check | Result | Evidence |
|---|---|---|
| Upper protocol gate | Passed in 167.923 seconds on 32 CPUs; 55 workspace and 24 sharding cases | `upper-final.log`, `upper-report.json` |
| Production Verus proofs | 1,497 sharding, 40 host-core and 53 transaction-core items verified; semantic negative controls rejected | `upper-final.log` |
| Full optional-Masstree runtime | 104 cases passed in fresh processes before the final-copy retry adjustment | `native.log` |
| Final migration/adapter rerun | 24 cases passed, including all four engine pairings and eight durable migration cases | `native-final.log` |
| Strict Clippy | Changed runtime, protocol and storage API crates passed with warnings denied | `clippy-final.log` |
| RocksDB-only runtime/API | 50 cases passed in fresh processes | `rocks-runtime.log` |
| RocksDB-only dependency graph | Eight executable symbol audits passed; no Masstree dependency | `rocks-runtime-audit.json`, `rocks-runtime.log` |
| CI gate selection | Ten cases passed | `selector.log` |
| Component census | Three regression tests passed; generated census and logical component sums match | `inventory.log`, `consistency.log` |
| Proof input identity | All 224 recorded inputs match the final source | `consistency.log` |

The six engine-free range tests cover half-open binary bounds, missing/future
keys, overlapping scans, admission/acquisition races, retained prepared or
uncertain pins, duplicate and changed identities, stale generations, recovery
incarnations, live I/O scopes and explicit cancellation. Dropping a guard keeps
the range reserved. Cancelling migration does not release transaction pins.

The runtime adapter test checks shard/range authorization, missing and nested
scopes, cleanup and recovery. In each of the four engine pairings, the final
migration regression holds an earlier destination pin until an observed
final-copy retry, asserts that the migration remains pending, then releases the
pin and requires successful transfer. Crash and leader-failover cases cover
interrupted prepare, copy, saved decisions and cleanup.

The range manager is ordinary Rust tested against its contract, not a new Verus
proof. Existing actor proofs retain their storage and quiescence assumptions.
Drain conservatively waits for all earlier pins on the shard. Precise draining
of only overlapping transactions remains a future optimization. This change
does not alter persisted row or journal formats; the live handshake rejects
peers using the previous migration contract. No performance campaign was run.

Reproduce the engine-free qualification in the container:

```sh
python3 scripts/upper_check.py --cpus 32
```

With the native storage archive/environment configured, run the focused cases:

```sh
python3 scripts/run_native_rust_tests.py --manifest crates/Cargo.toml \
  --package mako-runtime --features replication,rocksdb-engine,test-hooks \
  --lib --test migration --test durable_migration
```

For a configured `ROCKSDB_RUNTIME` CMake directory, run the
`rocks_runtime_audit` and `rust_runtime_tests` targets. Native test selection
retains a fresh process for each case. The default runner still selects all
package tests.

The initial adapter test compilation exposed a `u32`/`u64` mismatch; the new
retry test initially called `unwrap` on an infallible epoch pin. Both setup
errors were corrected without weakening assertions. Failed compile logs are
retained as `native-compile-attempt.log` and `retry-test-compile-attempt.log`.
The final test-only correction is outside the upper gate's recorded input set;
all recorded proof inputs were checked again afterward. Saved text logs have
trailing whitespace and final blank lines normalized.
