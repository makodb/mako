# Mako local boundary: safety, concurrency, and overhead gates

This document is the reproducible validation record for Items 4 and 5 of the
Rust Mako transaction-cache plan. Item 4 covers the in-memory C++
Silo/MassTrans boundary through direct C++, the raw `mako_local_*` C ABI, and
the safe `mako-local` Rust wrapper. Item 5 extends that boundary through the
volatile ordered cache and asynchronous RocksDB application contract. Neither
record tests disk-sync durability, distributed transactions, or the future
Rust OCC engine.

The first all-green record below completes the executable Phase 1A-1D boundary
gate. The separate Item 5 record completes Phase 1F for the current
asynchronous contract. Subsequent closure work resolved the remaining
Phase 1B-1E contract items described below. The ABI intentionally continues to
report revision 0 until a separate promotion action. Milestone 1 final
acceptance was completed by the comparative zoo-2 benchmark on 2026-08-26.
Historical Item 4 and Item 5 evidence remains tied to the exact commits named
in its records.

## Current Milestone 1 closure status

The functional and contract rows through Phase 1F are complete:

- **Phase 1B:** `CacheOptions::isolation` makes the isolation profile explicit.
  `StrictSerializable` is the production default; requesting `Opaque` rejects
  an engine that does not advertise the opacity feature. Point and scan
  read-your-writes remain required by the ordinary cache profile.
- **Phase 1C:** database open has a sized, append-only options prefix and keeps
  the original default-options symbol. The freeze choice retains implicit TLS,
  the conditional all-output-pointer rule, and process-lifetime STO worker,
  MassTrans table, and epoch state. Direct concurrent table-open tests cover
  same identity, name conflict, and numeric-ID conflict. A fresh-process probe
  attaches 460 distinct joined workers, checks idempotence on each one, and
  requires worker 461 to return `THREAD_LIMIT`. Because `LocalDb::open()`
  consumes one slot, `FixedWorkerPool` rejects configurations above 459 even
  in a fresh process; prior attachments may lower actual availability.
- **Phase 1D:** compile-fail examples pin transaction thread and database
  lifetimes. The production `FixedWorkerPool` uses bounded per-worker queues,
  health checks after every closure and unwind panic, poisoned-worker
  retirement, pending task failure, metrics, clean drain/join, and explicit
  conflict-only bounded retry with attempt/conflict reporting.
- **Phase 1E:** integrated native-cache tests hold the backend stopped while a
  two-record queue backpressures 8 writers across 128 commits, prove clean
  close drains and reopens six acknowledged records, prove forced process stop
  retains the applied prefix while discarding only its unapplied volatile
  suffix, and prove near-exhaustion recovery mints the final valid timestamp
  without consuming visibility or `CacheSeq` on the following exhaustion.
- **Phase 1F:** the cleanup, mutation, history, crash, and replay gate remains
  accepted by the Item 5 record below.

Phase 1G value eviction is explicitly deferred until after Milestone 1. Every
live value remains resident in Silo, so the live dataset must fit in RAM, and
backend commit-record history currently has no reclamation policy. This does
not make the writeback queue unbounded: `WritebackConfig::capacity` bounds the
detached permits plus prepared/ready in-memory records and applies producer
backpressure before native commit.

Exactly one recovered cache namespace per process is a Milestone 1 deployment
precondition, not a mutex-enforced feature. A future multi-namespace supervisor
must discover and scan every backend and floor the shared timestamp authority
before any namespace admits work.

The final Milestone 1 acceptance row is complete. The linked record retains the
methodology, all medians, the machine-readable report, and the significant
concurrent-write scaling limitation exposed by the run:

