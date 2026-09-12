# Rust STO versus C++ STO: Mako TPC-C comparison

This is the first application-level comparison of the Rust STO implementation
against Mako's native C++ STO/Masstree path. The result is clear: the Rust path
is functionally running the same workload, but it is not yet performance
competitive. It delivers about 27% of C++ throughput at one thread and 9% at
16 threads. The nearly identical, very low abort rates show that retries do not
explain the gap; the Rust path has both substantial per-transaction overhead
and a scaling bottleneck.

This is **not an official TPC-C or tpmC result**. It uses Mako's STO-derived
TPC-C application, schema, loader, transaction implementations, and
high-throughput execution model. It does not implement the audited TPC-C
terminal, keying-time, think-time, response-time, durability, or pricing
requirements. It is also not a byte-for-byte rerun of the EuroSys STO artifact.
The numbers below are committed transactions per second from this Mako
workload and must not be labeled tpmC.

## Result

| Workers / warehouses | C++ STO txn/s | Rust STO txn/s | Rust as % of C++ | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 86,156 | 23,213 | 26.96% | 0.0000% | 0.0000% |
| 4 | 336,577 | 72,486 | 21.46% | 0.0152% | 0.0136% |
| 8 | 668,871 | 101,424 | 15.17% | 0.0149% | 0.0149% |
| 16 | 1,292,746 | 114,509 | 8.82% | 0.0159% | 0.0179% |

Each throughput column is the median of three runs. “Rust as % of C++” is the
median of the three adjacent, paired Rust/C++ ratios rather than a ratio formed
from two independently selected medians. Across the three pairs, the ratio
spread was 0.98 percentage points at one thread and at most 0.35 points for the
other cells.

The C++ engine scales from 86.2k txn/s at one worker to 1.29M txn/s at 16
workers, a 15.0x increase. Rust reaches 114.5k txn/s at 16 workers, only 4.9x
its one-worker result, and gains little from 8 to 16 workers. That flattening is
the most important result to address next.

## What was compared

Both modes use the same `sto_tpcc_bench` executable and therefore the same C++
TPC-C driver, record encoders, schema, loader, random workload selection, and
result accounting. The command-line engine switch changes only the
`abstract_db` implementation:

- `cpp` uses Mako's established `mbta_wrapper`, native C++ STO, and Masstree.
- `rust` uses the Rust STO runtime and transactional table implementation. It
  reaches the same native Masstree implementation through the C ABI adapter.

The configured transaction mix is NewOrder 45%, Payment 43%, Delivery 4%,
OrderStatus 4%, and StockLevel 4%. Every cell uses one non-replicated local
shard, one worker per warehouse, a fresh database per process, and the current
Mako “separate tree per warehouse” mode. Network replication and durable log
I/O are outside this comparison.

The 24-worker cell was intentionally omitted. Separate-tree mode opens 11
per-warehouse tables plus one shared item table, while the existing C++ wrapper
reserves 200 table IDs per shard. Eighteen warehouses require 199 IDs and are
the largest valid paired configuration; 24 would require 265. The runner caps
the comparison at 18 rather than changing Mako's global table-ID spacing and
potentially changing unrelated system behavior.

## Method

The run was made on `zoo-002`, an AMD EPYC 7702P host with 64 physical cores,
128 logical CPUs, one NUMA node, and 256 MiB of L3 cache. Cells used CPU IDs
0–15, so the 16-worker cell stayed on distinct physical cores. Both sides were
built at `-O2`/equivalent with native-CPU code generation, debug information,
and frame pointers; the Rust static library additionally used one codegen unit
and fat LTO.

For each of three repetitions, the runner shuffled the 1/4/8/16-worker cells,
ran the two engines adjacently, and alternated which engine ran first. Each
requested measurement interval was three seconds; observed intervals ranged
from 3.035 to 3.055 seconds. Database loading and the benchmark's deliberate
shutdown sleep are excluded from reported throughput.

