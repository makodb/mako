# Why the Rust STO singleton-put lane is still ~24% slower

Measured 2026-09-17 on `zoo-002` (AMD EPYC 7702P, 2 threads/core, boost off,
`schedutil`), matched controls at 1 worker, engine-only mode, 256 keys, 128-byte
values, checksum disabled. This report explains the mechanism behind the
remaining gap on the residual report's own cells. It changes no production code.

## Verdict

The Rust lane does not lose to stalls, cache behaviour, or branch prediction. It
retires **819 more instructions per transaction (2597 -> 3416, +31.5%) at the
same IPC (~2.3)**, with *fewer* branch misses and *fewer* L1 misses per
instruction. The 25% throughput deficit is that extra work, almost exactly:

- 819 extra instructions at the Rust lane's own IPC of 2.32 accounts for
  **+353 of the observed +382 cycles/tx** (1089.7 -> 1472.1); the remaining
  ~29 cycles/tx is the lane's ~2.5% lower IPC.

Those extra instructions are a balance of lane-specific work, and the largest
rows are *bookkeeping*, not payload work. Row values are self-time instruction
shares from section 4 (symbolic rows sum to +786 of the +819; the rest is below
the 0.15% reporting cutoff); positive = Rust costs more:

| Row | ins/tx | Evidence |
| --- | -----: | -------- |
| Generic singleton commit protocol (prepare/acquire/validate/install/release/teardown plan phases, hook driver, per-phase `catch_unwind`) vs C++'s `try_commit` + lock/check/unlock/cleanup | **+752** | profile rows; 10 `catch_unwind` frames and 11 `dyn` calls per tx |
| Per-operation native admission: structural-admission reader, RCU region, `ticker` guard, `coreid` lookups, and **one vDSO `gettimeofday`** | **+374** | ablation: 3416 -> 3046 ins, 1412 -> 1237 cyc, +13.7% tx/s; counters: 328,192 `gettimeofday` per 327,680 ops |
| Transaction lifecycle (`begin`/`attach`/`map_commit`/`touch`/`recycle_touched`/drop) vs C++ `start`/`stop` | **+153** | profile rows |
| Extra bulk payload copies (2x128 B vs 1x128 B `memcpy`) | **+115** | copy shim: 266 B vs 136 B per tx |
| Benchmark driver loop (shared Rust code, two instantiations) | +92 | profile rows |
| Erased item batch staging (`with_item_inner_validated`, `with_resolved_item_inner_validated`, `activate_typed_batch_item`, `Any` downcasts) | **+85** | profile rows |
| One owned 128-byte staging allocation + free per transaction | **+84** | allocator shim: 328,365 mallocs (42.18 MB = 128.5 B each) + 328,601 frees per 327,680 ops; C++: 823 mallocs total |
| Tree lookup (`find_unlocked` + `reach_leaf` vs `point_get_validated`) | -17 | profile rows |
| `BenchmarkConfig::getInstance()` (C++-only) | -28 | profile rows |
| Per-transaction RCU entry/exit callbacks (`thread_init` lambdas, foreground worker) | **-112** | profile rows; C++ takes RCU once per *transaction*, the Rust lane once per *operation* |
| Tree record write: bulk `memcpy` + lock-free publish vs C++'s release-fenced **byte-wise atomic store loop** in `stuffed_str::replace` | **-336** | `src/mako/sto/stuffed_str.hh:166-180`; C++ `install` = 580 ins/tx (23% of that lane) |
| Lane boundary: `mako_rust_fast_txn_*` C ABI shims (C++-only) | **-376** | profile rows |
| **Net** | **+819** | |

Two consequences of this shape. First, the Rust lane is not losing to payload
movement: its two bulk copies are *cheaper* than the C++ lane's one `memcpy`
plus a 148-byte byte-wise atomic loop, and it executes 17 fewer lookup
instructions. Second, the deficit is machinery — plan phases, per-lookup
admission, staged copies, driver/state-machine steps — that a one-item
transaction runs exactly once and cannot amortize; the same engine amortizes it
across a batch (section 5).

## 1 Matched controls, reproduced today

Frozen ThinLTO pair from the preceding optimization campaign
(`/var/tmp/mako-w1-fixes.ShjYqz/cpp-thin` sha256 `69e58f4b…`, `stage3-thin`
sha256 `d974c653…`), run as three alternating rounds of
`engine --workers 1 --iterations 262144 --warmup 65536 --perf 1`, pinned
`taskset -c 33-63` with workers on 0..0, `LD_PRELOAD` jemalloc:

| Round | C++ M tx/s | C++ cyc/tx | C++ ins/tx | Rust M tx/s | Rust cyc/tx | Rust ins/tx |
| ----- | ---------: | ---------: | ---------: | ----------: | ----------: | ----------: |
| 1 | 1.822 | 1089.7 | 2597 | 1.352 | 1472.1 | 3416 |
| 2 | 1.801 | 1103.2 | 2597 | 1.369 | 1452.4 | 3417 |
| 3 | 1.793 | 1109.6 | 2597 | 1.342 | 1479.9 | 3416 |

Rust/C++ = 0.743 today (0.760 in the residual report's table, which used the same
binaries). A separate probe harness pair (frame pointers, 16.7 M iterations)
gives 1.734/1.308 M = 0.755 at 2597/3426 ins/tx.

Extended `perf stat` counters on the same cells rule out the microarchitectural
explanations:

| Counter | C++ | Rust |
| ------- | --: | ---: |
| IPC | 2.34 | 2.29 |
| branch misses / all branches | 0.69% | 0.38% |
| branch misses per tx | 3.5 | 2.4 |
| L1d misses per 1000 instructions | 3.84 | 2.22 |
| dTLB misses (whole run) | 10,551 | 7,551 |
| frontend-stalled cycles | 15.9% | 9.5% |

