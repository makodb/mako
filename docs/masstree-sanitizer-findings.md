# Masstree — Sanitizer Findings Report

Surfaced by Tier 3.1 of `docs/masstree-test-plan.md`. This report retains the
original evidence and records each finding's current disposition. The fixed
defects are not suppressed. Only Masstree's documented optimistic-read
findings remain in the UBSan and TSan suppression files.

## TL;DR

| # | Sanitizer | Location | Class | My read |
|---|---|---|---|---|
| 1 | UBSan | `src/masstree/kpermuter.hh:128` | shift-exponent ≥ width | **fixed** by returning zero before a shift at or beyond the word width; suppression removed |
| 2 | UBSan | `src/masstree/string_slice.hh` | unaligned typed load | **fixed** with a `memcpy`-based load helper that preserves optimized x86 code generation; suppression removed |
| 3 | UBSan | `src/masstree/masstree_struct.hh:156` (via line 661) | array index from stale read inside optimistic retry | **accepted known UB debt**; the retry discards the value but cannot make the prior access language-defined |
| 4 | TSan | `src/masstree/*` (~1,500 distinct call sites) | racy reads of node fields under optimistic concurrency | **accepted known UB debt** in the inherited x86 implementation; the later version retry does not legalize plain C++ data races |
| 5 | TSan | `src/mako/spinlock.h:23,40` | race on `volatile uint32_t value` | **fixed** with `std::atomic<uint32_t>` and explicit memory ordering; suppressions removed |
| 6 | Native stress | Mako `SiloRuntime` thread registration | monotonically exhausted 512-slot worker ID space | **graceful rejection added** for opted-in callers; recycling and the legacy aborting path remain deferred |
| 7 | ASan | `src/masstree/masstree_struct.hh` external key suffix comparisons | fixed-width read beyond an exact-length 11/12-byte caller buffer | **fixed** by using bounded `memcmp` for the external operand; no suppression added |
| 8 | UBSan | `src/mako/core.h` per-core raw storage | placement construction at an address less aligned than the stored type | **fixed** with type-derived storage alignment and lifetime-aware pointer recovery; no suppression added |
| 9 | TSan | `src/mako/spinbarrier.h` | plain polling read racing with an atomic builtin decrement; no publication edge | **fixed** with a C++ atomic release sequence and acquire wait; no suppression added |
| 10 | TSan | guarded C++ TPC-C `std::cout`/`std::cerr` path | concurrent mutation of shared stream formatting state | **fixed on the tested TPC-C path** with one output lock shared by formatted and machine records; no suppression added |
| 11 | TSan | libnuma `numa_node_to_cpus` cache reached by `rcu::pin_current_thread` | unsynchronized lazy topology-cache construction in libnuma 2.0.19 | **fixed at the call boundary** by serializing topology queries during thread setup; no suppression added |
| 12 | TSan | Masstree `threadinfo::gc_epoch_` | reclaimer reads racing with participant entry and exit; publication also lacked a portable entry barrier | **fixed** with layout-preserving atomic references and an ordered snapshot/publish/recheck protocol; no suppression added |

The historical ASan run reported zero findings across its three Masstree test
binaries. The Rust STO native sanitizer workflow now reruns the expanded
boundary and TPC-C gate on every relevant pull request.

The Rust boundary ASan result has an explicit leak qualification. The ordinary
workspace sweep leak-checks every case except the 30 exact intentional
transaction-frame quarantine cases in
`scripts/ci/rust_sto_quarantine_tests.txt`; those cases are rerun individually
with leak reporting disabled. The native FFI runner leak-checks 31 of 32 unit
cases and disables leak reporting only for
`tests::post_install_row_count_failure_marks_runtime_indeterminate`, which
deliberately retains an indeterminate frame after publication begins. The gate
audits every substring skip against the test inventory before applying it.

---

## Finding 1 — `kpermuter::value_from` shift-exponent

**Resolution**: fixed. `value_from` computes the requested shift and returns
zero when it reaches the `value_type` width. The obsolete `shift-base` and
`shift-exponent` suppressions were removed, so UBSan will detect a recurrence.

