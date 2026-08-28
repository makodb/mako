# Optimized Rust STO/Masstree comparison on `zoo-002`

Date: 2026-08-28<br>
Implementation: `codex/sto-rust` at
`6936abb30000d45c310226ceee0f6f7796b611ad`

## Outcome

The global-lock scalability failure is fixed. Across the 21 paired
scenario/thread cells, the median paired Rust/C++ throughput ratio has an
unweighted geometric mean of **86.14%** and ranges from **74.86%** to
**140.99%**. Rust is about 20–25% behind at low thread counts, narrows the gap
through 16–32 cores, and is 1.06–1.41x faster in all three 64-core workloads.

For historical context, the same runner and workload shape on 2026-08-27 had a
3.33% Rust/C++ cell-geometric mean and ranged from 0.42% to 14.21%. Dividing
the final Rust medians by those archived Rust medians gives 5.59–265.67x, with
5.59–5.78x at one thread. These are not implementation-only speedups or a
contemporaneous paired A/B: both harnesses changed worker initialization from
strongly correlated shifted SplitMix states to independently scrambled starting
states, the warmup changed from two seconds to one, and Rust changed from a
non-LTO/16-codegen-unit build to fat LTO/one codegen unit. The main tables below
are the controlled result: freshly rebuilt current C++ and Rust binaries use
the same corrected streams and run as adjacent pairs.

This meets the development goal of roughly comparable committed-operation
throughput for this bounded point workload. It is not uniform parity or a
production-wide acceptance result.

Values below are medians of three five-second samples. `Mops/s` counts logical
operations in committed transactions, including the cost of aborted attempts
and retries. `Rust/C++` is the median of the three same-seed, per-repetition
throughput ratios, not a ratio formed from independently selected medians.

### Ten reads per transaction

| Threads | C++ Mops/s | Rust Mops/s | Rust/C++ | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 6.046 | 4.828 | 79.85% | 0.000% | 0.000% |
| 2 | 12.020 | 9.651 | 80.29% | 0.000% | 0.000% |
| 4 | 23.993 | 19.245 | 80.25% | 0.000% | 0.000% |
| 8 | 46.349 | 38.575 | 83.42% | 0.000% | 0.000% |
| 16 | 90.761 | 76.423 | 84.14% | 0.000% | 0.000% |
| 32 | 170.180 | 148.360 | 87.12% | 0.000% | 0.000% |
| 64 | 222.133 | 245.863 | 110.55% | 0.000% | 0.000% |

### Ten operations, 5% writes

| Threads | C++ Mops/s | Rust Mops/s | Rust/C++ | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 5.666 | 4.421 | 78.02% | 0.000% | 0.000% |
| 2 | 11.272 | 8.804 | 78.18% | 0.002% | 0.001% |
| 4 | 22.466 | 17.550 | 78.19% | 0.008% | 0.003% |
| 8 | 41.817 | 32.769 | 78.33% | 0.021% | 0.012% |
| 16 | 77.394 | 66.411 | 85.88% | 0.051% | 0.024% |
| 32 | 121.259 | 126.924 | 104.72% | 0.275% | 0.062% |
| 64 | 143.702 | 202.601 | 140.99% | 3.169% | 0.165% |

### Ten operations, 50% writes

| Threads | C++ Mops/s | Rust Mops/s | Rust/C++ | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.950 | 3.104 | 78.58% | 0.000% | 0.000% |
| 2 | 7.880 | 6.158 | 78.15% | 0.027% | 0.018% |
| 4 | 15.776 | 12.262 | 77.91% | 0.077% | 0.056% |
| 8 | 26.767 | 20.013 | 74.86% | 0.268% | 0.185% |
| 16 | 48.604 | 39.040 | 80.32% | 0.459% | 0.406% |
| 32 | 83.491 | 72.033 | 86.54% | 1.390% | 0.853% |
| 64 | 106.106 | 112.722 | 106.26% | 12.411% | 2.273% |

## The original cause and the fix

The [initial zoo-2 profile](../sto-rust-zoo2-2026-08-27/README.md) found that
73.11% of a 32-thread Rust read profile was in the kernel queued-spinlock slow
path. Each key used process-wide C-ABI handle mutexes, a tree-wide structural
shared mutex, a Rust resource-binding mutex, and a registry `RwLock`. It also
crossed the ABI and paid allocation, hashing, boxing, reference-counting, and a
fully general STO item lifecycle separately for every point. That explains
both symptoms: a roughly 7x one-thread deficit and a post-four-thread collapse
that grew to a 238x read-only gap at 64 threads.