Note on provenance: `engine-compare-sto-final-production` (sha256 `2ba98a1c…`),
the w1 audit's frozen candidate, is a *later, different* build; at 1 worker it
runs at 1.06 M tx/s on zoo-002 and must not be used as the report's control.

## 2 Per-operation native admission: 374 instructions, 171 cycles

The Rust `sto` lane enters a native RCU region, takes the `ticker` guard, checks
runtime health and takes structural admission **per operation**; the C++ lane
does its RCU entry/exit once per transaction through the
`MassTrans::thread_init()` callbacks and never reads a clock in the foreground.

Ablation (`/var/tmp/mako-residual-gap.7FPbwg/rcu`, the private whole-loop-scope
diagnostic over otherwise unchanged production libraries), three rounds today:

| Round | baseline tx/s | rcu-scope tx/s | baseline ins/tx | rcu ins/tx | baseline cyc/tx | rcu cyc/tx |
| ----- | ------------: | -------------: | --------------: | ---------: | --------------: | ---------: |
| 1 | 1,397,898 | 1,568,855 | 3417 | 3046 | 1421.3 | 1266.6 |
| 2 | 1,418,208 | 1,605,732 | 3416 | 3046 | 1398.7 | 1236.1 |
| 3 | 1,426,259 | 1,603,302 | 3416 | 3046 | 1394.0 | 1238.5 |

Delta: **-370 ins/tx, -165 cyc/tx, +13.7% throughput**. The `holder` variant run
in the same script is the null control: 3416 ins/tx and 1.372-1.386 M tx/s,
i.e. no change, so the method is sensitive and the effect is admission-specific.

Independent cross-check from the profiles: summing the admission symbols
(`ticker::guard`, `rcu::in_rcu_region`, `rcu::sync::do_cleanup`,
`scoped_rcu_base`, `coreid::*`, `SiloRuntime::Current`) plus the unresolved vDSO
IPs gives **10.92% of instructions = 374 ins/tx** and **11.64% of cycles = 171
cyc/tx** for the Rust lane, and **0.00% for the C++ lane**.

One of those instruction-blocks is a clock read per operation:

| Measure | Rust `sto` | C++ `c-abi` |
| ------- | ---------: | ----------: |
| `gettimeofday` calls, foreground worker | 328,192 | 13 (whole run) |
| `clock_gettime` calls, foreground worker | 5 | 0 |
| real syscalls in the run (`strace -c`) | none (only 3 `clock_nanosleep`) | none |
| vDSO share of instructions | 5-7% | < 0.04% |

The leaf is `[vdso] 0x86c`, the post-`rdtscp` TSC conversion inside the vDSO's
`gettimeofday` fast path (verified by sampling a `gettimeofday` microbenchmark,
which lands on the identical offset, at **68 ins / 94 cycles per call**, versus
`clock_gettime(CLOCK_MONOTONIC)` at vDSO `0xb03`, 87 ins / 92 cycles).

Source-level chain for that cost (current tree; the frozen binary's `ticker`
guard is the same mechanism):

- `sto-masstree` `lookup` -> `ensure_healthy` (`crates/sto-masstree/src/lib.rs:6057-6062`)
  -> `Directory::get` -> `Tree::get` -> `mako_mtree_get_trusted`
  (`crates/masstree/src/native.rs:305-322`, `src/mako/storage/mtree_abi.cc:2132-2147`).
- `trusted_operation` + `point_get_validated` (`mtree_abi.cc:909-975`) take, per
  directory lookup and *not amortized*: two native runtime-health loads, a
  `structure_read_guard` (sealed check, structure-reader bit `fetch_or`,
  `active_tree` publish/recheck, release clear; `mtree_abi.cc:254-302`) and an
  `operation_rcu_guard` that constructs `scoped_rcu_base<true,false>`
  (**`ticker` guard + `sync_->depth_++`, destructor `--depth_` + `do_cleanup()`**,
  `src/mako/rcu.h:358-401`). The ticker guard is where the `gettimeofday` comes
  from.
- The engine's put path never uses the Rust `ReadScope`/`PointSession` that would
  amortize this; it re-enters per lookup.
- The C++ lane pays its RCU pair once per transaction, in the
  `MassTrans::thread_init()` start/stop callbacks, and its clock reads happen on
  a helper thread (9,136 `clock_gettime` calls in the foreground run above),
  never on the measured worker.

## 3 Direct counts: copies, allocations, indirect calls

Interposition shims (`memcount.c`, `alloccount.c`, `clockcount.c`, appendix A),
`engine` mode, 262,144 + 65,536 ops per lane:

| Per transaction | Rust `sto` | C++ `c-abi` | Mechanism |
| --------------- | ---------: | ----------: | --------- |
| `memcpy` calls | 4.01 | 2.01 | Rust: 2x 128 B (TouchLog stage + record publish) + 2x <=16 B (harness key bytes); C++: 1x 128 B (pooled staging) + 1x <=16 B |
| bytes copied by `memcpy` | 266 B | 136 B | C++'s second 148 B payload copy happens *inside* `MassTrans::install` as a byte-wise atomic loop, invisible to the shim |
| `malloc` + `free` | 1.002 + 1.003 | 0.0025 + 0.004 | frozen pair: Rust owns one 128.5-byte staging buffer per transaction; C++ stages into a pooled `std::string`. The current tree's borrowed staging (`ValueRepr::BorrowedStaged`, `crates/sto-masstree/src/lib.rs:305-323`, taken when `38 < len <= 160` and `bounded_atomic_values`) removes that allocation by inspection; not re-measured here |
| vtable/erased calls | 11 `dyn` + 2 fn-pointer | 3 fn-pointer | engine-mode trace: `ErasedItemBatch`, `ErasedDirectCommitPlan`, `Any` downcasts, `CommitHook` (hook is `None` in engine mode, so no terminal, no record serialization, no HLC gate, no `canonical_ops()`) |
| `catch_unwind` frames | 10 | 0 | `direct_protocol::run_body` phase driver |
| RCU entries | 1 per op (+ ticker) | 1 per tx (callback pair) | see section 2 |

