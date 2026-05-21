# Marshal perf baseline — chunk-linked-list implementation

This file records the baseline for the existing `rrr::Marshal` (chunk-
linked-list with raw `char*` arithmetic, 51 per-method `// @unsafe`
overrides) before the planned `Cursor<rusty::Vec<uint8_t>>` rewrite.
Any future Marshal implementation must be compared against these
numbers using the same `bench_marshal` harness.

## Harness

- Source: `src/rrr/tests/bench_marshal.cc`
- Build:  `cmake --build build_clang21 --target bench_marshal -j32`
- Run:    `./build_clang21/bench_marshal`
- Method: each scenario runs a warmup pass (1% of iters or 1024,
  whichever is larger), then a wall-clock-timed pass via
  `std::chrono::steady_clock`. Reports total nanoseconds, ns/op, and
  ops/sec for each scenario.
- No CPU pinning, no governor lockdown — these are dev-box numbers
  meant for relative comparison, not absolute publication.

## Baseline (chunk-linked-list)

Captured 2026-05-21 on a dev box; column meanings:

| scenario                                              | iters     | total_ns      | ns/op     | ops/sec      |
|-------------------------------------------------------|----------:|--------------:|----------:|-------------:|
| write+read i64 (fresh Marshal each pair)              |  2,000,000|    191,447,200|     95.72 |   10,446,745 |
| write+read i64 (single Marshal, drains immediately)   |  5,000,000|    106,455,834|     21.29 |   46,967,835 |
| write 1024 i64 then read 1024 i64                     |     50,000|  1,059,721,353|  21,194.43|       47,182 |
| raw write(8) + read(8) (single Marshal)               |  5,000,000|    103,517,363|     20.70 |   48,301,076 |
| write 1KB blob + read 1KB blob                        |    200,000|     34,442,292|    172.21 |    5,806,814 |
| write+read std::string(100)                           |  1,000,000|    179,220,066|    179.22 |    5,579,732 |
| 4*i32 + string(100) round-trip                        |    500,000|    134,881,051|    269.76 |    3,706,970 |
| write 4KB blob (single write) + read 4KB              |    100,000|     30,733,012|    307.33 |    3,253,830 |
| write 10x 1KB then drain 10x 1KB                      |     50,000|    330,955,484|   6,619.11|      151,078 |

### Reading the numbers

- **i64 steady state ≈ 21 ns/op** — the dominant per-byte cost of the
  chunk linked list is small once a chunk is hot. `raw write(8)+read(8)`
  matches `operator<< / operator>>` for i64 (20.70 vs 21.29), so the
  operator-overload wrapper is essentially free.
- **Per-Marshal ctor+dtor ≈ 70 ns** — the gap between
  `fresh Marshal each pair` (95.72) and `single Marshal` (21.29) is
  the cost of allocating the first chunk + tearing it down. A
  `Vec<uint8_t>` with capacity reservation should match or beat this.
- **1 KB blob ≈ 5.95 GB/s; 4 KB blob ≈ 13 GB/s** — `memcpy` dominates
  the wide writes, which suggests the chunk-walk overhead is small
  for contiguous transfers.
- **10×1KB-then-drain at 661 ns/KB (1.5 GB/s)** — the slowdown vs the
  single 1KB pattern (5.95 GB/s) is the chunk-walk overhead in drain
  mode (the read side has to step through multiple chunks). This is
  the scenario most exposed to the rewrite — a `Vec<uint8_t>` with
  `Cursor` walks contiguous memory and should improve here.

## Comparison budget for the Cursor rewrite

The rewrite is a go if:
- Steady-state i64 op ≤ 25 ns/op (≤ +20% vs 21.29).
- 1 KB blob throughput stays ≥ 5 GB/s.
- 10×1KB drain pattern doesn't regress beyond 7 µs (~5% headroom).
- Per-Marshal ctor+dtor stays ≤ 100 ns (≤ +5% vs 95.72).

The rewrite is rejected (fall back to file-level submodule quarantine)
if any of:
- Any scenario regresses by >25% in ns/op.
- 10×1KB pattern doubles (would indicate that realloc-on-grow ate the
  win from contiguous memory).

These thresholds are stated up front so the comparison in Marshal-4
is a yes/no, not a judgment call.