| Gate | Candidate | Host | Result | Evidence |
| --- | --- | --- | --- | --- |
| Cache-level throughput, aborts, retry-inclusive p50/p99, drain/recovery time, and log/backend amplification versus `mrx` and raw RocksDB | `6574cf47c3233f208d5b2e68790e411c2ea3debe` | `zoo-002` (`zoo-2`), CPUs 0-15 | **PASS** | [Milestone 1 acceptance record](mako-cache-milestone1-acceptance.md) and [1,260-sample JSON](benchmarks/mako-cache-milestone1-zoo-002.json), SHA-256 `b0298c614fad1bcb8cafd5df60a61ea500c8e95b616d5646b97baa5c843111e0`. PASS is evidence/correctness acceptance, not a performance SLA; the record highlights poor concurrent-write scaling. |

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
CMake target does not exist. It runs the complete required-native local and
cache Rust suites, the C11/C++ probes, and the native ABI test, including all
five local cleanup failpoints, four end-to-end cache cleanup/quarantine cases,
all sixteen cache write-path crash points, and deterministic midpoint-copy,
locked-scan, and replay-comparator interleavings. Cleanup-failure tests
deliberately retain quarantined uncertain state, so this profile is not
combined with the strict hook-off LSan baseline. The aggregate hook target also
runs the mandatory Phase 1F mutation gate after the ordinary cache suite.

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

The production adapter is separately covered by
`crates/mako-local/tests/fixed_worker.rs`. It proves that submitted closures
reuse the configured long-lived workers, make transactional progress through
the explicit conflict-only retry helper, and retire a sacrificial worker after
native cleanup quarantine without routing later work to it. The adapter's
private queues are bounded, and dropping its awaitable `Task` never cancels
accepted native work mid-transaction. Its fresh-process configuration ceiling
is 459 workers because the thread that opened `LocalDb` has already consumed
one of the 460 slots; native startup can still fail earlier when the process
has prior attachments.

`MakoLocalThreadLimitProbe` is process-isolated because attachment IDs are
never recycled. It creates and joins exactly `MAKO_LOCAL_MAX_WORKERS == 460`
workers sequentially, attaches twice on each, then verifies that a 461st
distinct worker returns `THREAD_LIMIT`. This distinguishes the lifetime budget
from a simultaneous-thread ceiling.

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

## Phase 1F cache correctness gate

The cache cleanup matrix keeps the native and safe-wrapper surfaces distinct.
The raw native ABI exercises all five injected cleanup boundaries: begin,
operation, commit, abort, and destroy. Four end-to-end cache scenarios cover
the safe paths that can retain uncertain native state: preparation-error drop,
explicit abort, active-transaction drop, and ambiguous commit. Every scenario
runs on a fresh OS worker and requires the process-wide quarantine count to
increase by exactly one. There is intentionally no fifth cache `Destroy`
scenario: safe Rust aborts an active transaction before destroy, while a
terminal commit makes the facade inactive before destroy. The raw ABI test is
the executable coverage for that lower-level seam.

Fresh-process crash coverage has ten production-default write-path points and
all sixteen points in the hook-enabled profile. Eight recovery/replay points
are interrupted on two consecutive fresh-process restarts. These tests prove
whole-record cache publication and recovery behavior around the native and
black-box RocksDB calls; they do not interrupt RocksDB internals or claim an
unflushed tail survives.

The `mako-history` application checker always runs the transaction oracle
first. It then requires one global logical clock, constrains serialization by
dense `CacheSeq` order, derives canonical final same-key/RYW mutations, and
checks exact backend batches, retries, frontiers, visible/backend states,
wait barriers, and pinned unknown suffixes. Real cache histories exercise a
three-transaction asynchronous backlog/reopen path and concurrent disjoint
commits whose wrapper responses are deliberately reversed at a Rust-side
post-native, pre-publication seam. The backend transcript is decoded through
the production record reader and compared independently with the actual
`BlobOp` batch. Deliberate decoded-batch divergence must turn the same checker
path red; partial materialization is rejected earlier by transcript decoding.

The hook gate also proves that detached bind performs exactly zero allocations
or frees while a live allocator tripwire detects an injected allocation, and
that the exact native `MakoTimestamp` reaches both the persisted record and the
applied frontier. Recovery records the exact replay sequence and requires
`[1, 2, 3]` before exposure.

The mutation runner is deliberately outside the production crate path. It
copies the crate and lockfile into a fresh workspace for the baseline and each
mutant, rewrites path dependencies explicitly, starts with a distinct absent
Cargo target directory, compiles before testing, and accepts a kill only from
one designated exact failure signature. Its twelve contracts cover stale
journal writeback, early capacity discharge, hook allocation, cancellation
gaps, missing/premature Ready, an unpinned unknown, partial/reordered/duplicate
replay, a wrong Mako timestamp, and a missing recovery clock floor. It verifies
that the original source hash tree is unchanged and removes every temporary
workspace before returning success.

