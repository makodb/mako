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

## Prototype results (Marshal V2, Vec<uint8_t> + read_pos)

Captured 2026-05-21 on the same dev box, side-by-side with the
baseline above, after two perf fixes in `third-party/rusty-cpp`:
  - `Vec::extend_from_slice` / `Vec::write` now memcpy trivially-
    copyable T instead of looping `push()` per element.
  - `Vec::reserve` grows geometrically (`max(new_capacity, 2*capacity)`)
    instead of allocating exactly the requested capacity — a sequence
    of `reserve(size+8)` calls now amortizes to O(N) total rather
    than O(N²).

Without those two fixes, V2 was 6× to 33× slower on burst-write paths.
With them, V2 wins on every scenario.

| scenario                                              | baseline ns/op | V2 ns/op | delta % |
|-------------------------------------------------------|---------------:|---------:|--------:|
| write+read i64 (fresh Marshal each pair)              |          94.89 |    54.85 |  -42%   |
| write+read i64 (single Marshal, drains immediately)   |          21.41 |     9.39 |  -56%   |
| write 1024 i64 then read 1024 i64                     |       21,353   |   9,950  |  -53%   |
| raw write(8) + read(8) (single Marshal)               |          20.69 |     9.59 |  -54%   |
| write 1KB blob + read 1KB blob                        |         149.49 |   107.86 |  -28%   |
| write+read std::string(100)                           |         166.04 |    81.65 |  -51%   |
| 4*i32 + string(100) round-trip                        |         265.38 |   159.52 |  -40%   |
| write 4KB blob (single write) + read 4KB              |         308.55 |   259.52 |  -16%   |
| write 10x 1KB then drain 10x 1KB                      |       6,646.21 | 1,282.70 |  -81%   |

Every go/no-go threshold from the prior section is met by a large
margin:
  - Steady-state i64 ≤ 25 ns/op:  9.39 ns/op ✓
  - 1 KB blob ≥ 5 GB/s:           9.5 GB/s ✓ (was 5.95 GB/s)
  - 10×1KB drain ≤ 7 µs:          1.28 µs ✓
  - Per-Marshal ctor+dtor ≤ 100 ns: 54.85 ns ✓

Decision: **proceed with the Cursor-style rewrite**. The Vec<u8> +
read_pos approach maps cleanly to Marshal's dual-position model, and
the perf is better across the board, not just on the chunk-walk
scenarios.

Reproducing these numbers:
```
cmake --build build_clang21 --target bench_marshal bench_marshal_v2 -j32
./build_clang21/bench_marshal   # baseline
./build_clang21/bench_marshal_v2  # prototype
```

