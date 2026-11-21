# One-worker frame and direct-dispatch optimizations

The reusable core frame and singleton direct-dispatch changes improve the
Rust cache's foreground write path. The final one-worker before/after pair
measured 0.737 versus 0.885 million ACKs/sec, a 20.1% improvement including
ThinLTO. Four-worker ACK throughput improved from 2.791 to 3.440 million,
or 23.2%. The shared-payload experiment did not help and was removed.
The C ABI remains the default engine. This report does not claim parity.

## Isolating the frame and compiler changes

Millions of committed transactions per second, medians of three fresh
processes on zoo-002. Each transaction updates one 128-byte value. Boost is
disabled throughout.

| Rust variant | Engine only | Cache ACK |
| --- | ---: | ---: |
| Original, no LTO | 1.082 | 0.724 |
| Reusable frame, no LTO | 1.177 | 0.761 |
| Original, ThinLTO | 1.167 | 0.779 |
| Reusable frame, ThinLTO | 1.276 | 0.839 |

With the original compiler settings, the frame change improves ACK throughput
by 5.1%. ThinLTO adds 10.3% to the changed implementation. Together they improve
ACK throughput by 16.0%. ThinLTO is an explicit standalone-build option, not a
change to the workspace-wide release profile.

These measurements precede the direct-dispatch change. The final comparisons
below use fresh pairs instead of multiplying gains from separate experiments.

## Retained changes

`Runtime::attach` allocates one general transaction frame per worker. Begin,
commit, abort and clean Drop transfer its exclusive box without moving the
item pools, index or lock storage. The active transaction handle shrinks from
208 to 16 bytes on this 64-bit build; the frame remains 200 bytes. Begin resets
attempt-local state in place. It adds no transaction-boundary allocation or
shared reference-count operation.

The existing commit driver still owns cleanup. Unknown outcomes quarantine
the full frame, and a poisoned runtime cannot resume simply because some
storage was recycled. The terminal-read protocol remains separate. No OCC,
timestamp, log format, queue, RocksDB or durability policy changed.

The singleton direct terminal now enters the concrete typed batch once.
Its shared phase helper removes the outer per-phase batch dispatch while
retaining the existing inner plan, adapter callbacks and held guards. Larger
direct batches use the same phase helper with erased dispatch. Cold failures,
finish and quarantine remain in the existing commit driver.

The independent review caught an earlier-destruction difference for caught
panic payloads in the first draft. The final helper returns those payloads to
the driver and drops them only after its cleanup handler returns. New tests
check that order, including a payload destructor that also panics.

The five-repetition isolated dispatch comparison measured:

| Rust variant | Engine only | Cache ACK |
| --- | ---: | ---: |
| Frame only, no LTO | 1.181 | 0.763 |
| Frame + dispatch, no LTO | 1.291 | 0.814 |
| Frame only, ThinLTO | 1.296 | 0.835 |
| Frame + dispatch, ThinLTO | 1.389 | 0.892 |

Dispatch improves ACK throughput by 6.6% without LTO and 6.8% with ThinLTO.
Disassembly confirms removal of the outer per-phase batch calls. The
non-LTO binary's text grows by 2,576 bytes.

## Final matched results

Each line is a median of three fresh processes. Final Rust uses both retained
code changes and ThinLTO. The original Rust control uses its original non-LTO
build. The separate C++ comparison enables ThinLTO for both engine binaries.

| ACK comparison | One worker | Four workers |
| --- | ---: | ---: |
| Original Rust | 0.737 | 2.791 |
| Final Rust, same pair | 0.885 | 3.440 |
| C++ with ThinLTO | 1.153 | 4.471 |
| Final Rust, C++ pair | 0.885 | 3.390 |

Rust remains 23.2% slower than C++ at one worker and 24.2% slower at four.
These are engine-facade comparisons, not direct C++ versus direct Rust table
calls. Engine-only before/after results were 1.088 versus 1.414 million at
one worker and 4.186 versus 5.219 million at four. In the C++ pair,
engine-only C++ versus Rust measured 1.824 versus 1.395 million at one
worker and 7.224 versus 5.194 million at four.

A final same-settings non-LTO control measured 0.726 versus 0.821 million
ACKs/sec at one worker, a 13.0% gain from the code changes alone. Its
engine-only results were 1.096 versus 1.276 million. This is separate from
the 20.1% before/after result that also enables ThinLTO.

## Bounded queues and recovery

The final binaries also completed four 20-second checks: each engine at one
and four workers, with 1,024 queue slots per worker, CRC32C, complete drain
and fresh-process reopen. Every acknowledged transaction and all final values
were recovered, and every cache remained healthy.

Fully applied throughput was about 68,000–72,000 transactions/sec in these
short checks. Queues reached their limits, and adding workers did not increase
throughput materially. That is consistent with a writeback bottleneck; it is
not evidence of foreground parity. One sample per cell cannot establish a
small sustained-performance difference. Reopens took 18–20 seconds for
1.36–1.44 million retained log entries. This is not a five-minute GC-age soak
or a test of recovering the unpersisted ACK tail.

## Rejected shared-payload experiment

The prototype stored one immutable reference-counted payload in both the STO
intent and log holder for the first trusted PUT with a 39–128-byte value.
Snapshot and uncertain-cleanup owners kept it alive. Reuse required exclusive
ownership through `Arc::get_mut`; ordinary values retained their fallback.

