# Mako local boundary: safety, concurrency, and overhead gates

This document is the reproducible gate for Item 4 of the Rust Mako transaction
cache plan. It covers the in-memory C++ Silo/MassTrans boundary through direct
C++, the raw `mako_local_*` C ABI, and the safe `mako-local` Rust wrapper. It
does not test RocksDB durability, distributed transactions, or the future Rust
OCC engine.

An all-green validation record below completes the executable Phase 1A-1D
boundary gate. It does not freeze revision 0, promote it to ABI v1, resolve the
remaining ABI-design decisions in Phases 1C/1D, complete Phase 1F, or complete
Milestone 1.

## Memory and thread safety

Rust-only ownership and lifecycle policy runs against the fake ABI under a
dated Miri toolchain:

```bash
rustup toolchain install nightly-2026-08-12 \
  --profile minimal --component miri --component rust-src --component rustfmt
./scripts/run_mako_local_miri.sh
```

The runner fails if the exact toolchain, Miri, or rustfmt is unavailable. It
never falls back to stable Rust or an ordinary `cargo test`. The fake ABI is
intentional: Miri cannot interpret the C++ engine, but it can exhaustively
exercise Rust handle ownership, output validation, transaction transitions,
and cleanup/quarantine policy.

Each native sanitizer gets a fresh CMake tree:

```bash
BUILD_DIR=build-mako-local-asan  MAKO_LOCAL_SANITIZER=asan  ./ci/ci.sh makoLocalSanitizerGates
BUILD_DIR=build-mako-local-ubsan MAKO_LOCAL_SANITIZER=ubsan ./ci/ci.sh makoLocalSanitizerGates
rustup toolchain install nightly-2026-08-12 --profile minimal --component rust-src
BUILD_DIR=build-mako-local-tsan  MAKO_LOCAL_SANITIZER=tsan  ./ci/ci.sh makoLocalSanitizerGates
```

The target first runs `MakoLocalAbiTests` plus the strict C11 and C++ ABI
conformance executables, then links the instrumented native archive into the
required-native Rust integration suite. That suite also executes the
direct-C++ and raw-ABI differential children. CMake propagates the matching
sanitizer runtime to Rust's final Clang link. Native CTest output is verbose,
successful child stderr is relayed, and Cargo runs with `--nocapture`, so a
passing LSan suppression summary remains in the gate transcript. ASan and
UBSan stop on their first unsuppressed finding; TSan stops on any finding
outside the reviewed Masstree optimistic-read suppressions. Missing native
artifacts, missing Cargo, or an unsanitized configuration fail rather than
reduce coverage.

TSan uses the same pinned `nightly-2026-08-12` as Miri plus
`-Zsanitizer=thread -Zbuild-std`, so the Rust wrapper, standard library, and
libtest harness are instrumented together with the Clang-TSan C++ archive.
Merely linking Clang's runtime into stable Rust's prebuilt standard library
causes false reports in its uninstrumented lock-free channels and is not an
accepted fallback. Rust test executables retain `--test-threads=1` for bounded,
deterministic reporting, not as a synchronization workaround. This does not
serialize the engine tests themselves: `worker_pools`, the history schedules,
and the native C++ suite still create their explicit worker threads and
exercise the concurrent boundary.

Cleanup/quarantine seams are tested in a distinct hook-enabled functional
profile:

```bash
BUILD_DIR=build-mako-local-hooks ./ci/ci.sh makoLocalHookGates
```

The profile must be configured with `MAKO_LOCAL_TEST_HOOKS=ON`; otherwise its
CMake target does not exist. It runs the complete required-native Rust suite,
the C11/C++ probes, and the native ABI test, including all five cleanup
failpoints plus deterministic midpoint-copy, locked-scan, and replay-comparator
interleavings. Cleanup-failure tests deliberately retain quarantined uncertain
state, so this profile is not combined with the strict hook-off LSan baseline.

The GitHub Actions `mako-local-sanitizers`, `mako-local-hooks`, and
`mako-local-miri` jobs run the same gates. Ordinary boundary CI additionally
runs the complete C++ ABI test instead of only compiling it.

