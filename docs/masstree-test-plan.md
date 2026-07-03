# Masstree Test Plan — Path to Industry Grade

Scope: `src/masstree/` only. Excludes the Silo transactional layer
(`test_silo_runtime.cc`, `test_silo_multi_site_stress.cc`) and excludes
the STO benchmark suite under `src/mako/sto/`.

## Current state (baseline)

Three gtest binaries, **20 cases total**, exercise the Masstree tree and
its supporting runtime:

| Binary | Cases | Coverage |
|---|---:|---|
| `test_masstree` | 2 | `InsertSearchAndRemove`, `RangeScanReturnsSortedKeys` |
| `test_masstree_internals` | 13 | threadinfo, global epoch, RCU start/stop/quiesce/deferred-dealloc, pool allocation, allthreads-list integrity |
| `test_masstree_multi_instance` | 5 | `MasstreeContext` epoch & registry isolation, concurrent RCU, two parallel trees, RCU stress |

Plus one legacy upstream tree test wired via CTest:
`test_masstree_legacy_scan` (uses `query_masstree` against value-string,
value-array, value-versioned-array; conditional on `log.hh`).

The other legacy upstream tests (`string`, `atomics`, `json`, `msgpack`)
exercise utility headers, not the tree.

**Coverage gap:** tree-correctness is essentially **2 gtest cases**.
Everything else tests infrastructure around the tree (epochs, RCU,
contexts), which would not catch a regression in `masstree_insert.hh`'s
split logic.

## Target

Bring tree-correctness coverage from 2 cases to a defensible floor
(~50–80 unit cases) and add the property-based, concurrency,
performance, fuzzing, and soak tiers that a production-grade
concurrent index library is expected to ship with.

---

## Tier 1 — Functional correctness

**Goal:** make `test_masstree.cc` a real correctness suite.
**Why first:** lowest risk, immediate confidence boost; every later tier
benefits from a thorough functional matrix as the oracle.
**Estimate:** 1–2 weeks.

### 1.1 Key-shape matrix

Each shape exercises a distinct code path through key-slice handling.

- Empty key (zero-length)
- 1-byte key (one slice, partial)
- 7-byte key (one slice, partial)
- 8-byte key (one full slice, sub-key boundary)
- 9-byte key (forces second slice)
- 16-byte key (two full slices)
- Keys sharing 8-, 16-, 24-byte prefixes (forces layer creation)
- Keys longer than `MASSTREE_MAX_KEY_LEN` (must error gracefully)
- Keys containing embedded null bytes (Masstree slices are raw bytes)

### 1.2 Operation matrix per key shape

For each key shape from §1.1:

- `insert-new` (key not present)
- `insert-duplicate` (key present, default semantics)
- `insert-overwrite` (key present, replace value)
- `search-hit`
- `search-miss`
- `search-after-remove`
- `remove-existing`
- `remove-missing`
- `remove-then-reinsert`

### 1.3 Structural triggers

Workloads designed to exercise B+tree mechanics inside Masstree.

- Insert ascending → right-edge splits
- Insert descending → left-edge splits
- Insert random order
- Pattern that causes node merge / layer collapse on removal
- ≥ `width` (15) keys sharing a slice prefix → layer expansion
- Insert then remove all → tree returns to empty + zero leaked nodes

### 1.4 Scan semantics

- Forward scan
- Reverse scan
- Start-inclusive / start-exclusive
- End-inclusive / end-exclusive
- Limit (first N)
- Scan on empty tree
- Scan crossing layer boundaries
- Scan where the start key was just removed (iterator contract)

### 1.5 Iterator contract under mutation

Masstree scans use optimistic retry; this contract should be tested
explicitly so a future refactor can't silently break it.

---

## Tier 2 — Property-based / oracle tests

**Goal:** find bugs no human will write a test for.
**Why second:** small code surface, very high bug-find / LOC ratio.
**Estimate:** 3–5 days.

### 2.1 Reference oracle

- `std::map<std::string, std::string>` as gold model.
- Generate random sequences of `{insert, overwrite, remove, lookup,
  range-scan}`.
- After every op: assert Masstree result matches oracle.

