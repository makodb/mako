# Final production-source paired benchmarks

Completed 2026-09-13 on zoo-002. All 60 foreground cells and all 72 main
sample/recovery cells passed. No private optimization arm is included.
These bounded measurements do not establish performance acceptance or a
default-engine cutover. C ABI remains the default.

The final source includes record-local OCC versions, bounded atomic values,
pooled wrapper logs, restricted-write HLC prefetch, the bounded read-only
outcome fence, and the composite native/core health diagnostic. It excludes
the rejected borrowed-value and direct-mapped resolved-key cache experiments.

Each table reports the median of three fresh processes per engine. Percent
differences divide the STO median by the C ABI median. The JSON also retains
minimum and maximum observations; three repetitions are not a confidence interval.

## Foreground one-put results

Each worker uses 256 disjoint 8-byte keys and 128-byte values. Warmup is
65,536 commits and measurement is 262,144 commits per worker. CRC is off.
The ACK queue holds all admitted writes, 524,288 records per worker.
Value/count verification is outside worker timing; the unapplied ACK tail
is discarded afterward. These ACK numbers are not drained storage rates.

Workers are individually pinned to physical CPUs 0 through W-1, writeback
to CPU 32, and helpers to CPUs 33 through 63. All 60 PMU samples have
minimum running ratio 1.00000000.

### Engine facade

| Workers | C ABI Mtx/s | STO Mtx/s | STO difference | C ABI cycles/commit | STO cycles/commit |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.913 | 1.112 | -41.9% | 1037 | 1788 |
| 4 | 7.486 | 4.117 | -45.0% | 1054 | 1914 |
| 8 | 14.623 | 7.607 | -48.0% | 1075 | 2031 |
| 16 | 29.374 | 15.179 | -48.3% | 1068 | 2021 |
| 32 | 57.204 | 28.066 | -50.9% | 1076 | 2165 |

### Cache foreground ACK

| Workers | C ABI Mtx/s | STO Mtx/s | STO difference | C ABI cycles/commit | STO cycles/commit |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.159 | 0.732 | -36.8% | 1426 | 2454 |
| 4 | 4.488 | 2.799 | -37.6% | 1455 | 2534 |
| 8 | 8.123 | 5.440 | -33.0% | 1607 | 2624 |
| 16 | 14.640 | 10.200 | -30.3% | 1813 | 2763 |
| 32 | 20.142 | 16.869 | -16.2% | 2794 | 3362 |

## Main cache workloads with drain and fresh recovery

The unchanged main-v2 protocol uses CRC32C, Concurrent foreground mode and
per-worker engine ordering. Each queue has 262,144 divided by transaction
size record slots. This is not the earlier sustained-soak policy of 1,024
records per worker. RocksDB WAL is enabled and sync is false.

Read measurement/warmup counts are 262,144/65,536 commits per worker.
Write and RMW counts are 65,536/16,384 divided by transaction size.
Seed and warmup writes drain before measurement. Measured workers are
fresh threads, so attachment and cold scratch costs remain in this interval,
along with selected-key allocation, latency collection and joins.

Workers and unpinned child helpers share CPUs 0 through W-1; writeback alone
is pinned to CPU 32. The launcher stays on CPUs 33 through 63. Every cell
fully drained, then passed exact per-key value and checksum checks in a
fresh recovery process. There were 144 successful sample/recovery phases.

| Workload | Keys/tx | Contention | Workers | C ABI ktx/s | STO ktx/s | STO difference |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| read | 1 | low | 1 | 850.7 | 704.5 | -17.2% |
| read | 1 | low | 4 | 2234.1 | 1721.3 | -23.0% |
| read | 1 | low | 32 | 5184.1 | 3253.1 | -37.2% |
| read | 4 | low | 1 | 385.9 | 331.0 | -14.2% |
| read | 4 | low | 4 | 1307.0 | 1144.4 | -12.4% |
| write | 4 | low | 1 | 224.5 | 129.3 | -42.4% |
| write | 4 | low | 4 | 487.8 | 307.0 | -37.1% |
| rmw | 1 | low | 1 | 371.1 | 230.1 | -38.0% |
| rmw | 1 | low | 4 | 860.2 | 530.1 | -38.4% |
| rmw | 4 | low | 1 | 147.8 | 98.5 | -33.3% |
| rmw | 4 | low | 4 | 380.1 | 260.0 | -31.6% |
| rmw | 4 | high | 4 | 143.1 | 95.7 | -33.1% |

Low-contention measured conflicts across both engines: 0.

High-contention RMW c-abi: median 260,862 conflicts for 65,536 measured commits; median logical p99 85.4 microseconds.
High-contention RMW sto: median 324,748 conflicts for 65,536 measured commits; median logical p99 171.4 microseconds.

## Provenance and limits

Rust 1.95.0, commit 59807616e1fa2540724bfbac14d7976d7e4a3860,
non-LTO release, opt-level 3, one codegen unit, panic abort, znver1,
frame pointers enabled, debuginfo 1. Both builds used the unchanged native
production archives, with no fake ABI or test hooks.

Boost remained disabled and all 64 governors stayed schedutil. The final60
and main72 ran sequentially under the same cooperative performance lock.
The supervisor checked all four binary hashes and loaded-library hashes
before and after both stages. Main additionally checks its binary before
and after every sample and recovery phase.

All 256 source files and 21 native/link inputs match the final build snapshot.
The source archive and complete native build manifest are retained here.
Native fingerprint: 9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea.

| Engine | Binary | SHA256 |
| --- | --- | --- |
| c-abi | engine_compare | 7d3f90c5b3e77a101211c2c35e2925ed55cc9c9aafb85c7a98e01e817a98cd78 |
| c-abi | mako-cache-bench | ef5fe4ea3b99cdaf0f2ae0c87eac2d735190ed3a35ada48f8523439e96f15059 |
| sto | engine_compare | 2ba98a1ce78bf2997468e06b8bf5c9432f66b6ec389a99407b724e60ac1eedc7 |
| sto | mako-cache-bench | 7d66ab5fa6d69afa8f344017fe5d2754ee4212ea204d5bdd0fe1a8628fa44ec3 |

Immutable executables and original raw results remain on zoo-002 under
`/var/tmp/mako-engine-perf-20260912.iz63eX`, with local build copies under
`/var/tmp/mako-engine-perf-20260912.trsFuD`. The retained directory includes
both protocols, every cell result, stdout/stderr, machine snapshots and
main process-attempt memory samples. `final-results-summary.json` contains
the complete medians/ranges and the audit input hashes.

No new sustained GC soak was run on this final source. Earlier 960-second
soak evidence belongs to its recorded older source and remains separate.
The sizeable engine, write and RMW gaps remain. No acceptance budget was
supplied, and this run does not justify changing the default engine.