## 4 Function-level instruction ledger

`perf record -e instructions:u -F 3999 --call-graph fp` on the frame-pointer
probe pair, filtered to the measured window (tid + monotonic span from the
harness's `PROFILE_WINDOW` markers), self time, scaled by the lane's own
instructions/tx (2597 / 3426). Top entries:

| C++ `c-abi` | ins/tx | Rust `sto` | ins/tx |
| ----------- | -----: | ---------- | -----: |
| `MassTrans<…>::install` | 580 | `<…BorrowedInjectiveLockCommitPlan…>::acquire_all` | 125 |
| `Masstree::unlocked_tcursor<…>::find_unlocked` | 319 | `mako_engine_sto::txn::Transaction::put` | 122 |
| `Transaction::try_commit` | 190 | `mako_engine_sto::txn::Transaction::begin` | 116 |
| `mako_rust_fast_txn_put` (C ABI) | 173 | `with_resolved_item_inner_validated::{closure}` | 111 |
| `mako_rust_fast_txn_commit_and_destroy` (C ABI) | 125 | `with_item_inner_validated` | 105 |
| `Masstree::node_base<…>::reach_leaf` | 113 | `<…BorrowedInjectiveLockCommitPlan…>::prepare` | 104 |
| `reallyHandlePutFound` | 105 | `<…BorrowedInjectiveLockCommitPlan…>::release_all` | 96 |
| `thread_init()::{lambda()#1}` (RCU start) | 90 | `Masstree::unlocked_tcursor<…>::find_unlocked` | 230 |
| `handlePutFound<true,true,…>` | 86 | `point_get_validated` | 83 |
| `Transaction::stop` | 90 | `<…BorrowedInjectiveLockCommitPlan…>::validate` | 72 |
| `__memmove_avx_unaligned_erms` | 82 | `<…BorrowedInjectiveLockCommitPlan…>::install` | 73 |
| `Transaction::allocate_item` | 47 | `mako_engine_sto::txn::Transaction::recycle_touched` | 33 |
| `__func<…thread_init()::{lambda()#2}>::operator()` | 22 | `malloc` + `free` | 97 |
| — | — | `ticker::guard::guard` + `rcu::in_rcu_region` + `scoped_rcu_base` + `do_cleanup` + `coreid::*` | 201 |
| — | — | `vdso` (clock) | 68-241 (share-weighted; 68 by call count) |

Grouped by role, from the same two profiles (symbolic rows account for 2591 of
the C++ lane's 2597 ins/tx and 3378 of the Rust lane's 3426; the remainder is
below the 0.15% reporting cutoff):

| Role | C++ | Rust | Delta |
| ---- | --: | ---: | ----: |
| commit protocol (validate/acquire/install/release vs `try_commit` + lock/check/unlock/cleanup) | 258 | 1010 | +752 |
| lane boundary (C ABI shims / engine facade) | 376 | 0 | -376 |
| tree record write | 580 | 244 | -336 |
| native admission (ticker, RCU scope, `coreid`, health) | 0 | 201 | +201 |
| unresolved IPs (mostly the vDSO clock path) | 0 | 173 | +173 |
| transaction lifecycle (`begin`/`stop`/`start` plumbing) | 196 | 349 | +153 |
| payload copies (`memmove`/`memcpy`) | 86 | 201 | +115 |
| per-transaction RCU callbacks (`thread_init` lambdas) | 112 | 0 | -112 |
| allocator (`malloc`/`free`) | 0 | 84 | +84 |
| benchmark driver loop (shared Rust code) | 263 | 355 | +92 |
| item staging (batch/typed adapter/preflight) | 259 | 345 | +85 |
| tree lookup (`find_unlocked` + `reach_leaf` + `point_get_validated`) | 433 | 416 | -17 |
| `BenchmarkConfig::getInstance()` | 28 | 0 | -28 |

The +752/-336 pair is the same story told twice: the Rust lane moves work out of
a hand-specialised record write and into a generic commit-plan driver. Note that
these are *self* instruction counts, so a lane cannot be judged by one row: the
Rust lane's generic driver replaces C++ work that appears in several rows at once
(`handlePutFound`, `trans_write`, `allocate_item`, `thread_init` callbacks).

The residual campaign's own `cycles:u` profiles, scored over the same roles,
reproduce the same ordering (cycles/tx):

| Role | C++ | Rust | Delta |
| ---- | --: | ---: | ----: |
| commit protocol | 128.8 | 452.8 | +324.0 |
| tree record write | 252.0 | 100.3 | -151.8 |
| lane boundary (C ABI) | 163.6 | 0.0 | -163.6 |
| admission (ticker/RCU/`coreid`) | 0.0 | 93.2 | +93.2 |
| unresolved IPs (vDSO clock + libc) | 0.0 | 78.2 | +78.2 |
| transaction lifecycle | 81.9 | 149.4 | +67.5 |
| payload copies | 31.2 | 87.9 | +56.7 |
| per-transaction RCU callbacks | 52.1 | 0.0 | -52.1 |
| tree lookup | 135.1 | 182.5 | +47.4 |
| item staging | 111.4 | 145.0 | +33.6 |
| allocator + other | 11.4 | 40.5 | +29.0 |
| benchmark driver | 113.4 | 112.8 | -0.7 |

Admission plus unresolved-vDSO is **171.4 cycles/tx** here, matching the
ablation's **165 cyc/tx** within noise, and the rank order matches the
instruction table. Cycle totals: 1089.7 (C++) and 1472.1 (Rust) per transaction.

## 5 Fixed per-transaction cost versus per-item cost

Method: `--items N` (engine mode only) makes one commit stage N distinct keys;
the final-value verification is extended to the multi-key schedule
(`expected_iteration_multi`, in `engine_compare-items.rs` and
`patch_probe.py`). Three alternating rounds per cell, both lanes, at 1 worker.
If the deficit were a fixed per-transaction tax, the Rust/C++ ratio would
converge toward 1 as N grows and the per-item delta would shrink; if the deficit
is per-item work, the ratio stays flat.

**Status of the matrix.** The `--items` change is complete in source
(`engine_compare-items.rs` and `patch_probe.py`, with the verification schedule
extended to multi-key commits) and the curve has since been measured on the
current tree — see section 8.2. The split below is the source-level account of
the frozen revision's costs.

What the source says the split is (engine-mode trace; citations in appendix B):

- **Fixed per transaction**: worker/frame lifecycle (`worker::attach` TLS scan,
  `slot.acquire`, `WorkerContext::begin_isolation`, `frame.begin`,
  `take_touched`/`recycle_touched`), `CommitDriver::new`, the plan/report state
  machine, **10 `catch_unwind` frames**, 8 of the 11 erased calls,
  `SingletonBody::new` rebinding, `teardown_adapter_state`'s `Pooled` early
  return, `finish_items`' branch + `reset_shape`, `recycle_frame`,
  `complete_worker`, `slot.release`, `map_commit`.
- **Per item**: one directory lookup with its `structure_read_guard` +
  `operation_rcu_guard` (and vDSO clock read), one TouchLog entry and copy, one
  `prepare`/`acquire` (1 version CAS)/`install` (one publication: 32 B inline +
  6 B packed tail + 90 B suffix)/`release` (1 Release store) loop iteration, one
  payload copy at publish.
- **Discontinuity at N = 2**: `frame.item_count == 1` no longer holds, so the
  singleton lane (`SingletonBody`) is replaced by the erased per-phase path
  (`direct_preflight`/`direct_acquire_all`/`direct_validate`/`direct_install`/
  `direct_release_all`, +5 erased calls), the item index and batch capacities
  materialize, and the second put loses the borrowed-staging lane, adding one
  128-byte allocation and one copy (`ValueRepr::Staged`).

## 6 Limits of this attribution

- Instruction shares come from PMU sampling with `-F 3999`; with a short
  periodic loop the sampled IP can alias onto a few addresses. Every claim here
  is cross-checked against a cycle-share, a DSO-level share, or a direct call
  count (shim / ablation), so no conclusion rests on a single profile share.
- All cells are 1 worker, engine-only, existing key, 128-byte value. The
  `engine-logged` and `cache-ack` modes add other costs (record terminal, cache
  queue) and are not attributed here.
- The `rcu`, `holder` and `--items` builds are private diagnostics over
  production libraries; none of them is a production change.
- The frozen pair reproduces at 1.82 M vs 1.35 M on zoo-002 today. On zoo-005
  (Threadripper 2990WX, boost on) the same binaries were measured at 1.86 M vs
  1.06 M; cross-host absolute numbers are not interchangeable.
- The instruction profile was taken on the frame-pointer *probe* pair (16.7 M
  iterations), not the frozen pair; the two agree to within 1.5% on both
  instructions/tx and throughput ratio.

## 7 What this implies for the cutover

Ranked by measured size, with the evidence's own ceiling:

1. **Per-operation native admission (up to +13.7% tx/s if amortized).** The
   whole-loop-scope ablation is an upper bound: it removes the per-lookup
   `structure_read_guard`, the `scoped_rcu_base` pair and the `ticker` clock read
   and returns 3416 -> 3046 ins/tx. A production form needs a bounded,
   synchronous batch boundary (the diagnostic's own limits section says an idle
   or blocking scope is not safe).
2. **Generic commit-plan and item machinery (+752 ins/tx over the C++ lane).**
   Specializing the singleton path further (plan phases, erased dispatch,
   `catch_unwind` per phase) is the largest single block. The measurements do not
   say how much of it can be removed; the plan object itself is already pooled
   across transactions.
3. **Staging allocation and copies (+84 ins/tx and +115 ins/tx).** The current
   tree's borrowed staging path removes the 128-byte allocation by inspection;
   the two bulk copies are already cheaper than the C++ lane's byte-wise atomic
   store loop, so there is nothing to win by chasing copies.
4. **Do not chase `MassTrans::install`-style costs.** The C++ lane's 580 ins/tx
   byte-wise atomic store is a *dis*advantage of the C++ lane in this cell; a
   Rust design that matches it would be a regression.

## 8 Update 2026-09-18: what the current tree already fixed, and what each remaining fix is worth

The native archives were rebuilt (system headers had changed since 2026-09-14),
which unblocked building the *current* tree and the diagnostic harnesses. Three
new measurements change the priorities: HEAD has already removed a large part of
the gap, the remaining deficit is **per item**, and the batch-scope fix measured
in section 2 is worth much less on HEAD than it was on the frozen revision.

### 8.1 HEAD versus the frozen pair (engine, 1 worker)

| Lane | tx/s | cyc/tx | ins/tx |
| ---- | ---: | -----: | -----: |
| frozen Rust (`stage3-thin`) | 1,357,428 | 1459.2 | 3416 |
| **HEAD Rust** (`engine-compare-items-sto`) | **1,666,858** | 1193.3 | **2885** |
| frozen C++ (`cpp-thin`) | 1,822,104 | 1089.7 | 2597 |
| **HEAD C++** (`engine-compare-items-c-abi`) | **1,855,066** | 1070.2 | **2605** |

The instruction ratio fell from **1.315 (+31.5%) to 1.108 (+10.8%)**: the
single-put gap on the current tree is ~10%, not 24%. Three of the costs this
report measured on the frozen binary are already gone in HEAD, verified with the
same shims:

| Per transaction | frozen Rust | HEAD Rust | HEAD C++ |
| --------------- | ----------: | --------: | -------: |
| `memcpy` calls / bytes | 4.01 / 266 B | **3.01 / 138 B** | 2.01 / 136 B |
| `malloc` + `free` | 1.002 + 1.003 | **0.0021 + 0.0027** | 0.0025 + 0.004 |
| worker `gettimeofday` | 328,192 per run | **13 per run** | 13 per run |

So the per-transaction staging allocation, the second bulk copy, and the
per-operation vDSO clock read are no longer costs — the borrowed staging,
single-copy publish and untimed RCU scope in HEAD removed them (531 ins/tx).

### 8.2 The remaining deficit is per item, not per transaction

`--items N` stages N distinct keys in one commit (both arms, verification
extended to the multi-key schedule; all cells verified 256 keys):

| items | C++ ins/tx | C++ ins/item | Rust ins/tx | Rust ins/item | Rust/C++ |
| ----: | ---------: | -----------: | ----------: | ------------: | -------: |
| 1 | 2605 | 2605 | 2886 | 2886 | 1.108 |
| 2 | 4472 | 2236 | 5614 | 2807 | 1.255 |
| 4 | 8196 | 2049 | 10062 | 2516 | 1.228 |

Linear fits: C++ = **740 fixed + 1865/item**; Rust's general (N ≥ 2) lane =
**1166 fixed + 2224/item**, and the singleton lane's fixed part is ≈ 662. The
Rust lane's fixed cost is therefore *lower* than the C++ lane's, and its
**per-item cost is 359 instructions higher** (+19%). The gap grows with
transaction size: +10.8% at one item, +22.8% at four. For multi-key workloads
the per-item machinery — not per-transaction plumbing — is what to optimize.

### 8.3 Batched scopes: measured curve, and why per-transaction scoping is a pessimization

`--rcu-batch K` reopens the native RCU region every K transactions (0 = one
region for the whole measured loop, the section-2 ceiling). Three rounds,
264,144 + 65,536 ops, `sto` arm built from the residual's patched source copy:

| K | tx/s | ins/tx | vs unpatched |
| -: | ---: | -----: | -----------: |
| unpatched | 1,357,428 | 3416 | — |
| **1 (per transaction)** | 1,270,116 | 3631 | **-6.4%** |
| 8 | 1,500,786 | 3121 | +10.6% |
| 16 | 1,508,762 | 3085 | **+11.1%** |
| 64 | 1,542,610 | 3058 | +13.6% |
| 0 (whole loop) | 1,552,561 | 3039 | +14.4% |

Two conclusions. First, **a per-transaction scope is worse than no scope**: it
adds ~215 ins/tx, because a transaction with one lookup pays the top-level entry
exactly once either way, and the explicit close/open pair costs more than the
per-operation guard it replaces. Second, the gain saturates early: K = 16 gets
88% of the ceiling. But this curve was measured on the stage3 sources, where
admission was 374 ins/tx; on HEAD admission is 116 ins/tx (8.4), so batching is
now worth at most ~4%, and the remaining admission cost is better attacked by
reusing the scope object than by batching.

### 8.4 HEAD role ledger: where the remaining +283 instructions live

Same role mapping as section 4, on the HEAD binaries (sto 2885, c-abi 2605):

| Role | C++ | Rust | Delta |
| ---- | --: | ---: | ----: |
| commit protocol (plan phases vs `try_commit` + lock/check/unlock) | 219 | 808 | **+589** |
| lane boundary (`mako_rust_fast_txn_*` shims) | 413 | 0 | -413 |
| transaction lifecycle (`begin`/`put`/`recycle_touched`/TouchLog) | 201 | 530 | **+329** |
| tree record write | 559 | 239 | -320 |
| native admission (guards, scope emplace) | 0 | 116 | +116 |
| per-transaction RCU callbacks | 98 | 0 | -98 |
| item staging | 299 | 337 | +39 |
| driver loop | 253 | 290 | +36 |
| payload copies | 82 | 106 | +24 |
| tree lookup | 429 | 415 | -15 |
| **Net** | **2596** | **2879** | **+283** |

Hot Rust-side items inside those rows: `BorrowedInjectiveLockCommitPlan::acquire_all`
167, `Transaction::begin` 172, `commit_with_optional_hook` 164,
`Transaction::put` 160, `with_resolved_item_inner_validated::{closure}` 145,
`run_single_item_direct` 104, `Transaction::recycle_touched` 101,
`finish_and_teardown` 94, `optional<scoped_rcu_base>::emplace` 89, `TouchLog::touch`
23 + `drop_in_place<TouchLog>` 15.

### 8.5 Re-ranked fix list

1. **Specialize the singleton commit path** (`sto-core` plan phases): +589 ins/tx
   sit in the Rust commit protocol against the C++ lane's 219. Elide the
   per-item erased dispatch (`ErasedItemBatch`/`ErasedDirectCommitPlan`) and the
   10 per-phase `catch_unwind` frames for the certified-unique lane, keeping the
   capability check and the `PlanState` machine. This is the only row that can
   close the remaining 10% on its own.
2. **Cut the per-item transaction lifecycle** (+329, of which `begin` 172,
   TouchLog touch/recycle/drop ~139, `put` 160 vs the C++ lane's 201 total):
   the engine-only path pays TouchLog bookkeeping for a value that is already
   borrowed; a singleton lane that skips it removes most of this row.
3. **Reuse the native scope object** instead of re-emplacing
   `optional<scoped_rcu_base>` per lookup (89 ins/tx); batched scopes are now a
   ~4% lever, not an 11% one.
4. **Do not touch** the tree write, payload copies, or the C-ABI boundary: Rust
   is 320 + 98 + 413 ins/tx ahead there. A C++ lane that replaced its byte-wise
   atomic store loop would close most of that by itself, which is the fair
   baseline for a cutover decision.

### 8.6 Implemented: one unwind frame for the direct-commit phases

`crates/sto-core/src/direct_protocol.rs` (commit `18e54d5e1`): the hook-free
direct lane previously wrapped every phase — acquire, validation, installation,
release — in its own `catch_unwind`, and the singleton body's creation in
another. Each landing pad forces its closure into a separate function and blocks
inlining across the commit chain. The phase sequence now runs under a **single**
unwind with `*self.phase` still written before every step, so a caught panic
reports the same `FailurePhase`, the caught payload keeps its lifetime, and the
owner's boundary/cleanup decision is unchanged; hook callbacks keep individual
containment because upper layers re-enter them.

Three interleaved rounds on zoo-002 (engine mode, 1 worker, frozen native
archives, previous binary side by side):

| Lane | tx/s before | tx/s after | cyc/tx before | cyc/tx after | ins/tx |
| ---- | ----------: | ---------: | ------------: | -----------: | -----: |
| Rust `sto` | 1,544,053 | **1,651,065** | 1283 | **1201** | 2886 -> 2906 |
| C++ `c-abi` | 1,845,211 | 1,860,488 | 1082 | 1066 | 2605 -> 2605 |

+6.9% throughput and -83 cycles/tx for the Rust lane, with the C++ lane
unchanged (control). Instructions per one-item commit rise by 20 while IPC rises
2.25 -> 2.42: removing the landing pads let the phase chain inline, trading a few
instructions for fewer calls and stalls. `--items` shows the per-item effect:
5468 ins/tx at two items and 9779 at four (from 5614 and 10062), i.e. **-71
instructions per item**; the singleton lane trades +20 instructions for its
lower cycle count. `sto-core` and `mako-engine-sto` tests pass (261 tests) and
every measured cell still verifies all 256 keys.

Absolute throughput drifts on this shared host (the same pre-change code
measured 1.667 M earlier in the day), so only interleaved comparisons carry
weight.

Remaining after this pass: the Rust commit protocol is still ~690 ins/tx against
the C++ lane's 219, spread over `plan.run_single_item_direct` (167),
`commit_with_optional_hook` (164), `TypedItemBatch::{run_single_item_direct,
finish_and_teardown}` (198), `plan.prepare` (88) and the erased-dispatch plumbing
(adapter `TypeId`/`Any` downcast/`capability`/lane selection, ~103). Closing that
needs the monomorphic singleton path: a certified-unique entry that calls
acquire/validate/publish/release on the concrete adapter and plan types without
the erased batch/plan layers — a design change to `sto-core`'s erased
representation, not a local optimization.

### 8.7 Re-verification, and two fixes the measurements rejected

**Measurement protocol correction.** Strictly interleaved A-then-B cells carry a
position bias on this host: the first cell of a round measures ~3-4% faster than
the second regardless of which binary it is. A teardown-skip build first looked
4% *faster* and then 4% *slower* depending on which position it occupied. Every
comparison below therefore uses ABBA ordering (A B B A / B A A B), equal repeats
per slot, medians, and the 4th build as a control where applicable.

**The section 8.6 change re-verified under that protocol** (20 samples per build,
`sto` arm, engine mode, 1 worker):

| build | tx/s median | range | cyc/tx | ins/tx |
| ----- | ----------: | ----- | -----: | -----: |
| before the change | 1,546,644 | 1.534-1.568 M | 1284.0 | 2886 |
| after the change | **1,659,758** | 1.643-1.676 M | **1197.3** | 2906 |

+7.3% throughput, -87 cycles/tx, non-overlapping ranges. The committed change
stands.

**Rejected: a per-transaction native RCU scope.** Implemented in
`crates/mako-engine-sto/src/txn.rs`: the transaction owns one
`masstree::RcuScope` from `begin` until the slot is released, closed explicitly
on commit, abort and every terminal (including the fused holder path, which the
cache *forgets* after publishing, so the region must be ended before it returns).
Measured: **3306 ins/tx and 1.37-1.41 M tx/s, a 17% regression.** This matches the
`--rcu-batch K=1` cell of section 8.3 independently: one scope begin/end pair per
transaction costs more than the per-lookup `operation_rcu_guard` it suppresses.
Reverted. (Side finding worth keeping: any resource owned by the engine facade
must be released before the fused publish, because the cache forgets that facade
on success.)

**Rejected: skip the plan teardown call when the plan is already pooled.** A
`teardown_required` predicate on `ErasedDirectCommitPlan` (default `true`,
overridden by the borrowed plan with exactly the condition under which its
teardown returns without touching adapter state) removes 6 ins/tx and one
`catch_unwind`. Measured under ABBA (24 samples per build): **1,591,238 vs
1,655,940 tx/s and 1247.8 vs 1198.2 cycles/tx — a 3.9% regression.** Fewer
instructions, worse cycles: the predicate is itself an erased call and the new
branch broke the straight-line phase sequence (IPC 2.42 -> 2.32). Reverted.

Both rejections sharpen the rule this workload follows: on the commit path,
removing an unwind helps only when the call sequence stays straight-line and
monomorphic (as in 8.6); adding indirection or a branch to save a few
instructions loses. The remaining +623-row (commit protocol) and +347-row
(lifecycle) therefore need the structural change, not further local trimming.

### 8.8 Rejected: resolving panic containment by policy

The engine's containment sites were converted to `contain()`, a helper that calls
the step directly under `panic = "abort"` and keeps `catch_unwind` otherwise, so
that an aborting build pays no wrapper and an unwinding build keeps the recovery
semantics (production sites only; test modules keep explicit `catch_unwind` for
fault injection). Both profiles verified: the unwind suites pass (18 test
binaries, `sto-core` + `mako-engine-sto`) and the abort harness builds and
verifies all 256 keys.

Measured under ABBA (20 samples per build, engine mode, 1 worker):

| build | tx/s median | range | cyc/tx | ins/tx (1 / 2 / 4 items) |
| ----- | ----------: | ----- | -----: | -----------------------: |
| before | 1,649,410 | 1.633-1.675 M | 1202.7 | 2906 / 5469 / 9778 |
| after | 1,598,538 | 1.562-1.609 M | 1241.2 | 2902 / 5414 / 9652 |

-3.1% throughput and +38 cycles/tx *despite* fewer instructions. Under
`panic = "abort"`, `std`'s `catch_unwind` already folds to `Ok(body)` with the
body inlined, so the closures were not being paid at those sites; replacing them
with a new generic wrapper changed inlining and layout for the worse. Reverted:
the tree measures identical to the committed baseline (1,649,680 vs 1,650,348
tx/s, overlapping ranges). The instruction count is not the objective function on
this path — the one change that won (+7.3%, 8.6) merged four phase bodies into a
single function, while three instruction-reducing edits lost (per-transaction RCU
scope -17%, teardown skip -3.9%, panic split -3.1%).

### 8.9 Abort-on-bug: scoped, attempted, and reverted pending the wider sweep

Policy chosen: remove panic containment from the commit path and let a bug
abort (the release profile is already `panic = "abort"`). The hot path was
converted first — `direct_protocol.rs` (phase driver, `Panicked` variant),
`item.rs` (`teardown_direct_plan`), `direct_commit.rs` and `lock.rs` (teardown
loops), and the `transaction.rs` driver entries, adapter-capability probes and
pooled-disposal helpers. It compiled and the tests exposed what the scoping
misses:

- **37 further containment sites** in `transaction.rs`'s general and terminal
  drivers (lines ~3391-4762): every phase callback (preflight, predicate
  upgrade, validation, acquire, install, release, cleanup) contains its own
  panic and classifies it by phase.
- **Machinery that only exists for containment**: `handle_acquire_panic`,
  `contain_unexpected_unwind` and `emergency_failure` (one pair per driver), the
  `Panicked(FailurePhase, payload)` variant, the `AdapterFaultKind::Panic`
  producers, and the `recover_after_callback_panic` chain
  (lock -> plan -> batch -> driver) with its quarantine bookkeeping.
- **Cross-crate test fallout**: `sto-core/tests/failure_protocol.rs` asserts the
  containment directly (install/release/finish panics preserving commit
  boundaries, payload-destructor panics, poisoning); `lock.rs` has ~40
  references to the recovery API and `mako-cache` ~33 to worker poisoning. Those
  tests cannot assert in-process recovery once aborts are the policy; they need
  subprocess-abort assertions instead.

Landing it half-way was not acceptable: containment would be inconsistent across
lanes and the tree would not compile. Everything was reverted and re-verified
(18/18 test suites pass; 1,655,789 vs 1,652,078 tx/s, 1197 vs 1202 cyc/tx against
the committed baseline binary).

Staged plan if pursued: (1) abort-on-bug for the certified singleton lane only,
keeping containment in the general/terminal/cache drivers — bounded, no
cross-crate test change; (2) sweep the 37 general-driver sites, delete the
panic-recovery machinery, and convert the `failure_protocol` panic tests to a
subprocess harness that asserts the child aborts with a phase-tagged message;
(3) measure under ABBA. Note the expectation from 8.8: swapping or deleting
containment wrappers has measured neutral-to-negative so far; the payoff in this
lane is expected to come from the monomorphization in (1), not from deleting the
catches.

### 8.10 Stage 1 implemented and rejected: merged certified singleton lane

Stage 1 was built: the certified one-item lane's three steps (preflight, plan
run, teardown) were merged and the intermediate unwind frames removed —
`cx.preflight`'s `commit_shape` dispatch replaced by the batch's own shape
flags, the singleton body driven by a new plain (uncontained) entry
(`run_singleton_body`), and the plan's singleton branch switched to it, so the
certified lane runs as plain calls while the general/terminal drivers keep their
containment.

Result: it compiles, and both the tests and the measurement reject it.

- **Tests**: the fault-injection suite covers this exact path, so abort-on-bug
  here breaks containment-asserting tests (`failure_protocol`: install/release
  panics preserving boundaries, payload-drop panics). Stage 2's test conversion
  is therefore a *prerequisite* of stage 1, not a follow-up.
- **Measurement** (ABBA, 16 samples per build, engine, 1 worker): 1,595,330 vs
  1,637,450 tx/s (-2.57%), 1245.5 vs 1212.2 cycles/tx, 2900 vs 2906 ins/tx —
  fewer instructions, more cycles, the same signature as every other
  layer-removal attempt on this path (8.7, 8.8, 8.9).

Reverted and re-verified after a clean rebuild: 18/18 test suites pass,
1,659,574 vs 1,655,362 tx/s, 1194.8 vs 1199.3 cycles/tx, both 2906 ins/tx. (An
intermediate re-measurement of a stale binary showed -6.4%; the fresh rebuild
recovered parity, which is worth remembering when comparing rebuilds on this
host.)

The four consecutive rejections now cover every source-level shape of this
change: wrapper swap, predicate skip, policy split, and merged plain-call lane.
The remaining ~300 ins/tx live in the erased *representation* — a `dyn` plan
stored per capability, a batch behind `Box<dyn ErasedItemBatch>`, and a lock
type that is a generic parameter rather than an associated type, which is what
prevents a C++-shaped monomorphic commit today. That representation change is
the outstanding work; no further local restructuring is supported by the
measurements.

## 9 Appendix

### A. Artifacts in this directory

| File | Purpose |
| ---- | ------- |
| `measure.sh` | frozen-pair matched controls + extended `perf stat` counters |
| `variants.sh` | baseline / `rcu` / `holder` alternation (ablation and null control) |
| `profile_ins.sh` | `perf record -e instructions:u` on the frame-pointer probe pair, window-filtered |
| `memcount.c` | `memcpy`/`memmove`/`memset` interposer with size buckets |
| `alloccount.c` | `malloc`/`calloc`/`realloc`/`free` interposer |
| `clockcount.c` | `gettimeofday`/`clock_gettime`/`time` interposer, per thread |
| `clockprobe.sh` | clock counters + `strace -c` syscall summary for both lanes |
| `clockbench.c` | microbenchmark that pins each clock call to its vDSO offset and cost |
| `engine_compare-items.rs` | the `--items` diagnostic harness (untracked in the worktree) |
| `patch_probe.py` | applies the same `--items` change to the residual probe harness |
| `build_probe_items.sh` | offline build of both `--items` arms |

Raw `perf.data` files, per-cell stdout/stderr and the probe binaries remain under
`/var/tmp/mako-attr-20260917.2` (profiles) and `/var/tmp/probe-mine` (items
build) on `zoo-002`.

### B. Reproduction commands

```bash
# matched controls (frozen ThinLTO pair)
ssh zoo-002 'sudo bash /var/tmp/attr-measure.sh /var/tmp/mako-attr-20260917.1'

# ablation and null control
ssh zoo-002 'sudo bash /var/tmp/attr-variants.sh'

# instruction profiles on the probe pair (frame pointers, PROFILE_WINDOW markers)
ssh zoo-002 'sudo bash /var/tmp/attr-profile-ins.sh /var/tmp/mako-attr-20260917.2'

# clock / copy / allocation counters for both lanes
ssh zoo-002 'sudo bash /var/tmp/clockprobe.sh'
ssh zoo-002 'cc -O1 -shared -fPIC -o /var/tmp/memcount.so /var/tmp/memcount.c -ldl'
LD_PRELOAD="$W/libjemalloc.so.2 /var/tmp/memcount.so" taskset -c 33-63 <bin> engine \
  --workers 1 --iterations 262144 --warmup 65536 --perf 0

# per-item amortization (after building both arms)
ssh zoo-002 'bash /var/tmp/build_probe_items.sh && sudo bash /var/tmp/items_matrix.sh'
```

Baselines: `LD_LIBRARY_PATH=/var/tmp/mako-engine-perf-20260912.iz63eX:<llvm lib>`,
`LD_PRELOAD=/var/tmp/mako-engine-perf-20260912.iz63eX/libjemalloc.so.2`.

### C. Engine-mode trace anchors (Rust lane, `engine_compare` mode `engine`)

- Driver: `crates/mako-cache-bench/src/bin/engine_compare.rs:295-343`.
- `LocalDb::trusted_bound_transaction` -> `attach` -> `worker::attach`:
  `crates/mako-engine-sto/src/{db.rs,txn.rs,worker.rs}` (`trusted_bound_transaction`
  297-309, `Transaction::begin` 222-256).
- `tx.put`: `crates/mako-engine-sto/src/txn.rs:477-556` -> `sto-masstree`
  `put_borrowed_with_previous_presence` (`lib.rs:2122`) -> `with_key_record_lookup`
  (`lib.rs:4451-4484`) -> `sto-core` `with_resolved_item` (`transaction.rs:1340-1377`)
  -> `with_item_inner_validated` (1740-1788) -> `prepare_typed_unique_batch`
  (2709-2760) -> `activate_typed_batch_item` (2595-2689) -> native lookup.
- Native admission per lookup: `src/mako/storage/mtree_abi.cc` `trusted_operation`
  (909-937), `point_get_validated` (941-975), `structure_read_guard` (254-302),
  `operation_rcu_guard` (528-537) -> `src/mako/rcu.h:358-401` (`ticker` guard,
  `depth_++`/`--`, `do_cleanup`).
- `tx.commit`: `txn.rs:741-751` -> `commit_report` -> `sto_core`
  `Transaction::commit` (`transaction.rs:1505`) -> `commit_with_optional_hook(None)`
  (1522-1544) -> `CommitDriver::run` (3394-3432) -> `run_direct` (3806-3844) ->
  `SingletonBody::{acquire,validate,install,release}`
  (`sto-core/src/singleton_direct.rs:52-172`) -> `BorrowedInjectiveLockCommitPlan`
  (`sto-core/src/direct_commit.rs:1929-2330`) -> `sto-masstree`
  `install_direct_record` (`lib.rs:9962-10014`) -> `CommittedStatePublication::publish`
  (`lib.rs:9380-9455`).
- Hook-free consequences: no `CommitHook` vtable calls, no HLC gate, no record
  encoding/CRC32C, no writeback queue, no `canonical_ops()` (all cache-path only).

C++ lane (for the contrasted numbers): `mako_rust_fast_txn_{begin,put,commit_and_destroy}`
in `src/mako/storage/mako_local_abi.cc` (begin 1345, put 3367, commit 4676) ->
`MassTrans::trans_write`/`handlePutFound`/`reallyHandlePutFound`
(`src/mako/sto/MassTrans.hh:361/1260/1140`) -> `Transaction::try_commit`
(`src/mako/sto/Transaction.cc:1067`) -> `MassTrans<...>::install`
(`MassTrans.hh:969-1066`, with `stuffed_str::replace` at
`src/mako/sto/stuffed_str.hh:166-180`) -> `Transaction::stop` (`Transaction.cc:672`)
-> `rcu_stop` through the `MassTrans::thread_init()` callbacks
(`MassTrans.hh:121-146`).