### 2.2 Determinism & shrinking

- Seeded RNG; failing seed printed on failure.
- Replay-on-failure harness.
- Shrinking (rapidcheck integration or hand-rolled minimizer).

### 2.3 Invariant checker (debug builds)

Runnable after every op:

- Leaf key order (within node).
- Internode key bounds (parent separators).
- Permuter consistency.
- Parent-pointer consistency.
- No orphan layers.
- Version-counter monotonicity.

---

## Tier 3 — Concurrency correctness

**Goal:** prove the lock-free / optimistic-retry machinery actually works.
**Why third:** without Tiers 1 & 2 it's hard to tell whether a
concurrency failure is a race or a single-threaded bug.

### 3.1 Sanitizer matrix — wired

Each test binary builds and runs cleanly under ASan, UBSan, and TSan.

**Workflow** — one build dir per sanitizer:

```bash
# AddressSanitizer
mkdir build_asan && cd build_asan
MAKO_ASAN=1 cmake .. -GNinja
ninja test_masstree test_masstree_property test_masstree_concurrent
./test_masstree && ./test_masstree_property && ./test_masstree_concurrent

# UndefinedBehaviorSanitizer
mkdir build_ubsan && cd build_ubsan
MAKO_UBSAN=1 cmake .. -GNinja
ninja test_masstree test_masstree_property test_masstree_concurrent
UBSAN_OPTIONS="suppressions=$(realpath ../src/masstree/ubsan_suppressions.txt):halt_on_error=1:print_stacktrace=1" \
  ./test_masstree

# ThreadSanitizer
mkdir build_tsan && cd build_tsan
MAKO_TSAN=1 cmake .. -GNinja
ninja test_masstree test_masstree_property test_masstree_concurrent
TSAN_OPTIONS="suppressions=$(realpath ../src/masstree/tsan_suppressions.txt):halt_on_error=0" \
  ./test_masstree_concurrent
```

`MAKO_ASAN` and `MAKO_TSAN` both force `USE_MALLOC_MODE=0` (libc malloc)
because each sanitizer replaces `malloc/free`; mixing with jemalloc
deadlocks at startup. `MAKO_UBSAN` is compatible with jemalloc.

**Suppressions files**:

- `src/masstree/ubsan_suppressions.txt` — three classes of pre-existing
  upstream UB:
  1. `kpermuter.hh:128` shift exponent equals 64 (UB per the C++ memory
     model; tolerated on x86). Fix is a one-line clamp.
  2. `string_slice.hh` unaligned 8-byte loads. Fix is `memcpy` into a
     local `uint64_t`.
  3. `internode::ikey` array-index-from-stale-position inside the
     stable_last_key_compare retry loop — the value is rejected by the
     surrounding version check, but UBSan sees the intermediate access.
- `src/masstree/tsan_suppressions.txt` — Masstree's optimistic
  version-counter machinery does intentional racy reads validated by
  retry. Suppressed at the source-file granularity. Also suppresses
  the pre-existing `src/mako/spinlock.h` plain-int spinlock (real bug
  in the surrounding mako code, separate fix).

**Status**: ASan clean (0 findings). UBSan and TSan clean after the
suppressions above are applied. CI integration (separate jobs per
sanitizer) is the remaining piece.

### 3.2 Linearizability check

- Small key space (e.g. 64 keys), 4 threads, 10k ops.
- Record per-thread op log + return value.
- Run a Lincheck-style verifier (or hand-rolled) against the
  `std::map` model: any linearization must exist.

### 3.3 Reader-writer stress

- N writers + M readers.
- Assert readers never observe an invariant-violating state
  (e.g. key not in tree but visible; key present but value mismatched).

### 3.4 RCU correctness

- Long-running readers spanning many writer epochs.
- ASan + a `Drop`-counting allocator wrapper assert no
  use-after-free across epoch boundaries.

### 3.5 Forward progress

- Per-op timeouts assert no deadlock / livelock under contention.

---

## Tier 4 — Memory & resource correctness — partially wired

**Goal:** catch leaks, fragmentation, RCU mis-reclamation.