**Where**: `src/masstree/kpermuter.hh:128`

Original code:

```cpp
value_type value_from(int i) const {
    return x_ >> ((i + 1) << 2);
}
```

**Sanitizer output**

```
src/masstree/kpermuter.hh:128:19: runtime error: shift exponent 64 is
too large for 64-bit type 'value_type' (aka 'unsigned long')
```

**How it's hit**

`value_from(i)` is called from `Masstree::leaf::split_into` at
`masstree_split.hh:97`. The permuter packs `width = 15` 4-bit slots
plus a 4-bit size field, so `x_` is a 64-bit word. When `i == 15`
(i.e. the slot one past the last), the shift is `(15 + 1) << 2 == 64`.
Per the C++ standard, shifting a 64-bit type by 64 is undefined
behavior; on x86 the shift amount is reduced mod 64 by the hardware,
so the result is `x_ >> 0 == x_` (not the all-zero result one might
expect). UBSan reports it; the program continues.

**Why I think it's probably benign**

The caller in `split_into` uses `value_from(width - 1)` to obtain the
permuter's high zero region for shifting into the new node's
permuter. With `i == width - 1 == 14`, the shift is `60`, which is
valid. The UBSan trace shows `i == 15`, which only happens if the
permuter is configured with a width such that `i + 1 == 16` and the
shift evaluates to 64; in that path Masstree expects an all-zero
result, which is what the hardware actually produces by accident on
x86 in this specific case (shifting `x_` by 0 mod 64 returns `x_`, but
the caller masks the result to zero downstream — I have not verified
the masking, so this is the part that warrants a second look).

The implemented guard makes the intended all-zero result explicit and
well-defined on every target.

---

## Finding 2 — `string_slice` unaligned 8-byte loads

**Resolution**: fixed. All affected typed dereferences now use a local
`memcpy` helper. The obsolete `alignment:string_slice` suppression was removed,
so UBSan will detect any new unaligned typed load in this code.

**Where**: `src/masstree/string_slice.hh:52, 83, 87, 158, 159` (and
likely more under richer workloads).

Original code:

```cpp
#if HAVE_UNALIGNED_ACCESS
    if (len >= size)
        return *reinterpret_cast<const T *>(s);
#endif
```

**Sanitizer output**

```
src/masstree/string_slice.hh:52:20: runtime error: load of misaligned
address 0x... for type 'const unsigned long', which requires 8 byte
alignment
```

**Analysis**

This is an intentional perf trick gated by `HAVE_UNALIGNED_ACCESS`,
which is set by the upstream Masstree configure step on x86_64 (and
ARMv7+). The code reads 8 bytes from a possibly-unaligned char
pointer because x86_64 silently handles the unaligned load.

Per the C++ memory model this is UB regardless of platform: a
`reinterpret_cast<const uint64_t*>(p)` followed by `*` is undefined if
`p` is not 8-byte aligned. The fact that x86 tolerates it doesn't
remove the UB.

The `memcpy` implementation removes the C++ alignment violation and lets the
compiler select the appropriate efficient load for each architecture.

---

## Finding 3 — `internode::ikey` array index from stale read

**Where**: `src/masstree/masstree_struct.hh:156`, hit via line 661.

```cpp
// masstree_struct.hh:155–156
ikey_type ikey(int p) const {
    return ikey0_[p];
}

// masstree_struct.hh:657
inline int internode<P>::stable_last_key_compare(
        const key_type& k, nodeversion_type v, threadinfo& ti) const {
    while (1) {
        int cmp = compare_key(k, size() - 1);
        if (likely(!this->has_changed(v)))
            return cmp;
        v = this->stable_annotated(ti.stable_fence());
    }
}
```

**Sanitizer output**

```
src/masstree/masstree_struct.hh:156:16: runtime error: index -1 out
of bounds for type 'const ikey_type[15]'
```

**Analysis**

`stable_last_key_compare` is a textbook optimistic-concurrency dance:

1. Compute `compare_key(k, size() - 1)` against the current node.
2. Check whether the version counter has changed since we took the
   snapshot `v`. If unchanged, return `cmp`.