ASan keeps leak detection enabled and aborts on every unreviewed leak. The
dedicated `src/mako/mako_local_lsan_suppressions.txt` file has three
frame-specific patterns covering only two revision-0 process-lifetime
ownership categories: `Sto::transaction` for retired-worker TLS storage, plus
`mako_local_table_open` and the direct differential control's
`DirectRunner::DirectRunner` for MassTrans table/epoch state that cannot be
reclaimed without global RCU quiescence. The direct control necessarily
bypasses the C ABI, hence the second table-allocation frame. No general engine,
allocator, source-file, or test-binary suppression is permitted.

The reviewed discovery baseline is exact: the 39-test native C-ABI process
reported 3 `Sto::transaction` roots / 94,104 bytes, 2 transitively retained
transaction buffers / 8,208 bytes, and 37 `mako_local_table_open` roots / 2,368
bytes—42 allocations / 104,680 bytes total. The direct-C++ differential child
reported 18 table roots / 1,152 bytes and 14 retained name buffers / 672 bytes—
32 allocations / 1,824 bytes total. A passing LSan suppression summary prints
root counts/bytes; retain it with the validation record and investigate any
change before updating this baseline. Allocations rooted at any other frame
remain fatal.

Published single-version values use layout-preserving atomic size and byte
access. A release fence after record-lock acquisition precedes every payload
store; an acquire fence after the optimistic copy precedes the final version
load. Consequently, observing any new byte forces that version load to see the
lock or a later version and reject the mixed snapshot. A hook-only midpoint
test forces this retry deterministically, while the sanitizer profile stresses
same-allocation 64-KiB/1,023-byte grow/shrink updates. This guarantee is scoped
to the local non-multiversion `versioned_str` path; generic and distributed
multiversion payload protocols are outside Item 4.

## Fixed-worker concurrency and soak

`crates/mako-local/tests/worker_pools.rs` runs exact pool sizes 1, 4, and 16.
The controller is separate from the pool. Every pool creates its workers once,
records their unique OS-thread identities, and sends every later phase to those
same threads. No transaction is moved between threads and no transaction gets
an ephemeral helper thread.

For every pool size the test:

1. stages one transaction per worker, commits a controller write, and requires
   every stale worker to return `Conflict`;
2. proves each conflicted worker can immediately commit a recovery write;
3. explicitly aborts on each worker, proves the write stayed invisible, and
   commits another recovery write;
4. completes 256 disjoint updates and 32 retried hot-counter increments per
   worker; and
5. verifies exact progress, counter, and recovery state before a final sentinel
   commit checks for a leaked lock or live transaction.

Startup, phase, release, soak, and shutdown waits are bounded. Every worker must
report shutdown and be joined. Per-worker update keys are pre-created so the
disjoint phase measures worker/transaction progress rather than legitimate
Masstree structural conflicts caused by concurrent tree growth.

The test is part of the required-native all-target suite. To isolate it in an
already verified build:

```bash
cd crates
mako_repo_root=$(cd .. && pwd)
MAKO_BUILD_DIR="${mako_repo_root}/build_item4" \
MAKO_LOCAL_FAKE_ABI=0 \
MAKO_LOCAL_REQUIRE_NATIVE=1 \
CARGO_TARGET_DIR="${mako_repo_root}/build_item4/cargo-target-mako-local" \
cargo test --locked -p mako-local --test worker_pools -- --nocapture
```

The absolute paths are intentional: Cargo build scripts execute from a package
context, so a relative `MAKO_BUILD_DIR` does not resolve from the invoking
shell's `crates/` directory.

## Relative wrapper overhead

Only optimized builds may measure overhead:

```bash
cmake -S . -B build_item4 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_item4 --target run_mako_local_overhead_benchmarks
```

The process-isolated matrix has four workers and crosses:

- direct MassTrans C++, raw C ABI, and safe Rust;
- read-only, write-only, and read-modify-write transactions;
- transaction sizes 1, 4, 16, and 64; and
- disjoint low contention and shared-key high contention.

Every configuration performs a warmup followed by seven fixed-successful-work
samples. Each surface executes in the fixed order direct C++, raw C ABI, then
safe Rust. The order is retained in the transcript so host drift is visible;
the harness neither randomizes the order nor claims to correct for it.