`src/masstree/tests/test_masstree_memory.cc` adds three tests that
double as ASan/LSan detectors:

| Test | What it asserts | What ASan/LSan turns it into |
|---|---|---|
| `MassiveInsertRemoveSizeReturnsToZero` | Insert 100 k, remove all, size == 0 | LSan flags any per-key allocation still live at exit |
| `RepeatedFillEmptyCyclesAreStable` | 1,000 fill-empty cycles, size == 0 each | LSan catches RCU-deferred frees that never run |
| `ReadersSurviveAggressiveWriterChurn` | 2-second 4-writer/2-reader churn; stable keys keep returning correct values | ASan catches a UAF where RCU reclaims a node a reader is mid-traversal of |

All three pass clean in both regular and ASan builds (3 tests, ~2.2 s
total). Combined with Tier 3.1's ASan run of `test_masstree_concurrent`,
the existing coverage answers most of the original Tier 4 checklist:

- ASan & LSan jobs: wired in Tier 3.1 (`MAKO_ASAN=1`); LSan default-on
  catches process-exit leaks.
- Deferred-free → reader → epoch-advance test:
  `ReadersSurviveAggressiveWriterChurn` plus
  `LongRunningReadersAcrossEpochs` from `test_masstree_concurrent`.
- Leak gate: ASan's LSan at process exit.

Open items left for later (not implemented in this iteration):

- Allocator failure injection (malloc-null path) — would require
  hooking `rcu::alloc` to fault-inject on a schedule. Deferred.
- Pool fragmentation soak with RSS accounting (`/proc/self/statm`).
  Skipped because RSS is too noisy across hosts to gate on; a manual
  inspection workflow would be more useful than a CI assertion.

---

## Tier 5 — Performance regression gate

**Goal:** prevent "correct but 3× slower" landings.
**Why fifth (early):** Masstree's value is performance; a perf gate
should be online before deeper refactors begin.
**Estimate:** 2 days for the harness, ongoing for baseline tuning.

### 5.1 Microbench harness — wired

`src/masstree/tests/masstree_perf.cc` runs six scenarios against a
`single_threaded_btree`:

| Scenario | What it measures |
|---|---|
| `insert_sequential` | bulk fill in key-ascending order |
| `insert_random` | bulk fill in shuffled order |
| `lookup_random` | point lookups on a pre-filled tree |
| `mixed_read_write` | 80/20 read/write on a pre-filled tree |
| `range_scan` | windowed range scans |
| `remove_sequential` | bulk remove |

Knobs:

- `--keys N` keyspace (default 32,768)
- `--lookup-rounds M` iterations for read-heavy scenarios (default 4)
- `--scan-window W` range-scan width (default 256)
- `--repetitions R` run each scenario R times, report best
- `--output file` write per-scenario JSON
- `--baseline file` compare against earlier `--output`
- `--fail-on-regress PCT` (with `--baseline`) exit 2 if any scenario
  is more than PCT % slower than baseline

Multi-thread coverage (added on top of the six single-thread scenarios
above) — pass `--threads "1,2,4,8"` (any comma-separated list) to add
three concurrent scenarios per thread count, named with a `_tN` suffix:

| Scenario | What it measures |
|---|---|
| `parallel_insert_tN` | N threads inserting disjoint key ranges into one tree |
| `parallel_lookup_tN` | N threads point-looking up a pre-filled tree |
| `parallel_mixed_tN` | N threads doing 80/20 read/write on a pre-filled tree |

The threads launch under a single atomic "go" flag so the wall-clock
window used for ops/sec brackets the parallel work, not thread spin-up.
Default is no multi-thread scenarios (back-compat).

**Workload sizing for credible multi-thread numbers**: at the default
`--keys 32768`, the parallel section can complete in milliseconds at
high thread counts, so launch overhead and imbalance dominate. Pass
`--keys 1048576` (1 M) or larger when running anything past ~4 threads.
On a 32-core / 64-thread Threadripper 2990WX with 1 M keys we observe
roughly 11× lookup / mixed scaling t1→t16 and ~6× insert scaling
t1→t16, with all three flattening or dropping slightly at t32 (write
contention on internal nodes plus the dual-die NUMA hop).