3. Otherwise refresh `v` and try again.

While the lock-free read of `size()` is in flight, a concurrent
writer can mid-update the node so that `size()` momentarily reads as
0, making `size() - 1` equal to -1. The reader then dereferences
`ikey0_[-1]` — which is a few bytes before the array. The result is
garbage, but the surrounding `has_changed(v)` check immediately
rejects it and the loop retries.

**Is it a bug?**

Algorithmically, no. Per the C++ memory model, yes — reading from
`ikey0_[-1]` is undefined behavior even if the result is discarded.

This is the standard tradeoff in published lock-free indexes: you
get speed via plain reads, you live with the C++ standard officially
calling your reads UB, and you rely on the runtime / hardware to not
weaponize that UB against you. The mainline Masstree implementation
chose this tradeoff explicitly.

The fix, if you want UB-clean code, is to bound the index before
dereferencing:

```cpp
int n = size();
int cmp = (n > 0) ? compare_key(k, n - 1) : -1;
```

— but the algorithm tolerates the stale `cmp` anyway because of the
version check, so the only correctness gain is portability across
hardware that traps on small negative array indices (which x86 does
not).

---

## Finding 4 — TSan races on Masstree node fields

**Where**: across the Masstree internals — top hot spots from the
concurrent stress run:

| Hits | Source line |
|---:|---|
| 1,584 | `masstree_insert.hh:64` |
| 524 | `masstree_struct.hh:136` |
| 405 | `masstree_split.hh:191` |
| 404 | `masstree_struct.hh:338` |
| 395 | `masstree_split.hh:218` |
| 293 | `masstree_insert.hh:151` |
| 280 | `masstree_remove.hh:167` |
| 260 | `compiler.hh:318` |
| 202 | `masstree_insert.hh:31` |
| 189 | `masstree_struct.hh:156` |

**Analysis**

These are reads of node-internal fields (permuter, version counter,
keylenx, parent pointer, etc.) that race with concurrent writers.
The reads are NOT protected by `std::atomic` ordering — they are
plain loads. Masstree's correctness argument is the version-counter
retry protocol: a reader reads the version, reads the fields, reads
the version again, and retries if the version changed. Any
"observed" inconsistent intermediate state is rejected by the second
version read.

Per the C++11+ memory model, the unprotected reads are technically a
data race regardless of the retry; the standard treats data races as
undefined behavior, not as "undefined if you act on the result." In
practice, compilers and hardware honor this code as written because
the relaxed atomic semantics it implicitly assumes happen to match
plain loads on x86.

The principled fix is significant: change the relevant fields to
`std::atomic<T>` and load them with `memory_order_relaxed` (or
`acquire`/`release` at the version-counter sync points). That would
make TSan happy and the code portable to weakly-ordered hardware
without surprises, at zero cost on x86. It is a non-trivial refactor
across the upstream headers.

For the test infrastructure, these are scoped out by
`src/masstree/tsan_suppressions.txt` at the source-file granularity
so that real new races elsewhere still surface.

---

## Finding 5 — `src/mako/spinlock.h` plain `volatile uint32_t` — **FIXED**

Resolved by rewriting `src/mako/spinlock.h` to use
`std::atomic<uint32_t>` with explicit acquire/release ordering
(`compare_exchange_weak(..., acquire, relaxed)` in `lock`,
`store(0, release)` in `unlock`, relaxed loads in the
test-and-test-and-set spin). `COMPILER_MEMORY_FENCE` was dropped
because the atomic orderings subsume it. The corresponding
`race:spinlock::lock` / `race:spinlock::unlock` entries in
`src/masstree/tsan_suppressions.txt` were removed; TSan runs of
test_masstree, test_masstree_property, test_masstree_concurrent,
and test_masstree_multi_instance all report 0 races afterwards.

The original analysis below is preserved for posterity.



**Where**: `src/mako/spinlock.h:22–52`