The four-worker pool is recreated for each of the 24 configurations. Because
STO attachment IDs are process-lifetime resources, each process-isolated
surface consumes a bounded and asserted 97 IDs: one controller/setup worker
plus `24 * 4` benchmark workers. This is intentional, bounded benchmark churn,
not the fixed-pool proof supplied by `worker_pools.rs`, and remains below the
460-ID process limit.

Every configured key is read back and compared with its exact expected value;
the transcript reports this as `validated_keys` plus the final checksum. This
does not claim that the point-read validation proves whole-table cardinality or
the absence of an unexpected extra key. Low-contention samples must have zero
conflicts. For every surface and every high-contention write/RMW configuration,
the seven measured samples in aggregate must have a conflict rate of at least
one percent, calculated as `conflicts / (commits + conflicts)`. A bounded
rendezvous prefix forces concurrent hot-key attempts so this is not left to
scheduler luck. High-contention read-only transactions are recorded but are
not expected to conflict. All high-contention results are excluded from the
wrapper-tax budget.

For each low-contention configuration, the harness takes the median duration
of seven samples. It records all 12 per-configuration `raw ABI / direct C++`
ratios and all 12 corresponding `safe Rust / raw ABI` ratios. For each workload
(read, write, and RMW), it also records the median and maximum across sizes
1/4/16/64. Both workload summaries for both boundary hops have an initial
same-host advisory sanity ceiling of `6.0x`. The maximum makes each underlying
low-contention configuration accountable; the median keeps the typical
workload shape visible. This wide ceiling catches debug builds, broken routing,
or catastrophic wrapper regressions. The benchmark remains an opt-in advisory
sanity gate, not a throughput promise, release SLA, or statistically controlled
cross-host comparison. Tightening it requires a controlled runner and a
baseline history.

Machine-readable transcripts and the wrapper-tax summary are retained beneath
`build_item4/mako-local-overhead-artifacts/`. Absolute throughput is diagnostic
only.

## Validation record

Item 4 is complete only when every row below is `PASS` from the same candidate
source state. A missing or skipped gate is not a pass, and neither is a failure
hidden by a new or ad hoc sanitizer suppression. Record the date,
host/toolchain identity, and artifact path or concise result before changing
the roadmap item to complete.

Candidate source: `<fill before declaring Item 4 complete>`  
Validation date and host: `<fill before declaring Item 4 complete>`

| Gate | Reproducible command | Result | Evidence |
| --- | --- | --- | --- |
| Rust ownership under Miri | `./scripts/run_mako_local_miri.sh` | `PENDING` | Pinned `nightly-2026-08-12`; record the test count/result. |
| C++/C/Rust under ASan | `BUILD_DIR=build-mako-local-asan MAKO_LOCAL_SANITIZER=asan ./ci/ci.sh makoLocalSanitizerGates` | `PENDING` | Record `MakoLocalAbiTests` and required-native Cargo results. |
| C++/C/Rust under UBSan | `BUILD_DIR=build-mako-local-ubsan MAKO_LOCAL_SANITIZER=ubsan ./ci/ci.sh makoLocalSanitizerGates` | `PENDING` | Record `MakoLocalAbiTests` and required-native Cargo results. |
| C++/C/Rust under TSan | `BUILD_DIR=build-mako-local-tsan MAKO_LOCAL_SANITIZER=tsan ./ci/ci.sh makoLocalSanitizerGates` | `PENDING` | Record the strict run and reviewed suppression file used. |
| Hook-enabled native seams | `BUILD_DIR=build-mako-local-hooks ./ci/ci.sh makoLocalHookGates` | `PENDING` | Record the native test count, five cleanup failpoints, midpoint payload retry, locked scans, comparator conflict, C11/C++ probes, and required-native Rust result. |
| Fixed pools 1/4/16 | Isolated `worker_pools` command above | `PENDING` | Record completion time and each pool's progress/conflict/abort result. |
| Release overhead matrix | `cmake --build build_item4 --target run_mako_local_overhead_benchmarks` | `PENDING` | Link the retained transcripts and record both hop summaries. |
| Required-native boundary suite | `cmake --build build_item4 --target run_mako_local_rust_tests` | `PENDING` | Record differential/history/binding/fingerprint result. |

When all rows are green, replace the placeholders with the observed results.
That records completion of Item 4 and the executable boundary gate only; the
ABI remains revision 0 until the separate design/freeze work is explicitly
accepted.
