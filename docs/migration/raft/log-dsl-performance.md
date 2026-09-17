# Performance of the Raft log DSL conversion

Measurement record for the three commits that made the Raft log Rust-owned:

| commit | what it did |
|---|---|
| `c95621a33` | `RaftData` -> DSL `RaftEntry` (9 fields to 2); container still `map<slotid_t, shared_ptr<RaftEntry>>` |
| `2b359f186` | container -> DSL `RaftLog` (`rusty::Vec<RaftEntry>` + base index) |
| `cd8188d88` | deleted `min_active_slot_` / `last_log_index_`, now `base()` / `last_index()` |

Baseline for every comparison is `3c6c739a1`, the commit immediately before
the first of them.

Everything below is MEASURED unless marked otherwise.

## Method

`examples/raft_bench.sh`, 1 partition, 1024-byte payload, batch 1, 10s
measured window after a 2s warmup. Baseline built in a separate worktree;
the compile flags for `server.cc` were diffed between the two build trees
and are byte-identical, as are `CMAKE_BUILD_TYPE`, both compilers, and
`CMAKE_CXX_FLAGS*`.

Arms were interleaved trial by trial rather than run in blocks, and the
whole comparison was then repeated with the within-pair order reversed,
because the first run of a pair could otherwise carry a systematic
advantage. Both orderings agree.

## Result

Two independent 6-trial runs, `scripts/raft_perf/compare.py --threshold 5`:

```
                        after-first run          before-first run
saturation throughput   -5.00% (floor 1.40%)     -5.29% (floor 0.84%)
saturation p50          +5.97% (floor 0.56%)     +5.90% (floor 0.83%)
saturation p99          +2.34% within noise      +2.01% within noise
throttled 20k/s p50     -6.39% BETTER            -7.56% BETTER
throttled 20k/s p99     -5.61% BETTER            -6.82% BETTER
throttled throughput    +0.00% within noise      +0.00% within noise
```

The saturation p50 rise is not independent of the throughput drop. The
offering thread is blocked on the 4096-entry in-flight bound for 9.9 of the
10 seconds, so latency there is just `outstanding / throughput`:
4096/40272 = 101.7ms measured 100.1ms, 4096/42394 = 96.6ms measured 94.5ms.
One phenomenon, reported twice.

## Where the cost is

Three-way bisect, n=5 per arm, arm order rotated each trial:

```
3c6c739a1  map + RaftData      42242 +-381   +0.00%
c95621a33  map + RaftEntry     41024 +-804   -2.88%
cd8188d88  RaftLog Vec         39849 +-556   -5.67%
```

It splits almost evenly. The entry conversion costs about as much as the
container conversion, which is the surprising half: that commit changed no
data structure, only the struct's field set (120 bytes to 32) and turned
two field reads into inline accessors.

## It scales with log length

```
duration  entries      delta
   3s     ~125k       -1.98%
  10s     ~400k       -5.67%
  20s     ~800k       -6.03%
```

Partitioning four ways, which quarters each log while keeping total entries
the same, gives -3.06% (n=3, and the baseline arm was noisy at +-3097, so
treat this as directional only).

With `MAKO_RAFT_SNAPSHOTS=1`, where compaction bounds the log instead of
letting it grow for the whole window, the gap is -4.29%. So this is not an
artifact of the unbounded-log regime the benchmark defaults to.

## The fix: blocks instead of one vector

The reallocation hypothesis was tested directly by pre-reserving the vector
for a million entries at construction, so it never grows during a window.
That recovered about two of the five points and pushed p99 and max BELOW the
map baseline, which identified the mechanism: the doubling reallocation
copies the whole log while `mtx_` is held, so the pipeline stalls for as long
as the memcpy takes. It is not the bytes -- the arithmetic below shows those
are negligible -- it is that the stall is synchronous and under the lock.

`RaftLog` now stores fixed 4096-entry blocks with the dead prefix recorded in
`head_`, so a full block is never touched again and an append never moves an
existing entry. Compaction releases whole leading blocks, which also removes
the `split_off` that copied every surviving entry.