```cpp
class spinlock {
public:
  spinlock() : value(0) {}

  inline void lock() {
    uint32_t v = value;                                    // <- racy
    while (v || !__sync_bool_compare_and_swap(&value, 0, 1)) {
      nop_pause();
      v = value;                                           // <- racy
    }
    COMPILER_MEMORY_FENCE;
  }

  inline void unlock() {
    INVARIANT(value);
    value = 0;                                              // <- racy
    COMPILER_MEMORY_FENCE;
  }

  // ...
private:
  volatile uint32_t value;
};
```

**Sanitizer output**

```
WARNING: ThreadSanitizer: data race
  Read of size 4 at 0x... by thread T1:
    #0 spinlock::lock() src/mako/spinlock.h
    #1 ticker::tickerloop() src/mako/ticker.h:227
    ...
  Previous atomic write of size 4 at 0x... by main thread:
    #0 spinlock::lock() src/mako/spinlock.h:23
    #1 ticker::guard::guard(ticker&) src/mako/ticker.h:110
    ...
```

**Analysis**

Per the C++11+ memory model, `volatile` is **not** a substitute for
`std::atomic` for cross-thread synchronization. `volatile` prevents
the compiler from optimizing away or coalescing the access, but it
does not impose any memory ordering and does not establish a
happens-before edge with concurrent accesses on the same object.

