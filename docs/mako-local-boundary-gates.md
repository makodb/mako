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

The accepted ASan run on candidate `5a3dd3eaf` retained the following exact
LSan suppression-table rows. These are the rows printed by LSan, not total
process allocation counts:

| Process | `Sto::transaction` | `mako_local_table_open` | `DirectRunner::DirectRunner` |
| --- | ---: | ---: | ---: |
| 40-test native C-ABI process | 8 / 250,944 bytes | 38 / 2,432 bytes | — |
| Differential direct-C++ child | — | — | 32 / 1,824 bytes |
| Differential raw-ABI child | — | 32 / 1,824 bytes | — |
| Differential safe-Rust child | 1 / 31,368 bytes | 32 / 1,824 bytes | — |
| Injected-divergence direct child | — | — | 32 / 1,824 bytes |
| History-oracle process | 4 / 125,472 bytes | 3 / 192 bytes | — |
| Transactions process | 23 / 721,464 bytes | 26 / 1,584 bytes | — |
| Fixed-worker process | 22 / 690,096 bytes | 6 / 336 bytes | — |

Relative to the preceding 39-test discovery run, the native process gained
exactly one 64-byte table row and five 31,368-byte transaction rows: the new
concurrent-payload fixture plus its writer and four readers. The direct-C++
row remains 32 / 1,824 bytes. Any future drift requires investigation and a
deliberate update to this record. Allocations rooted at any other frame remain
fatal.

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

Item 4 was accepted only after every row below passed from the same candidate
source state. No failure was hidden by a new or ad hoc sanitizer suppression.

Candidate source: `5a3dd3eaf5cb3b9c57de37b20879a71c53cf9a30`

Validation date and host: 2026-08-25, `zoo-005`, Linux
`7.0.14-5-pve` x86_64

Toolchains: Clang 21.1.8; Rust/Cargo 1.97.1; pinned Miri and Rust TSan
`nightly-2026-08-12` (`rustc 1.99.0-nightly`, Miri 0.1.0)

| Gate | Reproducible command | Result | Evidence |
| --- | --- | --- | --- |
| Rust ownership under Miri | `./scripts/run_mako_local_miri.sh` | `PASS` | 7/7 under the dated pin; `build_item4_final/item4-miri-final-v2.log` (`sha256:7eb1550f06927080419aaf631ef4d338c7e8e77ebc209be6a3f1f37b71a80bc4`). |
| C++/C/Rust under ASan | `BUILD_DIR=build-mako-local-asan MAKO_LOCAL_SANITIZER=asan ./ci/ci.sh makoLocalSanitizerGates` | `PASS` | Native 40/40, CTest 3/3, Rust 41 passed + 1 intentional role ignored, no unsuppressed finding, and all 13 retained rows recorded above; `build_item4_gate_probe_asan/item4-asan-final-v2.log` (`sha256:e8fbf78832cd1a5c86b951ae07c59989387537af8e11eb10b686e8ebcd6bbbad`). |
| C++/C/Rust under UBSan | `BUILD_DIR=build-mako-local-ubsan MAKO_LOCAL_SANITIZER=ubsan ./ci/ci.sh makoLocalSanitizerGates` | `PASS` | Native 40/40, CTest 3/3, Rust 41 + 1 ignored, zero diagnostics; focused RCU callback/memtag regression 1/1; `build_item4_gate_ubsan/item4-ubsan-final-v2.log` (`sha256:ba8a2edc6e14f716ccdebd7a6e9f93634974b19cecce95500e0414f2e34ffaa5`). |
| C++/C/Rust under TSan | `BUILD_DIR=build-mako-local-tsan MAKO_LOCAL_SANITIZER=tsan ./ci/ci.sh makoLocalSanitizerGates` | `PASS` | Native 40/40, CTest 3/3, fully instrumented Rust/std/libtest 41 + 1 ignored, zero warnings; 270 matches from the reviewed Masstree list; `build_item4_gate_tsan/item4-tsan-final-v2.log` (`sha256:4b5961f4a9184ec67341ec7d3cde6f93480fbcf1ef44ea462e813cf284f0eebc`). |
| Hook-enabled native seams | `BUILD_DIR=build-mako-local-hooks ./ci/ci.sh makoLocalHookGates` | `PASS` | Native 48/48, CTest 3/3, Rust 41 + 1 ignored; all five cleanup failpoints plus midpoint-copy, locked-scan, and comparator seams passed; `build_item4_hooks/item4-hook-gate-final-v2.log` (`sha256:2add4b4d55a38bbcdb340cffa1a1e30da183b23faa25f738f769eb41a030cbce`). |
| Fixed pools 1/4/16 | Isolated `worker_pools` command above | `PASS` | Exact stable pools completed conflict/abort/recovery/soak with retry counts `(0, 18, 1503)`; `build_item4_final/item4-worker-pools-final-v2.log` (`sha256:92d3c381a42a808fa46cd264f4c5e500d05a161d1ade358dc9b0013a63c754e2`). |
| Release overhead matrix | `cmake --build build_item4 --target run_mako_local_overhead_benchmarks` | `PASS` | ABI/direct aggregate median 1.468411x, maximum 3.522749x; safe/ABI aggregate median 0.848936x, maximum 1.744112x; all high-contention write/RMW rates exceeded 1%; artifacts in `build_item4_final/mako-local-overhead-artifacts/mako-local-overhead-2287216-0/`; log `sha256:499f2e6eb8653e0284453729e050a2b710f0bd7e46538acdbee1c5ec992e04d7`. |
| Required-native boundary suite | `cmake --build build_item4 --target run_mako_local_rust_tests` | `PASS` | History 12/12, fake-ABI 7/7, required-native 41 + 1 ignored, docs 4/4; 32-symbol allowlist and hook-off fingerprint `0fdce521373a479105052c672e2b2fd2f66a780abb07177a50c56d3b2596b11a` verified; logs `sha256:3b1a2425ec172b399290052a5d15f7a57acc6a32fa6ee587a8878b8a711c650a` and `sha256:c1c0d4882e485d7b43c02e3b6f48e237482ce8d6fca431e47155c4cd6c7adfc3`. |

This all-green record completes Item 4 and the executable Phase 1A-1D boundary
gate only. The ABI remains revision 0 until the separate design/freeze work is
explicitly accepted.