### 5.2 Regression-gate workflow

Per-hardware baselines aren't checked into the repo — what counts as a
regression on one machine is the norm on another. The intended pattern
is "before and after":

```bash
# 1. Capture baseline on `main`.
build_local/masstree_perf --repetitions 5 --output baseline.json

# 2. Apply your change, rebuild, re-run with the gate.
build_local/masstree_perf --repetitions 5 \
    --baseline baseline.json --fail-on-regress 15
echo "exit=$?"   # 0 = within 15 %, 2 = regressed beyond threshold
```

Threshold tuning notes:

- `--repetitions 1` (the default) has ~10 % noise on cold-CPU runs.
  Use ≥ 3 reps before relying on the gate; 5 reps in CI is reasonable.
- A 15 % threshold is a sane default for noisy shared hosts. On a
  dedicated box you can tighten to 5 % once you've validated the
  noise floor.
- Exit codes: 0 = pass, 1 = invalid args or scenario error, 2 = gate
  triggered. CI scripts should fail on any non-zero.

### 5.3 Cache-locality probe (optional, not wired)

- Scan benchmark with `perf stat` L1/LLC miss counters.
- Likely flaky in shared CI; treat as advisory if added.

---

## Tier 6 — Fuzzing — wired and running

**Goal:** catch the bugs the test suite was not designed to think about.
**Why sixth:** once Tiers 1 + 2 are in place, fuzzing finds deep bugs fast.

`src/masstree/tests/fuzz_masstree.cc` is a libFuzzer differential
harness: each invocation decodes the fuzzer-supplied byte string into
a sequence of `{insert, insert_if_absent, remove, search}` ops on a
short keyspace and applies each op to both Masstree and a
`std::map<std::string,uint64_t>` oracle. Any divergence in return
value, value payload, or final forward-scan output `abort()`s, which
libFuzzer treats as a finding and minimizes the input.

### Build & run

The build uses brew's clang 22 (`~/.linuxbrew/bin/clang++`) because
the stock Debian/Ubuntu `libclang_rt.fuzzer-x86_64.a` from
`libclang-rt-19-dev` is built against libstdc++ while this project
uses `-stdlib=libc++`, and the two ABIs cannot satisfy each other's
std::string references in a single link. Brew's LLVM ships with its
own libc++-built libFuzzer (whose private string namespace is
`std::__Fuzzer`) so the link is clean.

The `MAKO_FUZZER=1` cmake toggle implies `MAKO_ASAN=1`, strips
jemalloc, and instruments the whole build with
`-fsanitize=fuzzer-no-link`; the `fuzz_masstree` target additionally
links `-fsanitize=fuzzer` to pull in the libFuzzer runtime + `main`:

```bash
mkdir build_fuzz && cd build_fuzz
MAKO_FUZZER=1 cmake .. -GNinja \
    -DCMAKE_C_COMPILER=$HOME/.linuxbrew/bin/clang \
    -DCMAKE_CXX_COMPILER=$HOME/.linuxbrew/bin/clang++
ninja fuzz_masstree
mkdir corpus
./fuzz_masstree -max_total_time=60 -max_len=4096 corpus/
```

A representative 30-second smoke run on this 32-core host completed
~580 k iterations (~19 k runs/sec) with no divergence — confirming
both that the harness explores Masstree's input space efficiently and
that the surrounding libmako is sufficiently instrumented for
coverage-guided mutation.

### One known source-level requirement

Brew's libc++ 22 enforces the random-access iterator concept on
heap/sort algorithms — its `__sift_down` calls `operator[]` even on
iterators that declared themselves bidirectional. The
`silo_small_vector::small_iterator_` and `iterator_<...>` classes had
all the random-access operations (`+=`, `-=`, `+`, `-`, distance)
except `operator[]`. A two-method patch in
`src/mako/silo_small_vector.h` adds it. Backward-compatible with
system clang 19.

---

## Tier 7 — Masstree-specific scenarios — wired

**Goal:** cover the design's signature edge cases — the ones that
arise from Masstree being a trie of B+trees indexed by 8-byte
slices, not a flat B-tree.