The original implementation relied on:
- `__sync_bool_compare_and_swap` for the actual mutual exclusion
  (this is fine — it's an atomic builtin).
- `COMPILER_MEMORY_FENCE` for ordering around the critical section
  (this is a compiler fence, not a hardware fence; on x86 the
  hardware happens to provide TSO which masks the bug, but on ARM
  this would not establish the necessary release/acquire edge).
- Plain `volatile` reads in the spin loop.

On x86 the old code worked by accident. On ARM / POWER it would be a real
correctness bug — the load of `value = 0` in `unlock()` could be
reordered with respect to writes inside the critical section, and
the spinning load in `lock()` could observe stale values for an
unbounded period.

This was the only one of the first five findings classified as an outright bug
rather than an intentional optimistic-read pattern. Its replacement was
mechanical:

```cpp
std::atomic<uint32_t> value{0};
// lock:    while (value.load(memory_order_relaxed) || !value.compare_exchange_weak(zero, 1, memory_order_acquire)) { nop_pause(); }
// unlock:  value.store(0, memory_order_release);
```

It was initially suppressed by `race:spinlock::lock` and
`race:spinlock::unlock`. Both entries were removed after the atomic rewrite.

---

## Cross-cutting recommendations

- Findings 1, 2, 5, and 7–12 are fixed and unsuppressed. The sanitizer gate
  now treats a recurrence as a failure.
- Findings 3 and 4 remain accepted, explicitly qualified C++ UB debt in the
  inherited Masstree implementation. The filename-wide TSan suppressions can
  hide a new race in the same frames, so a passing TSan job means no
  unsuppressed finding under that reviewed list, not an absence of all races.
- In the original investigation, all 113 tests passed once the then-current
  suppressions were active. Current results are tied to workflow artifacts for
  the exact tested revision rather than this historical report.

---

## Finding 6 — Ephemeral threads on `concurrent_btree` SIGABRT — **graceful-fail path landed; not a Masstree issue**

**Important framing**: the bug lives in Mako's allocator layer, not
in Masstree. Pure upstream Masstree has no thread cap at all — its
`threadinfo::make()` just prepends to a per-context linked list.
The abort surfaces only when client code uses Mako's `mbtree`
wrapper (`concurrent_btree` / `single_threaded_btree`), which
routes Masstree's allocations through Mako's `rcu::s_instance` and
`coreid::core_id()` — and *those* are the things bound by
`SiloRuntime::NMAXCORES = 512`.

The contrast is locked in by tests:

  * `MasstreeMultiInstanceTest.PureMasstreeAcceptsUnboundedEphemeralThreads`
    spawns 5,000 ephemeral threads (10× Mako's cap) against a
    `Masstree::basic_table<PureParams>` with `threadinfo` from
    kvthread.hh. Passes cleanly in ~7 s.
  * `src/masstree/tests/repro_finding6.cc` runs the same pattern on
    `concurrent_btree` (Mako wrapper). Aborts after ≤2 cycles.
    Gated on `MAKO_REPRO_FINDING6=1` so CTest doesn't pick it up.

For Mako consumers who need to keep using `mbtree` and want a
graceful failure instead of an abort:



A graceful registration API has been added on top of the original
abort path (which is preserved for legacy callers):

```cpp
SiloRuntime* rt = SiloRuntime::Current();      // or your own Create()
if (!rt->try_register_current_thread()) {
    // Pool is exhausted — this thread cannot use masstree.
    // Refuse the request, or fall back to another data structure.
    return Error::TooManyThreads;
}
// Safe to call masstree ops from here on.
```

Helpers added (in `src/mako/core.{h,cc}` and
`src/mako/silo_runtime.{h,cc}`):

  * `coreid::try_current_core_id()` — non-lazy lookup, returns -1
    if the thread has no core_id for the current runtime. Never
    aborts.
  * `SiloRuntime::try_allocate_core_id()` — same as
    `allocate_core_id()` but returns -1 on cap exhaustion.
  * `SiloRuntime::try_register_current_thread()` — idempotent: binds
    the calling thread to this runtime (and its MasstreeContext) and
    reserves a core_id slot. Returns false iff the pool is full.

The original `allocate_core_id()` / lazy `coreid::core_id()` paths
still abort on cap exhaustion — backward-compat for existing
long-lived-thread callers that do not need a failure signal.

Tests (`test_masstree_multi_instance.cc`):

  * `TryRegisterCurrentThreadFailsGracefullyAtCap` — spawns
    NMAXCORES + 8 threads against a fresh runtime; asserts exactly
    NMAXCORES return true and 8 return false.
  * `TryRegisterCurrentThreadIsIdempotent` — verifies repeated calls
    from the same thread do not consume additional slots.

The original analysis below is preserved for posterity.



**Where**: any workload that repeatedly spawns a fresh `std::thread`,
performs a small batch of `concurrent_btree` ops, and exits. The
masstree-test-plan Tier 8 soak driver originally did this; the
abort surfaces within 1–5 s on a release build.

**Reproducer**: `src/masstree/tests/repro_finding6.cc`. Build with
`MAKO_REPRO_FINDING6=1 cmake ...` (gated so CTest does not pick it
up by default). Standalone run aborts at exit code 134 after ≤2
spawn cycles; under `gdb -batch` or `strace -f` it runs much
longer because thread setup is slowed enough that the underlying
counter takes longer to hit its cap.

**Root cause** (strace + `-k` stack walk pinpointed):

```
abort()
  ← SiloRuntime::allocate_core_id()       src/mako/silo_runtime.cc:113
  ← ...                                   repro_finding6 / libmako
```

```cpp
unsigned SiloRuntime::allocate_core_id() {
    unsigned id = core_count_.fetch_add(1, std::memory_order_acq_rel);
    ALWAYS_ASSERT(id < NMaxCores);
    return id;
}
```

`core_count_` is a **monotonic** counter. Each thread that ever
touches the masstree allocator path consumes one slot. The slot is
**never released** when the thread exits. `NMAXCORES = 1 << 9 = 512`
(see `src/mako/macros.h:50`). So spawn #513 trips the assert.

The "no diagnostic message" mystery is the release-build
`ALWAYS_ASSERT` macro (`src/mako/macros.h:72`):

```cpp
#define ALWAYS_ASSERT(expr) (likely((expr)) ? (void)0 : abort())
```

— bare `abort()`, no `write(2, ...)`, no fprintf. strace confirms
no stderr write happens before the `tgkill(... SIGABRT)`.

**Why gdb / strace mask the bug**: both slow per-thread setup enough
that the program executes more "useful" code between thread
creations, but the counter eventually still hits 512.

**Why long-lived threads work**: each thread allocates a core_id
once. The fixed test pool (4 writers + 2 readers + N scanners) is
nowhere near 512 IDs.

**Full slot-recycling fix (not landed)**:

The cleanest fix is a `thread_local` sentinel that releases the
allocated `core_id` on thread exit and a freelist in `SiloRuntime`
that `allocate_core_id` pops from before falling back to
`fetch_add`. Roughly:

```cpp
class SiloRuntime {
    std::atomic<unsigned> core_count_{0};
    rusty::Mutex<std::vector<unsigned>> free_ids_{{}};
public:
    unsigned allocate_core_id() {
        {
            auto guard = free_ids_.lock().unwrap();
            if (!guard->empty()) { unsigned id = guard->back(); guard->pop_back(); return id; }
        }
        unsigned id = core_count_.fetch_add(1, std::memory_order_acq_rel);
        ALWAYS_ASSERT(id < NMaxCores);
        return id;
    }
    void release_core_id(unsigned id) {
        free_ids_.lock().unwrap()->push_back(id);
    }
};

// Per-thread RAII sentinel:
struct CoreIdGuard {
    SiloRuntime* rt;  unsigned id;
    ~CoreIdGuard() { if (rt) rt->release_core_id(id); }
};
thread_local CoreIdGuard tl_core_id_guard;
```

The caller path that currently calls `allocate_core_id()` (chain
runs from `threadinfo::make` or the simple_threadinfo allocator)
would set `tl_core_id_guard = {this, allocate_core_id()}` on
first use.

The minimum-effort interim mitigation is to bump `NMAXCORES`
(the macro definition is one line in `src/mako/macros.h:50`),
but that just kicks the can — any consumer with >512 unique
thread lifetimes still hits it. The freelist fix is roughly 20
lines and should be the actual landing.

---

## Finding 7: exact-length external key over-read in `equals_sloppy` (fixed)

**Where**: the external-key suffix comparisons in
`src/masstree/masstree_struct.hh`, reached by the Rust fixed-read and resolved-
cache native tests with exact-length 11-byte and 12-byte key allocations.

Masstree's internal stringbag storage is padded and may safely use
`string_slice<uintptr_t>::equals_sloppy`. The compared `key_type` suffix can,
however, point to caller-owned storage ending exactly at the logical key
length. `equals_sloppy` rounds its final comparison up to a machine word, so it
read four bytes beyond those exact allocations. ASan reported the out-of-bounds
read at the native Rust/Masstree boundary.

Both comparisons now use `memcmp(s.s, ka.suffix().s, s.len)`. The internal
padded comparisons retain `equals_sloppy`, so the fix is limited to the operand
whose padding is not guaranteed. No suppression or leak exception applies to
this finding; the fixed-read and resolved-cache tests run under ordinary ASan.

---

## Finding 8: under-aligned per-core storage (fixed)

**Where**: the byte arrays backing `percore<T>` and `percore_lazy<T>` in
`src/mako/core.h`.

The arrays had element-sized capacity but only byte alignment. Placement-new
therefore constructed cache-line-aligned values, including `ticker::tickinfo`,
at addresses that did not satisfy the value type's alignment. UBSan reported a
constructor call on an address that was not 64-byte aligned.

The storage now has `alignas` derived from its actual element type. The lazy
slot wrapper is itself aligned as `T` and uses `std::launder` when recovering a
pointer to a constructed object. The
`CoreStorageTest.LazyPerCoreStorageHonorsOverAlignment` regression uses a
128-byte-aligned value so the contract remains visible even on platforms where
ordinary allocations happen to satisfy cache-line alignment. No suppression
applies.

---

## Finding 9: spin-barrier race and missing publication edge (fixed)

**Where**: `src/mako/spinbarrier.h`.

`count_down()` used a legacy atomic builtin, while `wait_for()` polled the same
word through a plain `volatile` read. Those accesses race in the C++ memory
model. More importantly, a waiter that observed zero had no defined happens-
before edge from setup writes made by every participant.

The count is now `std::atomic<size_t>`. Each decrement is a release RMW, which
forms one release sequence, and the waiter uses an acquire load. Observing zero
therefore publishes the setup performed before every decrement. The
`SpinBarrierTest.WaitingThreadObservesEveryParticipantWrite` regression checks
that consequence directly. No suppression applies.

---

## Finding 10: shared benchmark stream state (fixed)

**Where**: concurrent loader, worker, RPC, and result output in the active C++
TPC-C/`dbtest` path used by `sto_tpcc_bench`, principally `bench.cc`,
`bench.h`, `dbtest.cc`, `rpc_setup.cc`, and `tpcc.cc` under
`src/mako/benchmarks`.

Two local-shard loader sets can write to `std::cerr` concurrently. libc++'s
stream operations mutate formatting state, so TSan reported a race even when
the individual messages appeared intact. Independent locks around only the
machine-readable result would also allow formatted output to split or corrupt
that record.

`src/mako/benchmarks/benchmark_output.h` now provides a scoped stream proxy
that holds one process-wide mutex for the complete insertion expression. The
tested TPC-C path's human output and raw machine-record writes use the same
mutex. The lock protects diagnostic and result publication; it is not acquired
for transaction bookkeeping. Other benchmark drivers still contain direct
stream writes and require their own audit before they are run concurrently in
one process. No suppression applies to the TPC-C finding.

---

## Finding 11: libnuma topology-cache initialization race (fixed at boundary)

**Where**: `rcu::pin_current_thread()` in `src/mako/rcu.cc`, reached
concurrently by the two local-shard TPC-C loader sets.

libnuma 2.0.19 lazily fills its process-wide `node_cpu_mask_v2` cache from
`numa_node_to_cpus()`. Its own source describes this cache as slightly racy and
notes that locking would be preferable. TSan observed one loader copying from a
cached mask while another loader initialized that mask.

Mako now serializes `numa_node_of_cpu()`, `numa_node_to_cpus()`, and the related
affinity-mask setup within `rcu::pin_current_thread()`. Every concurrent Mako
caller that reached the reported cache does so through this boundary. The lock
is taken only while a thread establishes affinity, before measured transaction
execution, and no sanitizer suppression was added. The multishard slow-exit
CTest is the regression workload for this path. This repair does not wrap
arbitrary direct libnuma calls in other benchmark or dormant runtime paths;
those paths remain outside this finding's tested boundary.

---

## Finding 12: Masstree RCU participant publication (fixed)

**Where**: `threadinfo::rcu_start()`, `threadinfo::rcu_stop()`, and
`threadinfo::hard_rcu_quiesce()` in `src/masstree/kvthread.hh` and
`src/masstree/kvthread.cc`. The strict TSan multishard C++ TPC-C lifecycle test
reported a reclaimer reading one loader's `gc_epoch_` while that loader wrote
the same word.

`gc_epoch_` advertises a nonzero RCU read-side epoch, or zero while its worker
is quiescent. A reclaimer scans every registered participant and frees limbo
entries older than the oldest advertised epoch. Plain concurrent reads and
writes were therefore both a C++ data race and a possible premature-free path.
A release-only entry store would remove neither the formal publication gap nor
the weak-memory Store-to-Load reordering that matters here.

Changing the member type would disturb the anonymous-union cache-line layout
and would make `threadinfo`'s construction-time raw initialization invalid for
a nontrivial atomic member. Instead, short-lived
`std::atomic_ref<mrcu_epoch_type>` operations cover every executable access to
the naturally aligned raw word. Compile-time checks require the reference to
be aligned and lock-free.

Entry takes a sequentially consistent context-epoch snapshot, publishes it
with a sequentially consistent store, and rechecks the context epoch. It
repeats if an advancer overlapped publication, preventing a paused worker from
entering under an epoch that reclamation has already passed. Peer scans and context
epoch operations join the same total order. Exit clears the participant with a
release store after its protected accesses; owner-only arithmetic uses relaxed
loads. The constructor's plain zeroing remains before registration and
publication, and `threadinfo` allocations remain process-lifetime.

The same audit found a plain function-static flag in `threadinfo::make()` that
could race between concurrent attachers. A function-static `std::once_flag`
now publishes that assertion-only allocator initialization. The multishard
slow-exit CTest exercises both repairs under TSan. No suppression applies.