Measured against the same baseline, n=5, `compare.py --threshold 5`:

```
                 flat vector        blocks
throughput          -5.67%          -2.05%   (noise floor 1.76%)
p50                 +5.90%          +2.72%   (noise floor 1.40%)
p99                +20.37%          -1.06%   within noise
max                +52.37%          -5.87%
verdict          REGRESSION      no regression beyond 5%
```

The tail is now better than the map it replaced. The remaining -2.05% is the
entry commit's share, discussed below; it is marginally above its noise floor
and under the gate's threshold.

## What was ruled out

- **Ordering bias.** Reversing the within-pair order reproduces it.
- **Build differences.** Identical flags, compilers, and build type.
- **Reallocation copying.** `sizeof(RaftEntry)` is 32 bytes, so the whole
  doubling sequence to 800k entries copies about 51MB across 20 seconds.
  That is a quarter of one percent at a pessimistic 1GB/s, not six.
  INFERRED from the size, not measured directly.
- **Compaction cost.** `RaftLog::compact_through` uses `split_off`, which is
  O(surviving entries); with snapshots off it is never called, and the
  regression is present anyway.

## The entry commit's share is not CPU

The remaining -2% belongs to `c95621a33`, which changed no data structure.
Profiling its leader against the baseline's, 20s windows, gperftools:

```
                        baseline   entry-only
total CPU samples         12198       12226
__memcpy                   1070        1181
__syscall_cancel_arch      1193        1161
futex_wait                  798         846
```

Identical CPU, near-identical profile shape, 2.8% less throughput. Its tail
barely moves either (p99 +5.0%, max +2.1%, against the flat vector's +20% and
+52%), so it is not the stall signature. Whatever it costs is off-CPU and is
not visible to a sampling profiler. Unattributed.

Note the profiler itself needed building: see below.

## Profiling on this host

`perf` is installed but `kernel.perf_event_paranoid` is 4, which denies
unprivileged `perf record`, and there is no root on this machine. gperftools
is absent -- CLAUDE.md's "Use Google perftools (linked automatically)" is
inaccurate, nothing links `libprofiler` -- and valgrind is not installed.
`kernel.yama.ptrace_scope` is 1, so `gdb -p` cannot attach to the benchmark's
processes.

What worked, with no privileges: build gperftools from source into the home
directory (it has a CMake build, so the missing `libtool` does not matter),
then `LD_PRELOAD` its `libprofiler.so` with `CPUPROFILE` set, which samples
through `setitimer`/SIGPROF and needs no kernel permission at all.

One wrinkle worth recording: `pprof` silently fails to symbolise a profile
whose `/proc/self/maps` has an inode wide enough to abut the path with no
separating space (`1099538229610/home/...`). The mapping is dropped and every
main-binary frame renders as a bare address. Inserting the space into the
profile's text tail fixes it.

## What could not be done

- `perf` is installed but `kernel.perf_event_paranoid` is 4, which denies
  unprivileged `perf record`.
- gperftools is absent. CLAUDE.md's "Use Google perftools (linked
  automatically)" is inaccurate; nothing links `libprofiler`, in the same way
  its Docker instruction is inaccurate.
- `valgrind` is not installed, so callgrind is unavailable.
- `kernel.yama.ptrace_scope` is 1, so `gdb -p` cannot attach to the
  benchmark's processes for stack sampling; they are not gdb's children.

Attributing the remaining cost needs one of those. Lowering
`perf_event_paranoid` to 1, or installing `valgrind` or `gperftools`, would
be enough.

## Correctness, for the record

All three commits are correct as far as the suites can tell: raftLabTest
25/25 with ALL TESTS PASSED at each, `shard1ReplicationSimpleRaft` clean at
each, both build trees green, and `cargo check` / `cargo clippy` clean on the
extracted `raft` crate. The transitional assertion carried through
`2b359f186` never fired, which is what licensed deleting the two duplicate
extent fields in `cd8188d88`.