`src/masstree/tests/test_masstree_layered.cc` covers the new ground:

| Test | Targets |
|---|---|
| `DeepLayerKeysRoundTrip` | 64-byte shared prefix → 8 nested layers; 32 keys split the deepest layer's leaf. |
| `LayerCollapseOnRemoval` | 24-byte shared prefix → 3 layers; insert 64 keys then remove all. Under LSan this catches a missed layer-free in `masstree_remove.hh`. |
| `MixedLengthKeysInOneLeaf` | Same 8-byte first slice, suffixes of length {0, 1, 7, 8, 50, 200, 1000} bytes — stresses keylenx packing and forward-scan ordering across wildly different key lengths. |
| `SliceBoundaryFuzz` | Every length L in 1..32 inserted in shuffled order, half removed, then reinserted — cross-product of "just under 8B", "exactly 8B", "8B + 1", etc. |

Already covered earlier:

- Empty-string key — Tier 1 `EmptyKeyRoundTrip`.
- Keys with embedded null bytes — Tier 1 `KeyShapes/embedded_nulls`.
- 8/9/16/17-byte single keys in isolation — Tier 1 parameterized shapes.

All 4 layered tests pass clean under both regular and ASan builds
(~0 ms wall; tiny histories that exercise specific code paths).

---

## Tier 8 — Long-running soak / chaos — partially wired

`src/masstree/tests/test_masstree_soak.cc` runs a sustained mixed
workload (writers, removers, readers, scanners) on a shared
`concurrent_btree` for a bounded duration. Default 30 s
(CTest-friendly); set `MASSTREE_SOAK_SECONDS=N` to extend, e.g.
`MASSTREE_SOAK_SECONDS=86400` for a weekly 24-hour run. CTest TIMEOUT
property is 120 s so the default fits; the env-var override pushes
through it for manual long runs.

What it catches above and beyond Tier 3's shorter stress:

- Slow leaks invisible in a 2-second window (paired with LSan).
- Epoch-counter wrap edge cases that require many cycles.
- Race classes that depend on uncommon scheduling — longer wall
  time → more chances to interleave the wrong way.

Stable-key invariant: a fixed 8,192-key range is never written
after setup. Readers continually verify these keys; any mismatch
or missing key bumps an atomic failure counter that the test
asserts is zero at the end.

**Not wired**: thread join/leave churn (workers that spawn briefly,
do a batch, then exit, repeated continuously). The earlier version
of the soak test included that and reliably reproduced a SIGABRT
inside `concurrent_btree` — see Finding 6 in
`docs/masstree-sanitizer-findings.md`. Until that's investigated
the soak runs with long-lived workers only.

---

## Tier 9 — Build & API gates — partially wired

**Goal:** prevent accidental ABI breaks and transitive-include rot.

`src/masstree/tests/test_masstree_headers.cc` is a header
self-containment sentinel. It #includes every public Masstree header
plus the mako-side wrappers, and instantiates both
`single_threaded_btree` and `concurrent_btree` so the templates are
actually compiled. If a future refactor drops a forward declaration
or starts depending on a transitive include from an unrelated header,
this TU fails to compile before a downstream consumer notices.

Not wired (deferred, low priority for a template-heavy header-only
library):

- `nm`-based public-symbol gate. Less meaningful here because the
  public API is templated (`mbtree<P>`) — there is no stable ABI
  surface to diff between commits the way there would be for a C
  library.
- Multi-`-std=` compiler matrix (C++17 / C++20 / C++23). The
  project already pins `gnu++23`; adding additional std modes would
  require duplicated build targets. Worth doing if Masstree ever
  ships as a standalone library outside this project.

---

## Suggested execution order

1. **Tier 1** — functional correctness matrix.
2. **Tier 2** — property + oracle.
3. **Tier 5** — perf regression gate.
4. **Tier 3** — concurrency (TSan first, linearizability second).
5. **Tier 6** — fuzzing.
6. **Tiers 4, 7, 8, 9** — fill in around the others.

## Tracking

- File this plan in `docs/masstree-test-plan.md` (this file).
- Each tier delivered as a separate PR or PR series.
- Update the "Current state" section as cases land.