The host had a known LXD daemon restart loop, so each pair was aligned just
after a restart. Before starting it, the runner required a two-second window
with an unchanged LXD restart count, at least 95% idle time on every selected
CPU, and no recognized competing STO build, benchmark, or `perf` process. It
then rejected both results if the restart count changed or a competitor was
present after the pair. All 12 final pairs passed on their first attempt; none
was retried or discarded.

## Why the Rust path is slower

An indicative one-thread `perf` run made while developing this benchmark did
not identify one dominant function. It showed overhead distributed across the
Rust transaction layer, native-tree crossings, and value handling. In
particular:

- A fresh insert currently performs a directory lookup and then enters the
  atomic native `get_or_insert` path, causing two Masstree traversals for a
  common NewOrder operation. `mt_get_or_insert` alone accounted for about 5%
  of the sampled one-thread profile.
- Generic STO bookkeeping remains visible on every record: preflight,
  admission/epoch tracking, `ArcSwap` accounting, commit, and lock-plan
  teardown all appeared separately in the profile. These are small in
  isolation but accumulate across a multi-record TPC-C transaction.
- Point reads and scans currently materialize owned Rust values and then copy
  them again into C++ strings across the ABI. The `sto_tpcc_get` boundary and
  string/allocation work were measurable hot paths.
- Bounded scans now stop after the caller's logical limit, which fixed an
  earlier whole-range pathology, but they still build a Rust result vector and
  clone row keys/values before invoking the C++ callback.
- The 8-to-16-worker plateau, despite negligible abort rates and separate
  per-warehouse trees, suggests shared runtime coordination or transaction
  bookkeeping contention. The one-thread profile alone cannot localize that
  scalability problem, so this last point is an inference from the scaling
  curve rather than a proven root cause.

These costs also explain why the earlier point-operation microbenchmark was
much closer to C++: TPC-C exercises multi-record transactions, inserts, range
scans, application value encoding, and many ABI crossings in every commit.

## Next optimization priorities

1. Add a safe “insert after confirmed miss” native primitive so a transactional
   insert does not search Masstree twice. Preserve the current publication and
   capacity-failure guarantees, then measure NewOrder directly.
2. Add presence-only and caller-buffer/borrowed point-read paths. Avoid cloning
   an `Arc<[u8]>`, allocating a temporary value, and copying through an
   intermediate C++ string when the transaction only needs existence or can
   provide its final buffer.
3. Profile at 8 and 16 workers with per-core counters and lock/contention data.
   Focus the Rust core on shared epoch/admission accounting, transaction-item
   membership, preflight, and lock-plan construction; the scaling curve makes
   this higher priority than polishing small FFI call overheads.
4. Stream scans into a reusable callback scratch buffer instead of first
   constructing a `Vec<ScanRecord>`. Retain the new logical-limit behavior and
   phantom/read-set semantics.
5. After each change, rerun paired one-thread and scaling cells. Once the curve
   improves, use at least five repetitions and 30-second intervals on a host
   without the LXD restart issue before treating the numbers as a stable
   performance baseline.

## Reproducibility and audit

The archive contains the [raw records](raw.jsonl), [summary](summary.csv),
[run metadata](run.json), [build environment and hashes](environment.json),
[exact command](run-command.txt), [artifact checksums](SHA256SUMS), and the
[audit program](audit.py). The checked-in [audit output](audit.txt) was
produced with:

```sh
python3 docs/performance/sto-rust-tpcc-zoo2-2026-08-28/audit.py
```

The audit validates all 24 engine samples, pair adjacency and ordering, CPU
affinity, result/mix invariants, guard evidence, throughput arithmetic, and the
archived summary. The remote process could not populate `git_commit` and
`git_status` in `run.json` because Git rejected that root-owned process's view
of the shared worktree as unsafe. `environment.json` therefore records the
implementation commit and the separately captured SHA-256 of the measured
executable. The binary was stored at an ephemeral path and was not copied into
the repository, so the hash verifies a retained copy but cannot recreate one.
The enhanced runner now fingerprints the executable, configuration, and source
revision automatically for future runs.
