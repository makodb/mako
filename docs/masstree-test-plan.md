# Masstree Test Plan — Path to Industry Grade

Scope: `src/masstree/` only. Excludes the Silo transactional layer
(`test_silo_runtime.cc`, `test_silo_multi_site_stress.cc`) and excludes
the STO benchmark suite under `src/mako/benchmarks/sto/`.

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

## Tier 4 — Memory & resource correctness

**Goal:** catch leaks, fragmentation, RCU mis-reclamation.

- ASan & LSan jobs in CI (verify currently wired).
- Explicit deferred-free → reader → epoch-advance test.
- Allocator failure injection (malloc-null path); may be deferred.
- Pool exhaustion test: insert millions of keys, assert no
  pathological fragmentation.
- Leak gate: full test run; assert allocator reports zero leaks
  (jemalloc hooks).

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

## Tier 6 — Fuzzing — wired (with a known link blocker on this host)

**Goal:** catch the bugs the test suite was not designed to think about.
**Why sixth:** once Tiers 1 + 2 are in place, fuzzing finds deep bugs fast.

`src/masstree/tests/fuzz_masstree.cc` is a libFuzzer differential
harness: each invocation decodes the fuzzer-supplied byte string into
a sequence of `{insert, insert_if_absent, remove, search}` ops on a
short keyspace and applies each op to both Masstree and a
`std::map<std::string,uint64_t>` oracle. Any divergence in return
value, value payload, or final forward-scan output `abort()`s, which
libFuzzer treats as a finding and minimizes the input.

Build via the `MAKO_FUZZER=1` cmake toggle (implies `MAKO_ASAN=1`,
strips jemalloc, instruments the whole build with
`-fsanitize=fuzzer-no-link`; the fuzz target additionally links
`-fsanitize=fuzzer` to pull in the libFuzzer runtime + `main`):

```bash
mkdir build_fuzz && cd build_fuzz
MAKO_FUZZER=1 cmake .. -GNinja
ninja fuzz_masstree
mkdir corpus
./fuzz_masstree -max_total_time=60 -max_len=4096 corpus/
```

### Known blocker on Debian/Ubuntu hosts

On a stock `libclang-rt-19-dev` install the link **fails** with
unresolved `std::__cxx11::basic_string` references inside
`libclang_rt.fuzzer-x86_64.a`. Root cause: the distro fuzzer runtime
is compiled against libstdc++, and this project uses
`-stdlib=libc++`. The two ABIs cannot satisfy each other's std::string
references in a single link, even with `-lstdc++ -Wl,--no-as-needed`
added at the end of the link line.

The fuzz target source itself is correct — gtest, ASan, and UBSan
runs of test_masstree_property exercise the same code path against
the same oracle without divergence. The Tier 6 wiring is ready to
run once a libc++-built libFuzzer runtime is available.

**Workarounds**, roughly in order of effort:

1. Build compiler-rt's libFuzzer from source against libc++
   (~30 min one-time cost; produces a libc++-compatible
   `libclang_rt.fuzzer-x86_64.a` to drop in).
2. Switch the project to libstdc++ for the fuzz build only —
   requires rebuilding libmako without `-stdlib=libc++`, large
   blast radius.
3. Run the fuzz harness via OSS-Fuzz, whose toolchain images
   bundle a matching fuzzer runtime.

---

## Tier 7 — Masstree-specific scenarios

**Goal:** cover the design's signature edge cases.

- Layer explosion: keys with 100+ shared prefix bytes
  (deeply nested layers). Test memory bound and lookup cost.
- Layer collapse on removal: insert enough keys to create N layers,
  remove all, assert layers reclaimed.
- Key compression boundary: keys ending exactly on slice boundary
  vs one byte over.
- Variable key length within same node: mix 5-byte and 200-byte
  keys in the same leaf.
- Empty-string key.
- Keys containing embedded null bytes.

---

## Tier 8 — Long-running soak / chaos

**Goal:** catch slow leaks, epoch-advancement bugs, perf drift.

- 24-hour soak (weekly, not per-commit):
  - Random workload + memory accounting.
  - Epoch advancement + GC pressure under sustained load.
  - Assert: flat memory, no slow leaks, no perf drift.
- Thread join/leave churn: threads register/deregister with
  `MasstreeContext` repeatedly during workload.

---

## Tier 9 — Build & API gates

**Goal:** prevent accidental ABI breaks and transitive-include rot.

- `nm`-based public-symbol gate: catches accidental ABI changes.
- Header self-containment: each public header compiles standalone.
- Build matrix: C++17 + C++20, gcc + clang.

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
