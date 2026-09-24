# Marshal perf baseline — chunk-linked-list implementation

This file records the baseline for the existing `srpc::Marshal` (chunk-
linked-list with raw `char*` arithmetic, 51 per-method `// @unsafe`
overrides) before the planned `Cursor<rusty::Vec<uint8_t>>` rewrite.
Any future Marshal implementation must be compared against these
numbers using the same `bench_marshal` harness.

## Harness

- Source: `src/srpc/tests/bench_marshal.cc`
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

## Post-mortem: where did the 81% speedup actually come from?

A follow-up investigation (after the swap landed) measured resource
counters for the two implementations under the same bench load:

| metric              | chunk-list | Vec-backed | ratio   |
|---------------------|-----------:|-----------:|--------:|
| wall time           |     2.31 s |     1.10 s |  2.1×   |
| Max RSS             |  918,696 KB |   5,828 KB |  **158×** |
| Minor page faults   |    232,304 |        310 |  **749×** |
| mmap syscalls       |         80 |         52 |  1.5×   |

The 918 MB RSS on the chunk-list version was the smoking gun.

**Root cause**: `Marshal::read()` in the chunk-list implementation
had a commented-out `//delete chnk;` line. Every read in steady
state unlinked the consumed chunk from `head_` but never freed it.
The destructor only walked from `head_` forward, so chunks already
advanced past `head_` were leaked until process exit. The line had
been commented out since 2020 (commit 19046c3d, "all changes" — the
initial drop of the chunk implementation into the tree). No
production caller ever ran a long-lived `Marshal` heavily enough
for the leak to be noticeable; the microbench is the first
workload that produced enough back-to-back read cycles to expose it.

Working through the bench math: 50K iters × ~2 chunks consumed per
iter × ~8 KB chunk ≈ 820 MB of leaked storage. The page-fault
stalls from backing 232K newly-touched pages dominate wall-clock
time on the drain pattern; that's the bulk of the headline 81%
improvement.

**Other factors** (real, but smaller without the leak):
  - `shared_ptr<raw_bytes>` atomic refcount on chunk dtor (LOCK XADD
    per chunk destruction, ~50-100 cycles each on x86).
  - Triple-indirection per chunk byte access (`head_->data->ptr +
    read_idx`) vs single-indirection on the Vec (`buf_.data() +
    read_pos_`).
  - Branch count: chunk read/write paths have ~5 branches per op
    (head_==null?, fully_written?, n_write<n?, fully_read?,
    tail_==head_?); the Vec path is mostly straight-line.
  - Cache locality: two scattered 8 KB chunk allocations vs one
    contiguous Vec.

**Calibrated headline**: the Vec rewrite would still win every
benchmark scenario even if the leak were fixed in the chunk-list
implementation, but the gap on the drain pattern would shrink from
5× to closer to 1.5-2× — the chunk-walk indirection and atomic
ops account for a real but bounded fraction of the speedup.

The rewrite **fixes the leak as a side effect**: Vec capacity is
reused across reads (read() calls `buf_.clear()` when fully drained,
which resets `size_` to 0 but keeps the allocation), so steady-state
loops don't grow memory at all. This is a non-trivial latent-bug
fix on top of the safety win.

Diagnostic commands used:
```
# bench under /usr/bin/time -v
/usr/bin/time -v ./build_clang21/bench_marshal

# alloc syscall counts
strace -c -e trace=brk,mmap,munmap,mprotect -- ./build_clang21/bench_marshal
```

## Calibrated A/B: leak-fixed chunk-list vs Vec

To isolate the structural perf difference (independent of the
chunk-list leak), we benched a leak-fixed variant: same chunk-list
implementation as the original, but with the commented-out
`delete chnk;` in `Marshal::read()` uncommented. Same harness, same
hardware, same run.

| scenario                                              | leaky chunk | leak-fixed chunk | Vec       | chunk(fixed) vs Vec |
|-------------------------------------------------------|------------:|-----------------:|----------:|--------------------:|
| write+read i64 (fresh Marshal each pair)              |       94.61 |            54.63 |     54.85 | tied (Vec  +0.4%)   |
| write+read i64 (single Marshal, drains)               |       21.44 |             9.81 |      9.39 | tied (chunk +4%)    |
| write 1024 i64 then read 1024 i64                     |   21,353    |        10,976    |    9,950  | Vec +10%            |
| raw write(8) + read(8) (single Marshal)               |       20.69 |             9.22 |      9.59 | tied (chunk -4%)    |
| write 1KB blob + read 1KB blob                        |      149.49 |           110.41 |    107.86 | tied (chunk +2%)    |
| write+read std::string(100)                           |      171.21 |           115.91 |     81.65 | **Vec +42%**        |
| 4*i32 + string(100) round-trip                        |      265.38 |           209.30 |    159.52 | **Vec +31%**        |
| write 4KB blob + read 4KB                             |      308.55 |           259.47 |    259.52 | tied (0%)           |
| write 10x 1KB then drain 10x 1KB                      |    6,646.21 |         1,291.48 |  1,282.70 | tied (0%)           |
| Max RSS                                               |   918,696 KB |        5,884 KB |   5,828 KB | tied                |

### What the calibrated numbers actually say

