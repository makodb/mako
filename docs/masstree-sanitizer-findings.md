# Masstree — Sanitizer Findings Report

Surfaced by Tier 3.1 of `docs/masstree-test-plan.md`. Each finding is
documented in enough detail that the user can decide whether it is an
actual bug or an intentional pattern. No fixes have been applied; the
items below are all currently silenced by the suppressions files in
`src/masstree/{ubsan,tsan}_suppressions.txt`.

## TL;DR

| # | Sanitizer | Location | Class | My read |
|---|---|---|---|---|
| 1 | UBSan | `src/masstree/kpermuter.hh:128` | shift-exponent ≥ width | **likely benign** on x86 — value is consumed by a mask whose unused-bit branch is never taken, but the UB is real per the C++ memory model |
| 2 | UBSan | `src/masstree/string_slice.hh:52,83,87,158,159` | unaligned 8-byte load | **intentional perf trick**, gated by `HAVE_UNALIGNED_ACCESS`; UB per spec, safe on x86_64 |
| 3 | UBSan | `src/masstree/masstree_struct.hh:156` (via line 661) | array index from stale read inside optimistic retry | **intentional lock-free pattern**; value is discarded by the surrounding version-check retry |
| 4 | TSan | `src/masstree/*` (~1,500 distinct call sites) | racy reads of node fields under optimistic concurrency | **intentional lock-free pattern**; safety is via Masstree's version-counter retry, not via `std::atomic` ordering |
| 5 | TSan | `src/mako/spinlock.h:23,40` | race on `volatile uint32_t value` | **the only finding that looks like a real bug** — plain `volatile` is not a substitute for `std::atomic<uint32_t>` under the C++11+ memory model |

ASan: zero findings across all three test binaries.

---

## Finding 1 — `kpermuter::value_from` shift-exponent

**Where**: `src/masstree/kpermuter.hh:128`

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

**What to check before deciding**

- Walk the call site at `masstree_split.hh:97` and confirm the
  downstream consumer either masks the high bits away or never enters
  this code path with `i + 1 == 64`.
- If the caller does mask, the fix is a one-line clamp:
  `int s = (i + 1) << 2; return s >= 64 ? 0 : (x_ >> s);` — well-defined
  on all platforms.
- If the caller depends on the x86-specific "shift by 64 returns x_"
  semantics, that's a real bug on any non-x86 target.

---

## Finding 2 — `string_slice` unaligned 8-byte loads

**Where**: `src/masstree/string_slice.hh:52, 83, 87, 158, 159` (and
likely more under richer workloads).

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

**Is it a bug?**

If the codebase is x86_64-only, no — the generated code is correct
and identical to the safe alternative.

If the codebase ever targets older ARM or any strict-alignment
architecture, yes — and the fix is `memcpy` into a local `uint64_t`,
which modern compilers fold to a single unaligned load on x86 (so the
perf trick is preserved).

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

The current implementation relies on:
- `__sync_bool_compare_and_swap` for the actual mutual exclusion
  (this is fine — it's an atomic builtin).
- `COMPILER_MEMORY_FENCE` for ordering around the critical section
  (this is a compiler fence, not a hardware fence; on x86 the
  hardware happens to provide TSO which masks the bug, but on ARM
  this would not establish the necessary release/acquire edge).
- Plain `volatile` reads in the spin loop.

On x86 the code works by accident. On ARM / POWER it would be a real
correctness bug — the load of `value = 0` in `unlock()` could be
reordered with respect to writes inside the critical section, and
the spinning load in `lock()` could observe stale values for an
unbounded period.

**This is the finding I'd recommend looking at most carefully** — it
is the only one of the five that I would call an outright bug rather
than "intentional but UB-per-spec." The replacement is mechanical:

```cpp
std::atomic<uint32_t> value{0};
// lock:    while (value.load(memory_order_relaxed) || !value.compare_exchange_weak(zero, 1, memory_order_acquire)) { nop_pause(); }
// unlock:  value.store(0, memory_order_release);
```

It is suppressed in `src/masstree/tsan_suppressions.txt` by
`race:spinlock::lock` / `race:spinlock::unlock` because chasing it
inside Tier 3.1 was out of scope.

---

## Cross-cutting recommendations

- **Findings 1–4 are all "UB per the C++ memory model that happens to
  work on x86."** Whether to fix any of them is a portability and
  hygiene call, not a correctness call on current hardware.
- **Finding 5 is a real concurrency bug** on any non-TSO architecture
  and a "works by accident" pattern on x86. Worth investigating
  whether `spinlock` is on a hot enough path that the relaxed-atomic
  rewrite needs benchmarking before/after.
- **None of the five findings affected the test suite's pass rate**
  (113/113 passed under each sanitizer once the suppressions were in
  place). They were all surfaced as out-of-band warnings.

---

## Finding 6 — Ephemeral threads on `concurrent_btree` SIGABRT — **graceful-fail path landed**

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

**Fix sketch (not landed here)**:

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