The accepted implementation removes those bottlenecks without weakening the
failure protocol:

- append-only atomic handle registries, TLS worker validation, and
  per-worker/native-core-ID structural-reader publication remove lifecycle and
  shared-tree mutexes from steady reads;
- an explicit worker-affine native read scope amortizes repeated scalar point
  reads; the timed fixed-batch path instead uses `mt_get_strided` to pay
  validation, structural admission, RCU, and the ABI crossing once per batch;
- segmented stable registry slots remove the registry `RwLock`; an explicit
  bounded eager-contiguous layout gives preloaded workloads direct `RecordId`
  indexing, while the public default remains lazy and segmented;
- values of at most eight bytes stay inline in each record slot;
- homogeneous unique-item batches, reused item/batch storage, borrowed lock
  acquisition, fused visitors, and lazy uniqueness checks avoid the generic
  per-key hash/box path;
- read-only records use a proven preflight-free validation lane and omit the
  adapter committed-finish callback when core teardown is sufficient; and
- a persistent per-worker lock-plan nonce prevents stale plan-token matches
  when worker IDs are reused, removing the high-thread mixed-workload cliff.

Every material candidate was gated against the prior candidate with matched
same-seed pairs. Regressions—including smaller registry packing, compact read
items, a cold record sidecar, metadata caching, and several over-specialized
helpers—were reverted instead of being accumulated into the result.

## What remains at low thread counts

A near-final one-thread read profile (the `opt69` binary, before the last two
small gated improvements) measured about 7,024 cycles per Rust transaction and
5,521 per C++ transaction. Normalizing the sampled `reach_leaf` share gives
about 2,508 and 2,516 cycles per transaction respectively, so Masstree traversal
itself is not the residual gap. `perf` reported about 39.5% more L1 data-load
events per Rust transaction and 40.8 versus 9.7 data-TLB load events per
transaction. The roughly 1,500-cycle total delta is work outside the sampled
`reach_leaf`; source and profile evidence are consistent with the general
`RecordId` registry, typed item lifecycle, commit protocol, and other harness
work around the lookup, but this is not a component-by-component attribution.

A dedicated fixed-copy `u64` table and compact typed lock lane could reduce
that residual, but would be a second specialized data structure rather than a
safe optimization of the general binary-value abstraction. It is deliberately
left as a separately reviewed future experiment. The profile inputs and
reports are archived under [`profile-1t-read`](profile-1t-read/).

## Method

Both harnesses execute the same logical stream:

- 100,000 prepopulated eight-byte big-endian keys and logical `u64` values;
- ten unique keys per transaction;
- 0%, 5%, or 50% writes, with each write reading then incrementing the value;
- retry of the same materialized transaction until commit;
- 1, 2, 4, 8, 16, 32, or 64 worker threads;
- a one-second warmup followed by a five-second measurement;
- seeds 1, 2, and 3 over three deterministically shuffled repetitions; and
- adjacent C++/Rust pairs, with the first engine alternating by shuffled
  schedule position inside each repetition.

Each process was restricted with `taskset` to the first `N` CPUs from 0–63.
Those IDs are one hardware thread from each physical core; SMT siblings 64–127
were excluded. Threads could migrate within the selected set.

This is an end-to-end comparison of two integrations, not an isolated STO
measurement or a Rust-versus-C++ language benchmark. Both engines execute the
same logical stream, but not the same physical work. C++ MassTrans stores a
`versioned_value_struct<uint64_t>*` directly in Masstree, while Rust crosses
the stable C ABI for a `RecordId`, resolves an eagerly allocated Rust registry
slot, and executes the typed Rust item protocol. The Rust benchmark explicitly
opts into `RegistryLayout::EagerContiguous` for the fully preloaded 100,000-key
space; the public `LazySegmented` default is not represented.

Conversely, C++ was built with `READ_MY_WRITES=OFF`, and every measured write
performs `transGet` followed by `transPut`; the latter traverses Masstree again.
Keys are unique within a transaction, so disabled read-your-writes does not
change this workload's abstract result, but the extra write traversal means the
comparison is not uniformly biased in favor of C++. Throughput counts committed
logical operations and includes retry cost, so differing abort rates—especially
at high thread counts—are part of the end-to-end result and must not be read as
per-attempt core speed.