It passed native and Miri ownership tests, including a sealed but unpublished
holder after uncertain commit cleanup. It nevertheless regressed performance.
In its matched non-LTO run, original versus shared-payload ACK throughput was
0.725 versus 0.713 million. Combining it with the frame change reached 0.750
million, below the frame change alone at 0.759 million. It removed one payload
allocation/copy but added reference-counting, uniqueness checks and wrapper
representation work. The measurement does not isolate their individual costs.

The prototype's API, wrapper representation and dedicated tests were removed
from the worktree. Its frozen source and test evidence remain in the experiment
artifacts. No borrowed-caller lifetime shortcut replaced it.

## Compiler settings and scope

Both engines use Rust 1.95.0, release optimization level 3, one codegen unit,
`target-cpu=znver1`, frame pointers and the same existing native archives.
The ThinLTO comparison changes only the Rust LTO setting. It does not rebuild
the native C++ code with different optimization or enable PGO.

For a standalone benchmark binary, after configuring the required native
and RocksDB library environment documented in the benchmark README:

```bash
CARGO_PROFILE_RELEASE_LTO=thin \
CARGO_TARGET_DIR=/absolute/path/to/a/separate-target \
cargo +1.95.0 build --locked --release --manifest-path crates/Cargo.toml \
  -p mako-cache-bench --no-default-features --features engine-sto \
  --bin engine_compare --bin mako-cache-bench
```

Do not apply this override blindly to the mixed C++/Rust static-library build.
The workspace keeps LTO disabled because its legacy static-library combination
has a known duplicate-standard-library-symbol constraint. PGO remains future
work and needs separate training and held-out workloads; it is not included
in these results.

The foreground protocol uses worker CPUs starting at 0, writeback CPU 32,
helpers on 33–63, jemalloc, and RocksDB 8.9 with WAL enabled and `sync=false`.
Each worker has 256 disjoint keys, 65,536 warmup commits and 262,144 measured
commits. The ACK queue has 524,288 slots per worker and checksum generation is
off. Every sample verifies visible values, committed counts and healthy queue
accounting, then discards its unapplied tail. These are ACK measurements, not
sustained persistence or crash-durability measurements.

All foreground PMU running ratios were 1.0. Measured counters exclude startup,
warmup and verification. Process order alternates between repetitions. CPU
governors are recorded and left unchanged. The initial three campaigns contain
24 cells each: isolated source changes, isolated LTO changes, and matched
C++/Rust one- and four-worker comparisons. The five-repetition dispatch
campaign adds 40 cells. Final original-Rust and C++ control pairs add another
48. The final same-settings non-LTO control adds 12, for 172 foreground cells
in total. There are also eight bounded-queue checks, four on the frame-only
candidate and four on the final candidate.

## Validation and artifacts

The frame change passed 274 core tests and three doctests, strict core Clippy,
six leak-checked frame Miri tests, three existing exact quarantine Miri cases,
and the TPC-C lifetime-erased-owner Miri regression. Its source snapshot also
passes all 35 engine tests and the cache's 206 library, 18 integration and
eight Loom tests. The final frame-plus-dispatch snapshot passes 280 core tests,
three core doctests, strict core Clippy, six new ordinary Miri cases and two
expanded exact quarantine cases. All 35 engine tests, 206 cache library tests,
18 cache integration tests, eight Loom tests and five cache doctests also pass
on that final snapshot. Cache test cases run serially because they share a
process-wide order namespace; individual cases still exercise concurrency.
All nine native-free engine Miri tests pass on the final snapshot with leak
checking enabled.

An initial parallel cache run failed that fixture restriction and is excluded.
An initial snapshot engine build omitted the external differential fixture;
the complete retry includes it and executes the required C++ oracle. Failed
build attempts are retained, not counted as successful qualification.

Scratch artifacts are under `/var/tmp/mako-w1-implementation.VtilJG`, with
remote originals under `/var/tmp/mako-w1-fixes.ShjYqz`. The frame-only patch
and core qualification are under `/var/tmp/mako-worker-frame.3ZpJJA`.
The direct-dispatch patch and core qualification are under
`/var/tmp/mako-direct-terminal.ZkxPmY`.

Fresh final ASan and TSan gates each pass 278 checks with exact inventories.
ASan runs 259 tests with leak checking, 11 exact intentional-quarantine cases
with only leak checking disabled, and eight separate uninstrumented Loom
models. TSan runs 270 instrumented tests and those eight separate models.
Neither model run limits the schedule search. The required LSan negative
control fails as expected and is not counted as a positive test.
Both gates preserve all 260 checked source hashes and 14 native/build-input
hashes. Their reports are in the linked `sanitizers` directory. System RocksDB
is outside this instrumentation; these are not new Rust UBSan results.

The first sanitizer runs passed the tests but failed the wrapper's unchanged-
build-input check because CMake discovered the new Rust files and regenerated
`build.ninja`. They remain under `/var/tmp/mako-w1-final-sanitizers.HJrd7x`.
The accepted clean reruns are under
`/var/tmp/mako-w1-final-sanitizers-stable.8tVqBo`.

The [artifact audit](mako-cache-rust-sto-w1-2026-09-13/audit.json) verifies all
172 foreground records, eight completed bounded-queue checks, binary hashes,
native archives and 263 final source files against the worktree. Existing
source changes from the original snapshot are confined to `sto-core`.
The linked directory retains the exact results and environment records in
`raw.tar.gz`, plus build commands, patches and qualification reports.
Library resolution was inspected
after the first three campaigns; the final candidates' runtime-library hashes
were checked before and after their runs. This is not an initial loaded-library
audit of every earlier exploratory process.