Throughout this gate, *applied* means that the ordered atomic RocksDB batch
returned successfully and the process-local watermark then advanced. It does
not mean the RocksDB WAL was synchronized. The cache never adds a WAL flush,
memtable flush, or `fsync` to this contract.

The loss wording follows that distinction. A clean cache/process shutdown
drains every acknowledged transaction. A forced cache/process stop can lose
the acknowledged but unapplied in-memory tail while preserving the already
applied backend prefix. A machine or power failure is stronger: with the
production `Wal` mode's `sync=false`, it may also lose an applied but unsynced
RocksDB WAL tail. Neither the applied watermark nor the Phase 1F crash matrix
claims that such a tail is durable.

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

## Item 5 (Phase 1F) validation record

Item 5 was accepted from implementation commit
`5546062af6955ce4b7928ade1da00e16c803580c`.

Validation date and host: 2026-08-25, `zoo-005`, Linux
`7.0.14-5-pve` x86_64

Toolchain: Rust/Cargo 1.97.1. Native hook build identity:
`46f0504d12b24a492aacf486ff98b54433688bdd439db0aa7338af4ab3f3a2df`.

| Gate | Reproducible command | Result | Evidence |
| --- | --- | --- | --- |
| Application-aware history oracle | `cd crates && cargo test --locked -p mako-history --all-targets` | `PASS` | 23 application histories and 12 base transaction-oracle histories; global tick uniqueness, cache-order constraints, retries/frontiers/waits, pinned suffixes, and deterministic negative replay all passed. |
| Required-native cache suite | `cmake --build build_item4_hooks --target run_mako_cache_mutation_tests -j 64` | `PASS` | Mandatory dependency baseline: 53 unit tests, 1 cleanup integration test with four fresh-worker scenarios, 1 allocation probe, 7 native integration tests, 4 RocksDB tests, 1 timestamp probe, and 1 documentation test. Hook crash coverage was 16/16 write points and eight recovery points interrupted twice; the production-default matrix was 10/10. |
| Native cleanup and ABI probes | `build_item4_hooks/test_mako_local_abi`; `ctest --test-dir build_item4_hooks -R '^(MakoLocalAbiTests|MakoLocalAbiC11Probe|MakoLocalAbiCppConformance)$' --output-on-failure`; `cmake --build build_item4_hooks --target check_mako_local_abi_symbols` | `PASS` | Native ABI 48/48, including all five raw cleanup seams; CTest 3/3 and the exact 32-symbol allowlist passed. Cache coverage remains the four safe-wrapper scenarios described above. |
| Strict isolated mutation gate | Same mandatory CMake target above | `PASS` | 12/12 killed, 0 survivors, 0 harness errors; baseline green; before/after source digest `baeeaf685089e3cb20607ecca5ca2015db4c5924374dc719c00ae33a5520eb61`; zero retained temporary workspaces. Report: `build_item4_hooks/mako-cache-phase1f-mutations.json`, 190,455 bytes, 2,107.55 seconds, `sha256:93f253d50f7666b895f262d953729575759e4d1e706e5c099ded7ac38f1c7590`. |
| Formatting and strict lint | `cd crates && cargo fmt -p mako-cache -p mako-history -- --check`; native environment plus `cargo clippy --locked -p mako-cache --features test-support --all-targets -- -D warnings`; `cargo clippy --locked -p mako-history --all-targets -- -D warnings` | `PASS` | No formatting drift, warnings, or diff whitespace errors. Independent final review reported no remaining actionable finding. |

This all-green record completes Item 5 and Phase 1F for the current
single-machine, asynchronously applied contract. The subsequent Phase 1B-1E
closure work is summarized at the top of this document. The final zoo-2
comparative row above completes Milestone 1 acceptance. No current phase adds
a disk-sync guarantee, recovers an unflushed in-memory tail, or adds
distributed routing, 2PC, or replication.