Warmup writes remain in each table, so the two engines can enter the timed mixed
run with different numeric values. Values remain fixed-width and only
increment, making this expected to be throughput-neutral, but cross-engine
checksums are intentionally not compared.

Both timed code paths use effective `-O2`, debug information, native CPU code,
and frame pointers. Rust additionally uses fat LTO and one codegen unit; the
C++ repository target does not use LTO. Both use glibc allocation. Exact
compiler commands, linkages, build IDs, and hashes are under [`build`](build/)
and summarized in [`environment.json`](environment.json).

## Audit and confirmation

[`audit.py`](audit.py) independently verifies all 126 rows, the exact schema,
63 complete adjacent pairs, schedule-position engine alternation, seeds, CPU
affinities, elapsed-time bounds, accounting identities, recomputed rates, and
the 42-row summary. Twenty of 21 paired cells stayed below both gates: 3%
per-engine CV and 5% paired-ratio spread.

Read-only/64-thread had a 5.28% paired-ratio spread and therefore received the
predeclared separate five-pair confirmation:

| Run | C++ Mops/s | Rust Mops/s | Rust/C++ | C++ CV | Rust CV |
| --- | ---: | ---: | ---: | ---: | ---: |
| Main, 3 pairs | 222.133 | 245.863 | 110.55% | 2.87% | 0.38% |
| Confirmation, 5 pairs | 224.672 | 245.454 | 109.47% | 4.01% | 0.38% |

The confirmation reproduces the central ratio but also confirms that C++ is
the noisy side of this saturated cell: its five values span 220.0–241.5 Mops/s,
while Rust spans 243.9–246.3 Mops/s. All samples are retained under
[`confirmation-read10-64`](confirmation-read10-64/); none was discarded.
It ran immediately after the main matrix, from approximately 13:11:03 to
13:12:04 UTC, but the main run's continuous host monitor had already stopped
and no separate confirmation monitor was collected. Its environmental noise
therefore cannot be diagnosed beyond the paired samples themselves. It
corroborates the direction and central estimate, but its 4.01% C++ CV and
10.48% paired spread do not clear the predeclared noise gates; the cell remains
flagged.

Before the timed run, the exact native build passed the C11 header and
41-symbol ABI tests. Separately compiled debug test binaries against that same
source/native build passed four safe Masstree and six transactional Masstree
integration tests; the smoke run exercised the exact release benchmark
binaries. The implementation commit had also passed workspace formatting,
all-target tests, strict Clippy, and warning-free rustdoc. Sanitizers were not
rerun as part of this performance archive and this run is not evidence for the
production cutover sanitizer gate.

## Host caveats and unmeasured work

`zoo-002` is a shared, non-isolated host with `schedutil` and boost enabled.
The accepted preflight window was 99.55% idle overall. An LXD service was in a
known restart loop and briefly used part of one core roughly once a minute;
stopping or repinning a shared service was intentionally avoided. Adjacent
paired runs, randomized cell order, schedule-position alternation, three
repetitions, continuous
[`pidstat`](monitor/pidstat.txt)/[`vmstat`](monitor/vmstat.txt), and the
targeted confirmation mitigate or characterize but do not eliminate that
noise.

The run does not measure latency percentiles, scans, misses and tombstone
interning, non-eight-byte values, allocation counts, lock-hold time, precise
range-witness false conflicts, or application traces. Three five-second
samples on one host and one keyspace are an engineering gate, not a
publication-grade performance study or a production capacity result.

## Artifacts

- [`raw.jsonl`](raw.jsonl): all 126 main-matrix samples and exact commands
- [`summary.csv`](summary.csv): per-engine medians, minima, maxima, and aborts
- [`run.json`](run.json): runner configuration and schedule inputs
- [`run-command.txt`](run-command.txt): the fully expanded runner invocation
- [`audit.txt`](audit.txt): paired ratios, variation, and audit outcome
- [`confirmation-read10-64`](confirmation-read10-64/): all ten confirmation
  samples and their audit
- [`environment.json`](environment.json): source, host, build, ABI, and hashes
- [`build`](build/): exact compile command, linkage, build IDs, and test logs
- [`idle`](idle/), [`monitor`](monitor/), and [`smoke`](smoke/): host gate,
  continuous run monitoring, and pre-run smoke results
- [`SHA256SUMS`](SHA256SUMS): checksums for every other archived artifact