**Once the leak is fixed, the two implementations are tied on most
scenarios.** The headline 81% improvement on the chunk-walk drain
pattern (`write 10x1KB then drain`) collapses to **0%** in the
calibrated comparison — that was entirely the leak. Same for the
bulk-blob transfers (1 KB and 4 KB scenarios) and the i64
hot-loop.

**The Vec-backed Marshal has one real structural win: ~30-40%
faster on string and string-heavy mixed payloads.** That isolates
to the `peek()` path used by varint decode (`m >> v_len`): the
chunk-list `peek()` walks chunks via a while loop with a branch
per chunk; the Vec `peek()` is a single memcpy from
`buf_.data() + read_pos_`. For payloads that decode one or more
varints (every string operator>>, every container operator>>),
this overhead adds up.

**On RPC-shaped workloads** (4*i32 + string, payload-shape index),
that translates to ~25-30% lower latency — a real but bounded
structural win.

### So what's the real value of the rewrite?

1. **Latent-bug fix** (the leak). Production callers never hit it
   in 5 years, but the bench was the first workload back-to-back
   enough to surface it. A rewrite that incidentally removes the
   foot-gun is worth something on its own.

2. **30-40% faster on varint/string-heavy paths**. The peek
   simplification (chunk-walk → straight memcpy) is genuine.

3. **The safety win that motivated the rewrite in the first place**:
   marshal.cpp -476 LOC, @unsafe LOC 382 → 10. That's the headline,
   not the bench numbers.

The bench numbers should be read as "no perf regression on bulk
paths, modest perf win on string-heavy paths." The original
chunk-list-headline 81% drain speedup was a measurement artifact
of an unrelated bug, not a structural advantage of the Vec design.

## End-to-end RPC A/B (rpcbench, single-host loopback)

The microbench numbers above are Marshal in isolation. For an
end-to-end view, we ran `rpcbench` against all three Marshal
variants under the same client/server configuration:

  - 4 client threads, 100 outstanding requests / thread
  - mode=fast (no fiber dispatch on the server — isolates the
    Marshal hot path from scheduler/fiber overhead)
  - 8 s per run, payload byte_size ∈ {100, 1024}
  - server + client both pinned to one host, loopback addr

| variant            | bsize=100 qps | bsize=1024 qps |
|--------------------|--------------:|---------------:|
| Vec (run 1)        |       927,317 |        644,391 |
| Vec (run 2)        |       981,667 |        693,299 |
| chunk leaky        |       857,419 |        623,277 |
| chunk leak-fixed   |       832,005 |        667,492 |

Mean Vec: ~955,000 qps @ 100B; ~669,000 qps @ 1024B (±~3%
run-to-run noise).

**Vec vs leak-fixed chunk-list**: **+15%** at 100B, ~tied at 1024B.
**Vec vs leaky chunk-list**: +11% at 100B, +7% at 1024B.

### Why the leak's RPC impact is smaller than its microbench impact

In production, Marshals are reused across RPCs on a connection.
Each frame writes into the existing chunk; chunks only get *fully
drained* (the trigger for the leak in `Marshal::read()`) sporadically,
not on every operation. The leak rate is bounded by how often a
chunk ends with `read_idx == write_idx`. At rpcbench's 800K-RPC/s
* 100B payload pace, that's only ~15 MB/s of leaked storage over
an 8 s run — visible as a ~7-11% throughput hit, not the
catastrophic 158× RSS blowup the microbench produced.

The microbench's `write 10x 1KB then drain` scenario specifically
constructs a fresh Marshal each iter and fully drains it — every
iter consumes 2 chunks and leaks both. That's the worst case for
the leak and it shows it.

### Why Vec still wins by 15% at 100B even after the leak fix

The 100B payload triggers the string operator>> path (varint
length decode + `peek()` + read), which is exactly the path
where the microbench showed Vec winning 30-42%. The end-to-end
RPC has many other costs (epoll, syscall, frame codec), so the
30-40% Marshal-layer win translates to ~15% at the RPC layer.

At 1024B payloads, the Marshal cost is a smaller fraction of
total RPC time, so the Vec advantage washes out — the two
implementations are tied within run-to-run noise.

### Calibrated final read

End-to-end RPC throughput, ranked best to worst at the small-payload
size where Marshal cost matters most:

  1. Vec-backed (current)         — ~955K qps @ 100B
  2. chunk-list, leaky (original) — ~857K qps @ 100B  (-10% vs Vec)
  3. chunk-list, leak-fixed       — ~832K qps @ 100B  (-13% vs Vec)

That the leak-fixed chunk-list is *slightly slower* than the
leaky one at 100B is within noise but plausible: the leaky version
skips the per-read `delete chnk` cost. The leak doesn't hurt RPC
throughput much over 8 s, but it would degrade long-running
servers in production. The Vec rewrite avoids both costs.

Diagnostic commands:
```
# build rpcbench against the current marshal.cpp
cmake --build build_clang21 --target rpcbench -j32

# server + client, capture avg qps
./build_clang21/rpcbench -s 127.0.0.1:8848 -w 16 -m fast &
./build_clang21/rpcbench -c 127.0.0.1:8848 -t 4 -o 100 -b 100 -n 8 -m fast \
   | grep "avg qps"
```

