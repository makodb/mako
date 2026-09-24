# C++ Multi-Core Optimization Techniques in Mako

This document provides a detailed reference for the multi-core and multi-threading
optimization techniques used throughout the Mako/Silo codebase. Each technique
includes the motivation, the implementation, code examples from the codebase, and
pitfalls to watch out for.

**Target audience:** Developers learning high-performance concurrent C++.

**Prerequisite knowledge:** Basic C++ (templates, RAII), basic concurrency concepts
(threads, mutexes, atomics).

---

## Table of Contents

1. [Per-Core Data Partitioning](#1-per-core-data-partitioning)
2. [Cache-Line Alignment and False Sharing Prevention](#2-cache-line-alignment-and-false-sharing-prevention)
3. [Custom Spinlocks](#3-custom-spinlocks)
4. [Memory Ordering (Acquire/Release Semantics)](#4-memory-ordering-acquirerelease-semantics)
5. [Compiler and CPU Memory Fences](#5-compiler-and-cpu-memory-fences)
6. [Lock-Free Data Structures](#6-lock-free-data-structures)
7. [Read-Copy-Update (RCU) and Epoch-Based Reclamation](#7-read-copy-update-rcu-and-epoch-based-reclamation)
8. [Thread-Local Storage (TLS)](#8-thread-local-storage-tls)
9. [CPU Prefetch Instructions](#9-cpu-prefetch-instructions)
10. [Branch Prediction Hints](#10-branch-prediction-hints)
11. [Per-CPU Memory Allocator](#11-per-cpu-memory-allocator)
12. [Optimistic Concurrency Control (Version Validation)](#12-optimistic-concurrency-control-version-validation)
13. [Stack-Allocated Small Containers](#13-stack-allocated-small-containers)
14. [Cooperative Scheduling (Fibers/Coroutines)](#14-cooperative-scheduling-fiberscoroutines)
15. [Work-Stealing Thread Pool](#15-work-stealing-thread-pool)
16. [Spin Barrier](#16-spin-barrier)
17. [Inline Assembly Atomic Primitives](#17-inline-assembly-atomic-primitives)
18. [Per-Site Runtime Isolation](#18-per-site-runtime-isolation)
19. [Compiler Intrinsic Memory Operations](#19-compiler-intrinsic-memory-operations)
20. [Summary: Hierarchy of Techniques](#20-summary-hierarchy-of-techniques)

---

## 1. Per-Core Data Partitioning

**Files:** `src/mako/core.h`, `src/mako/counter.h`, `src/mako/ticker.h`, `src/mako/rcu.h`

### The Problem

When multiple CPU cores write to the same shared data structure, the hardware cache
coherence protocol (e.g., MESI/MOESI) forces the cache line to bounce between cores.
Each write invalidates other cores' cached copies, causing **cache line bouncing**.
Even a simple shared counter under 8-core contention can drop to ~10% of single-core
throughput.

### The Solution

Give each core its own **private copy** of the data. Only aggregate across cores
when a global view is needed (which is rare).

### Implementation

```cpp
// src/mako/core.h

// Each element is padded to a full cache line to prevent false sharing.
template <typename T, bool CallDtor = false, bool Pedantic = true>
class percore {
public:
    // Access the current core's private copy (fast path).
    inline T& my() {
        return (*this)[coreid::core_id()];
    }

    // Access a specific core's copy (for aggregation).
    inline T& operator[](unsigned i) {
        INVARIANT(i < NMAXCORES);
        return elems()[i].elem;
    }

    inline size_t size() const { return NMAXCORES; }

protected:
    // Raw byte storage for NMAXCORES padded elements.
    // Each element occupies >= 64 bytes (one cache line).
    char bytes_[sizeof(util::aligned_padded_elem<T, Pedantic>) * NMAXCORES];
};
```

There is also a **lazy variant** that only constructs per-core objects on first access:

```cpp
// src/mako/core.h

template <typename T>
class percore_lazy : private percore<private_::buf<T>, false> {
    template <class... Args>
    inline T& my(Args&&... args) {
        return get(coreid::core_id(), std::forward<Args>(args)...);
    }

    template <class... Args>
    inline T& get(unsigned i, Args&&... args) {
        buf_t &b = this->elems()[i].elem;
        if (unlikely(!flags_[i])) {
            flags_[i] = true;
            T *px = new (&b.bytes_[0]) T(std::forward<Args>(args)...);
            return *px;
        }
        return *b.cast();
    }
};
```

### Usage Examples

**Event counters** (`src/mako/counter.h`) -- each core increments its own counter
with zero synchronization:

```cpp
struct event_ctx {
    percore<uint64_t, false, false> counts_;  // one counter per core
};

class event_counter {
    inline ALWAYS_INLINE void inc(uint64_t i = 1) {
        ctx_->counts_.my() += i;  // No atomic, no lock -- pure local write!
    }
};
```

To read the total, iterate and sum (rare, expensive, but safe):

```cpp
void event_ctx::stat(counter_data &d) {
    for (size_t i = 0; i < counts_.size(); i++)
        d.count_ += counts_[i];
}
```

**Ticker per-thread state** (`src/mako/ticker.h`):

```cpp
class ticker {
    percore<tickinfo> ticks_;  // per-core epoch tracking
};
```

**RCU per-thread sync** (`src/mako/rcu.h`):

```cpp
class rcu {
    percore_lazy<sync> syncs_;  // per-core RCU state, lazily initialized
};
```

### Key Insight

The fundamental trade-off: writes are perfectly parallel (each core writes to its
own cache line), but reads that need a global view must iterate all cores. This is
profitable when writes vastly outnumber global reads -- which is typically the case
for counters, epoch tracking, and memory allocator state.

### Pitfalls

- **Memory usage:** `NMAXCORES` (512) copies of each element exist, each padded to
  64 bytes. A `percore<uint64_t>` uses 512 * 64 = 32 KB even though only 8 bytes per
  core is "useful."
- **Core ID must be stable:** If a thread migrates between cores mid-operation, it
  may read stale data from the old core. Pin threads to cores (see Section 11).

---

## 2. Cache-Line Alignment and False Sharing Prevention

**Files:** `src/mako/macros.h`, `src/mako/util.h`

### The Problem: False Sharing

Two unrelated variables that happen to reside on the same 64-byte cache line will
cause cache line bouncing if different cores write to them. This is called
**false sharing** -- the hardware doesn't know the variables are independent.

```
Cache line (64 bytes):
[ counter_A (8 bytes) | counter_B (8 bytes) | padding (48 bytes) ]
         ^                      ^
     Core 0 writes          Core 1 writes
     --> INVALIDATES Core 1's copy of the entire line!
```

### The Solution

1. **Align** structures to cache line boundaries.
2. **Pad** structures to fill the entire cache line.

### Implementation

```cpp
// src/mako/macros.h

#define CACHELINE_SIZE 64
#define LG_CACHELINE_SIZE __builtin_ctz(CACHELINE_SIZE)  // = 6 (log2(64))

// Force a variable or struct to be aligned to a cache line boundary.
#define CACHE_ALIGNED __attribute__((aligned(CACHELINE_SIZE)))

// Add invisible padding to fill to the next cache line boundary.
// Uses a zero-length array with alignment -- the compiler rounds up the
// containing struct's size without adding addressable bytes.
#define CACHE_PADOUT  \
    char __XCONCAT(__padout, __COUNTER__)[0] __attribute__((aligned(CACHELINE_SIZE)))
```

The workhorse container:

```cpp
// src/mako/util.h

template <typename T, bool Pedantic = true>
class aligned_padded_elem {
public:
    T elem;
    CACHE_PADOUT;  // Pads to cache line boundary after elem.

private:
    // Compile-time check: every instance is a multiple of 64 bytes.
    inline void __cl_asserter() const {
        static_assert((sizeof(*this) % CACHELINE_SIZE) == 0, "xx");
    }
} CACHE_ALIGNED;  // Starts at a cache line boundary.
```

### Usage in the Codebase

```cpp
// Global singletons placed on their own cache lines:
static rcu s_instance CACHE_ALIGNED;      // src/mako/rcu.h:293
static ticker s_instance CACHE_ALIGNED;   // src/mako/ticker.h:188

// SpinLock's atomic flag on its own cache line:
// src/srpc/base/threading.hpp:104
std::atomic<bool> locked_ alignas(64);

// RCU sync checks alignment at construction:
// src/mako/rcu.h:138
ALWAYS_ASSERT(((uintptr_t)this % CACHELINE_SIZE) == 0);
```

### How `CACHE_PADOUT` Works

This is a subtle trick. Consider:

```cpp
struct Foo {
    int x;            // 4 bytes
    CACHE_PADOUT;     // Expands to: char __padout0[0] __attribute__((aligned(64)));
};
// sizeof(Foo) == 64, NOT 4!
```

The zero-length array `char[0]` occupies no addressable space, but
`__attribute__((aligned(64)))` forces the *next* address after `x` to be aligned
to 64 bytes. The compiler inserts implicit padding of 60 bytes between `x` and the
zero-length array, making `sizeof(Foo) == 64`.

### Pitfalls

- **Wastes memory:** Every padded element uses 64 bytes minimum. A `percore` of
  512 padded 8-byte integers uses 32 KB.
- **Only works for static/stack allocation:** Heap-allocated objects need
  `aligned_alloc` or `posix_memalign` -- regular `malloc` only guarantees 16-byte
  alignment.
- **x86-specific:** `CACHELINE_SIZE = 64` is correct for all modern x86 CPUs, but
  some ARM chips use 128-byte cache lines.

---

## 3. Custom Spinlocks

**Files:** `src/mako/spinlock.h`, `src/srpc/base/threading.hpp`, `src/srpc/base/threading.cpp`

### When to Use Spinlocks vs Mutexes

| Property | Spinlock | Mutex (`pthread_mutex`) |
|----------|----------|------------------------|
| Wait mechanism | Busy-wait (spins) | Kernel sleep/wakeup |
| Context switch | Never | Yes (on contention) |
| Overhead (uncontended) | ~10 ns | ~25 ns |
| Overhead (contended) | Wastes CPU | Efficient (sleeps) |
| Best for | Short critical sections (<1 us) | Long critical sections (>10 us) |

### Implementation: Mako's CAS Spinlock

```cpp
// src/mako/spinlock.h

class spinlock {
    volatile uint32_t value;

    inline void lock() {
        // TTAS: Test-and-Test-and-Set pattern.
        // First READ the value (cheap, can be served from cache).
        uint32_t v = value;
        while (v || !__sync_bool_compare_and_swap(&value, 0, 1)) {
            // If locked, spin with PAUSE instruction.
            nop_pause();
            v = value;
        }
        COMPILER_MEMORY_FENCE;
    }

    inline void unlock() {
        INVARIANT(value);
        value = 0;
        COMPILER_MEMORY_FENCE;
    }
};
```

**Why TTAS (Test-and-Test-and-Set)?**

A naive "test-and-set" calls CAS on every iteration. CAS generates a **write-intent**
bus transaction even when it fails. Under contention, N cores all hammering CAS
generates O(N) bus transactions per iteration -- severe interconnect congestion.

TTAS first *reads* the value. If it's locked, the read can be served from the
local cache (shared state, no bus traffic). Only when we see 0 do we attempt CAS.
This reduces bus traffic from O(N) to O(1) per acquisition.

### Implementation: SRPC's Adaptive Spinlock

The SRPC framework has a more sophisticated spinlock with **exponential backoff**:

```cpp
// src/srpc/base/threading.cpp

void SpinLock::lock() {
    // Phase 1: Try immediate CAS (fast path).
    bool expected = false;
    if (locked_.compare_exchange_strong(expected, true,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        return;
    }

    // Phase 2: Spin for up to 1000 iterations.
    int wait = 1000;
    while ((wait-- > 0) && locked_.load(std::memory_order_relaxed)) {
        asm volatile("pause");
    }

    // Phase 3: Fall back to sleeping (50 us intervals).
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = 50000;  // 50 microseconds
    expected = false;
    while (!locked_.compare_exchange_weak(expected, true,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
        nanosleep(&t, nullptr);
        expected = false;
    }
}
```

This three-phase design handles:
- **No contention:** CAS succeeds immediately (~10 ns).
- **Brief contention:** Spin-wait resolves it in microseconds.
- **Extended contention:** Sleep to avoid wasting CPU.

### The `PAUSE` Instruction

```cpp
// src/mako/amd64.h
inline ALWAYS_INLINE void nop_pause() {
    __asm volatile("pause" : :);
}
```

On x86, `PAUSE` (encoding: `F3 90`, also `REP NOP`) does two things:

1. **De-pipeline the loop:** Without PAUSE, the CPU speculatively executes many
   iterations of the spin loop. When the lock becomes available, the speculative
   state must be flushed (~100 cycle penalty). PAUSE tells the CPU "this is a
   spin-wait, don't speculate."

2. **Power reduction:** PAUSE puts the core in a low-power state for ~40 clock
   cycles (varies by microarchitecture). This reduces power consumption and
   thermal throttling.

### Pitfalls

- **Spinlocks are dangerous for long hold times:** If the lock holder is preempted
  by the OS scheduler while holding a spinlock, all waiting cores spin uselessly
  for the entire preemption duration (up to 10 ms!). Pin threads to cores and use
  short critical sections.
- **Priority inversion:** A high-priority thread can spin-wait on a lock held by a
  low-priority thread that the OS won't schedule. Use mutexes if priority matters.
- **Fair vs unfair:** The spinlocks in this codebase are *unfair* -- there is no
  guarantee about ordering. A thread can repeatedly lose the CAS race (starvation).
  This is acceptable for short critical sections.

---

## 4. Memory Ordering (Acquire/Release Semantics)

**Files:** `src/mako/circbuf.h`, `src/mako/ticker.h`, `src/srpc/base/threading.hpp`

### The Problem

Modern CPUs and compilers **reorder** memory operations for performance. A store
followed by a load might actually execute in the opposite order. This is invisible
to a single thread (it sees a consistent view), but *other threads* can observe the
reordering.

### The C++ Memory Model

C++11 defines six memory orderings, from cheapest to most expensive:

| Ordering | Cost (x86) | Guarantee |
|----------|-----------|-----------|
| `relaxed` | Free | No ordering guarantees |
| `consume` | Free | Data-dependent ordering (rarely used) |
| `acquire` | Free on x86* | All reads/writes after this are visible after the acquire |
| `release` | Free on x86* | All reads/writes before this are visible before the release |
| `acq_rel` | Free on x86* | Both acquire and release |
| `seq_cst` | ~5-20 ns on x86 | Total order across all threads (uses `MFENCE` or `LOCK`) |

\* On x86, `acquire` and `release` are "free" because x86's TSO memory model already
provides them. On ARM/POWER, they generate explicit fence instructions.

### Usage in the Codebase

**Circular buffer** (`src/mako/circbuf.h`) -- producer/consumer with acquire/release:

```cpp
// Producer: store pointer with RELEASE semantics.
// All writes that prepared the object happen-before this store.
buf_[icur].store(p, std::memory_order_release);

// Consumer: load pointer with ACQUIRE semantics.
// All reads of the object happen-after this load.
Tp *ret = buf_[tail_].load(std::memory_order_acquire);
```

The pattern:
1. Producer writes to object fields.
2. Producer does a `release` store to publish the pointer.
3. Consumer does an `acquire` load to read the pointer.
4. Consumer reads object fields -- guaranteed to see the producer's writes.

**SpinLock** (`src/srpc/base/threading.hpp`):

```cpp
// Lock: ACQUIRE -- all critical section reads happen AFTER the lock.
locked_.compare_exchange_strong(expected, true, std::memory_order_acquire, ...);

// Unlock: RELEASE -- all critical section writes happen BEFORE the unlock.
locked_.store(false, std::memory_order_release);
```

**Ticker epoch tracking** (`src/mako/ticker.h`):

```cpp
// Read global epoch with acquire (see all writes from previous epochs).
inline uint64_t global_current_tick() const {
    return current_tick_.load(std::memory_order_acquire);
}

// Update per-thread epoch with release (publish all writes from this epoch).
ti.current_tick_.store(tick_, std::memory_order_release);
```

### When to Use `relaxed`

Only when ordering doesn't matter -- e.g., statistics counters:

```cpp
// It's fine if other threads see a slightly stale value.
while (locked_.load(std::memory_order_relaxed)) {
    asm volatile("pause");
}
```

### Key Rule

**Acquire-Release pairs create synchronization points:**
- Thread A does writes, then a `release` store.
- Thread B does an `acquire` load, then reads.
- Thread B is guaranteed to see all of Thread A's writes.

This is cheaper than `seq_cst` because it doesn't require a global ordering --
only pairwise ordering between the releasing and acquiring threads.

### Pitfalls

- **Over-using `seq_cst`:** Default `std::atomic` operations use `seq_cst`, which
  is correct but expensive. For producer-consumer patterns, `acquire`/`release`
  suffices.
- **Under-ordering:** Using `relaxed` where ordering matters causes data races that
  are nearly impossible to debug (they may only manifest on certain CPU
  architectures, under high load, non-deterministically).
- **x86 gives false confidence:** Code that works on x86 (which has strong TSO
  ordering) may break on ARM/POWER. Always use the correct C++ memory order, even
  if it's "free" on your platform.

---

## 5. Compiler and CPU Memory Fences

**Files:** `src/mako/macros.h`, `src/mako/masstree/compiler.hh`

### Compiler Fence (Zero CPU Cost)

```cpp
// src/mako/macros.h:69
#define COMPILER_MEMORY_FENCE asm volatile("" ::: "memory")

// src/mako/masstree/compiler.hh:94
inline void fence() {
    asm volatile("" : : : "memory");
}
```

This emits **zero machine instructions.** The `"memory"` clobber tells the compiler
"assume all memory has been modified." This prevents the compiler from:

- Reordering loads/stores across the fence.
- Caching values in registers across the fence.
- Eliminating "redundant" loads (which may not be redundant from another thread's
  perspective).

### CPU Fence (Hardware Cost)

```cpp
// src/mako/masstree/compiler.hh:116
inline void memory_fence() {
    asm volatile("mfence" : : : "memory");  // x86 full memory barrier
}
```

`MFENCE` is a **hardware instruction** that:
1. Drains the store buffer (all pending writes become visible).
2. Prevents any load/store reordering across the fence.
3. Cost: ~10-30 ns on modern x86.

### Relaxed Fence with PAUSE

```cpp
// src/mako/masstree/compiler.hh:111
inline void relax_fence() {
    asm volatile("pause" : : : "memory");  // Spin-loop hint + compiler fence
}
```

This combines the PAUSE instruction (for spin-loop efficiency) with a compiler fence.

### Backoff Fence (Exponential Backoff)

```cpp
// src/mako/masstree/compiler.hh:147
struct backoff_fence_function {
    int count_ = 0;
    void operator()() {
        // Execute (count_ + 1) PAUSE instructions.
        for (int i = count_; i >= 0; --i)
            relax_fence();
        // Exponential backoff: 1, 3, 7, 15, 15, 15, ...
        count_ = ((count_ << 1) | 1) & 15;
    }
};
```

This is used in Masstree's lock acquisition to reduce bus traffic under contention.
Each failed attempt increases the backoff duration exponentially (capped at 16 PAUSE
instructions).

### When to Use What

| Need | Solution |
|------|----------|
| Prevent compiler reordering only | `COMPILER_MEMORY_FENCE` / `fence()` |
| Prevent CPU + compiler reordering | `memory_fence()` (`MFENCE`) |
| Spin-loop iteration | `relax_fence()` (PAUSE) |
| Spin-loop with backoff | `backoff_fence_function` |

---

## 6. Lock-Free Data Structures

**Files:** `src/mako/circbuf.h`, `src/deptran/concurrentqueue.h`

### Circular Buffer (MPSC -- Multiple Producers, Single Consumer)

```cpp
// src/mako/circbuf.h

template <typename Tp, unsigned int Capacity>
class circbuf {
    std::atomic<Tp*> buf_[Capacity];  // Array of atomic pointers
    std::atomic<unsigned> head_;       // CAS-advanced by producers
    std::atomic<unsigned> tail_;       // Only modified by consumer
};
```

**Enqueue (multiple producers):**

```cpp
void enq(Tp *p) {
retry:
    // Step 1: Read current head position.
    unsigned icur = head_.load(std::memory_order_acquire);

    // Step 2: Check if the slot is empty.
    if (buf_[icur].load(std::memory_order_acquire)) {
        nop_pause();  // Slot occupied, spin.
        goto retry;
    }

    // Step 3: Atomically advance head to claim this slot.
    unsigned inext = (icur + 1) % Capacity;
    if (!head_.compare_exchange_strong(icur, inext, std::memory_order_acq_rel)) {
        nop_pause();  // Another producer won the race, retry.
        goto retry;
    }

    // Step 4: Publish the pointer (release store).
    buf_[icur].store(p, std::memory_order_release);
}
```

**Dequeue (single consumer -- no CAS needed):**

```cpp
Tp* deq() {
    // Spin until a slot is filled.
    while (!buf_[tail_.load(std::memory_order_acquire)]
            .load(std::memory_order_acquire))
        nop_pause();

    Tp *ret = buf_[tail_.load(std::memory_order_acquire)]
               .load(std::memory_order_acquire);

    // Clear the slot and advance tail (only one consumer).
    buf_[postincr(tail_)].store(nullptr, std::memory_order_release);
    return ret;
}
```

The key design insight: the **two-phase enqueue** (reserve slot via CAS, then fill
it) means that the slot transitions through three states:

```
nullptr (empty) --> head advanced past it (reserved) --> pointer stored (filled)
```

This prevents two producers from writing to the same slot.

### Moodycamel Concurrent Queue (MPMC)

The codebase also includes the **moodycamel lock-free queue**
(`src/deptran/concurrentqueue.h`), which is a highly optimized multi-producer,
multi-consumer queue with per-producer blocks and efficient memory reclamation.
It is used in the Deptran transaction framework.

### Pitfalls

- **ABA problem:** CAS can succeed spuriously if a value changes from A -> B -> A.
  The circular buffer avoids this because pointers are unique.
- **Memory reclamation:** Lock-free structures can't simply `delete` nodes that other
  threads might be reading. Solutions: RCU (Section 7), hazard pointers, or
  epoch-based reclamation.
- **Bounded vs unbounded:** `circbuf` is bounded (fixed capacity). If producers
  outpace the consumer, `enq()` blocks (spins). Size the capacity based on expected
  throughput.

---

## 7. Read-Copy-Update (RCU) and Epoch-Based Reclamation

**Files:** `src/mako/rcu.h`, `src/mako/rcu.cc`, `src/mako/ticker.h`, `src/mako/pxqueue.h`

### The Problem: Safe Memory Reclamation

In a concurrent system, freeing an object that another thread might still be reading
causes a use-after-free. Traditional solutions (reference counting, garbage
collection) have high overhead. RCU provides a near-zero-overhead solution for
read-heavy workloads.

### The Design

RCU has three components:

1. **Readers** enter a "critical section" declaring they're reading shared data.
   This is a trivial increment of a depth counter -- no locks, no atomics.

2. **Writers** publish new data and defer freeing old data. The old data is placed
   in a deletion queue tagged with the current epoch.

3. **Epoch advancement** -- a background thread (the **ticker**) periodically
   advances a global epoch. Once ALL readers have moved past the epoch when an
   object was "freed," the object can be actually deallocated.

### The Ticker (Epoch Advancement)

```cpp
// src/mako/ticker.h

class ticker {
    percore<tickinfo> ticks_;         // Per-core epoch tracking
    std::atomic<uint64_t> current_tick_;
    std::atomic<uint64_t> last_tick_inclusive_;

    // Background thread loop (runs as daemon).
    void tickerloop() {
        for (;;) {
            nanosleep(tick_us);  // 40 ms between ticks

            // Advance global epoch.
            const uint64_t last_tick = non_atomic_fetch_add(current_tick_, 1UL);
            const uint64_t cur_tick  = last_tick + 1;

            // Wait for ALL threads to catch up to the previous tick.
            for (each core's tickinfo) {
                if (thread hasn't advanced yet) {
                    // Acquire its lock and advance it.
                    lock_guard<spinlock> lg(ti.lock_);
                    ti.current_tick_.store(cur_tick, std::memory_order_release);
                }
            }

            // Safe to reclaim everything up to last_tick.
            last_tick_inclusive_.store(last_tick, std::memory_order_release);
        }
    }
};
```

### The RCU Guard (RAII)

```cpp
// src/mako/rcu.h

template <bool DoCleanup>
class scoped_rcu_base {
public:
    scoped_rcu_base(rcu& rcu_instance)
      : rcu_(&rcu_instance),
        sync_(&rcu_instance.mysync()),
        guard_(rcu_instance.get_ticker())
    {
        sync_->depth_++;  // Enter critical section (no atomic needed).
    }

    ~scoped_rcu_base() {
        const unsigned new_depth = --sync_->depth_;
        guard_.destroy();
        if (new_depth || !DoCleanup)
            return;
        // Outermost region exited: run deferred cleanup.
        sync_->do_cleanup();
    }
};

typedef scoped_rcu_base<true> scoped_rcu_region;
```

### Deferred Freeing

```cpp
// src/mako/rcu.cc

void rcu::free_with_fn(void *p, deleter_t fn) {
    sync &s = mysync();
    uint64_t cur_tick = 0;
    const bool is_guarded = get_ticker().is_locally_guarded(cur_tick);
    INVARIANT(is_guarded);

    // Schedule deletion after ALL threads move past (cur_tick + 1).
    s.queue_.enqueue(delete_entry(p, fn), to_rcu_ticks(cur_tick + 1));
}
```

### Cleanup Logic

```cpp
// src/mako/rcu.cc

void rcu::sync::do_cleanup() {
    const uint64_t clean_tick_exclusive = impl_->cleaning_rcu_tick_exclusive();
    // clean_tick_exclusive == the tick that ALL threads have completed.

    // Move entries older than clean_tick from queue_ to scratch_.
    scratch_.empty_accept_from(queue_, clean_tick);

    // Actually free them.
    for (auto it = scratch_.begin(); it != scratch_.end(); ++it) {
        it->run(*this);  // Calls free() or custom deleter.
    }
    scratch_.clear();
}
```

### Usage Pattern

```cpp
{
    scoped_rcu_region guard;  // Enter RCU critical section.

    // Read shared data -- guaranteed to be valid for the duration.
    Foo *p = shared_ptr.load(std::memory_order_acquire);
    p->do_something();

    // "Free" the old version (deferred until safe).
    rcu::s_instance.free(old_ptr);
}  // Guard destroyed -- cleanup runs if we're the outermost scope.
```

### Partitioned Queue (`pxqueue.h`)

RCU uses a **partitioned queue** for deferred deletions:

```cpp
// src/mako/pxqueue.h

template <typename T, size_t N>
struct basic_px_group {
    basic_px_group *next_;
    vec<T, N> pxs_;        // Batch of N entries
    uint64_t rcu_tick_;     // Epoch when these entries were queued
};
```

Entries are grouped by epoch tick. Cleanup processes entire groups at a time,
reducing per-entry overhead.

### Pitfalls

- **Long critical sections stall reclamation:** If a thread holds an RCU guard for
  a long time, no objects can be freed across the entire system. Keep RCU regions
  short.
- **Memory can accumulate:** Under write-heavy workloads, the deletion queue can grow
  large between cleanup passes (every 40 ms by default).
- **Not a general-purpose lock:** RCU protects *read access* only. Writers must use
  separate synchronization (e.g., spinlocks) among themselves.

---

## 8. Thread-Local Storage (TLS)

**Files:** `src/mako/core.h`, `src/mako/masstree/masstree_context.h`,
`src/srpc/reactor/reactor.h`

### The Mechanism

Thread-local storage gives each thread its own copy of a variable. Access is
essentially a single pointer dereference through the FS/GS segment register
(on x86-64), making it as fast as a global variable read.

### Usage

**Core ID caching** (`src/mako/core.h`):

```cpp
class coreid {
    static __thread int tl_core_id;      // -1 if not assigned
    static __thread int tl_runtime_id;   // which runtime allocated it

    static inline unsigned core_id() {
        // Fast path: already assigned.
        if (unlikely(tl_core_id == -1 || tl_runtime_id != runtime_id)) {
            tl_core_id = allocate_from_runtime(runtime);  // Slow path.
            tl_runtime_id = runtime_id;
        }
        return tl_core_id;  // ~1 ns access
    }
};
```

**Thread-local reactor** (`src/srpc/reactor/reactor.h`):

```cpp
class Reactor {
    static thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_;
    static thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_;
    static thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_coro_th_;
};
```

### `__thread` vs `thread_local`

| Feature | `__thread` (GCC extension) | `thread_local` (C++11) |
|---------|---------------------------|------------------------|
| Initialization | Zero-initialized only | Constructor/destructor support |
| Types | POD/scalar only | Any type |
| Performance | Slightly faster (no init check) | May have lazy init overhead |

The codebase uses `__thread` for primitive types (core IDs) and `thread_local`
for complex types (Reactor, Masstree context).

---

## 9. CPU Prefetch Instructions

**Files:** `src/mako/prefetch.h`

### The Problem

A main-memory access takes ~100 ns. During a B-tree traversal, each node lookup
is a pointer chase that can't be pipelined -- the CPU must wait for the current
node to arrive before reading the next pointer.

### The Solution

**Software prefetch** tells the CPU to start fetching a cache line *before* you need
it, hiding the memory latency behind useful computation.

### Implementation

```cpp
// src/mako/prefetch.h

// Prefetch a single cache line.
static inline ALWAYS_INLINE void prefetch(const void *ptr) {
    typedef struct { char x[CACHELINE_SIZE]; } cacheline_t;
    asm volatile("prefetcht0 %0" : : "m" (*(const cacheline_t *) ptr));
}

// Prefetch an entire object (up to 4 cache lines = 256 bytes).
template <typename T>
static inline ALWAYS_INLINE void prefetch_object(const T *ptr) {
    // Skip the first cache line (already accessed by the caller).
    for (unsigned i = CACHELINE_SIZE;
         i < std::min(sizeof(*ptr), 4 * CACHELINE_SIZE);
         i += CACHELINE_SIZE)
        prefetch((const char *)ptr + i);
}

// Prefetch [ptr, ptr + n) with manual loop unrolling.
static inline ALWAYS_INLINE void prefetch_bytes(const void *p, size_t n) {
    const char *ptr = (const char *)p;
    const void *pend = std::min(ptr + n, ptr + 4 * CACHELINE_SIZE);

    // Round down to nearest cache line.
    ptr = (const char *)round_down<uintptr_t, LG_CACHELINE_SIZE>((uintptr_t)ptr);

    // Manually unrolled: 3 iterations max.
    ptr += CACHELINE_SIZE;
    if (ptr < pend) prefetch(ptr);
    ptr += CACHELINE_SIZE;
    if (ptr < pend) prefetch(ptr);
    ptr += CACHELINE_SIZE;
    if (ptr < pend) prefetch(ptr);
}
```

### Why Manual Unrolling?

The compiler may not unroll the loop in `prefetch_bytes` because the early-exit
conditions (`ptr < pend`) make the iteration count data-dependent. Manual unrolling
avoids branch prediction overhead for a tight, known-small loop.

### Prefetch Levels

`prefetcht0` loads into L1 cache (fastest, but evicts existing data). Other options:
- `prefetcht1`: Load into L2 (less disruptive to L1).
- `prefetcht2`: Load into L3.
- `prefetchnta`: Non-temporal (don't pollute caches, for streaming access).

### Usage: Masstree B-Tree Traversal

```cpp
// During tree traversal, prefetch the next node while processing the current one.
// macros.h:9
#define BTREE_NODE_PREFETCH
```

When enabled, each B-tree node lookup issues a prefetch for the child node before
doing key comparisons. By the time the comparisons are done, the child node is
already in L1.

### Pitfalls

- **Premature prefetch:** If you prefetch too early, the data may be evicted before
  you use it. If too late, the memory access still stalls.
- **Over-prefetching:** Each prefetch consumes memory bandwidth. Prefetching more
  than ~4 cache lines ahead provides diminishing returns.
- **Useless on L1 hits:** If the data is already in cache, the prefetch is wasted
  work (though its cost is negligible).

---

## 10. Branch Prediction Hints

**Files:** `src/mako/macros.h`, `src/mako/masstree/compiler.hh`

### The Mechanism

```cpp
// src/mako/macros.h:66-67
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
```

These tell the compiler which branch is expected to be taken. The compiler uses this
to:

1. **Code layout:** Place the common case as fall-through (no jump). The rare case
   is placed in a separate section of the binary. This improves **instruction cache**
   locality.

2. **Branch prediction:** Modern CPUs predict branches based on history, but for the
   first encounter, the prediction is usually "fall-through." `likely`/`unlikely`
   ensures the common case aligns with this default prediction.

### Usage

```cpp
// Fast path: core ID already assigned (99.99% of calls).
// src/mako/core.h:37
if (unlikely(tl_core_id == -1 || tl_runtime_id != runtime_id)) {
    tl_core_id = allocate_from_runtime(runtime);  // Rare slow path.
}
return tl_core_id;

// Fast path: arena already initialized.
// src/mako/rcu.h:186
inline void ensure_arena(size_t arena) {
    if (likely(arenas_[arena]))
        return;
    arenas_[arena] = allocator::AllocateArenas(pin_cpu_, arena);  // Rare.
}

// Fast path: small vector hasn't overflowed.
// src/mako/silo_small_vector.h:67
inline size_t size() const {
    if (unlikely(large_elems))
        return large_elems->size();  // Rare overflow to heap.
    return n;
}
```

### Impact

Typically 1-5% throughput improvement on branch-heavy hot paths. The cost of a
mispredicted branch on modern x86 is ~15-20 cycles.

---

## 11. Per-CPU Memory Allocator

**Files:** `src/mako/allocator.h`, `src/mako/rcu.h`, `src/mako/rcu.cc`

### The Problem

`malloc`/`free` use a global heap protected by locks. Under multi-threaded
allocation-heavy workloads, this becomes a bottleneck.

### The Solution

Each CPU has its own **freelist-based allocator** with 32 size classes. Allocation
is a pointer pop; deallocation is a pointer push. **No locking, no contention.**

### Implementation

```cpp
// src/mako/rcu.cc -- allocation from per-CPU arenas

void* rcu::sync::alloc(size_t sz) {
    if (pin_cpu_ == -1)
        return malloc(sz);  // Fallback for unpinned threads.

    auto [allocsz, arena] = allocator::ArenaSize(sz);  // Determine size class.

    if (arena >= allocator::MAX_ARENAS) {
        return malloc(sz);  // Too large, use system allocator.
    }

    ensure_arena(arena);  // Ensure this size class has memory.

    // Pop from freelist (single pointer chase).
    void *p = arenas_[arena];
    arenas_[arena] = *reinterpret_cast<void**>(p);  // Follow "next" pointer.
    return p;
}

void rcu::sync::dealloc(void *p, size_t sz) {
    if (!allocator::ManagesPointer(p)) {
        ::free(p);
        return;
    }

    auto [allocsz, arena] = allocator::ArenaSize(sz);

    // Push onto freelist (single pointer write).
    *reinterpret_cast<void**>(p) = arenas_[arena];
    arenas_[arena] = p;
    deallocs_[arena]++;
}
```

### The Freelist Trick

Each freed block's first 8 bytes are reused as a "next" pointer, forming an
intrusive linked list. This means:

- **Zero overhead per block** -- no separate metadata.
- **O(1) alloc/dealloc** -- just pointer read/write.
- **Zero fragmentation within a size class** -- all blocks are the same size.

### Size Classes

```cpp
// src/mako/allocator.h
static const size_t LgAllocAlignment = 4;  // 16-byte alignment
static const size_t AllocAlignment = 16;
static const size_t MAX_ARENAS = 32;

// Arena 0: 16 bytes, Arena 1: 32 bytes, ..., Arena 31: 512 bytes.
static inline std::pair<size_t, size_t> ArenaSize(size_t sz) {
    const size_t allocsz = round_up<size_t, LgAllocAlignment>(sz);
    const size_t arena = allocsz / AllocAlignment - 1;
    return {allocsz, arena};
}
```

### NUMA Awareness

```cpp
// src/mako/rcu.cc
void rcu::pin_current_thread(size_t cpu) {
    sync &s = mysync();
    s.set_pin_cpu(cpu);

    // Pin to the NUMA node containing this CPU.
    auto node = numa_node_of_cpu(cpu);
    numa_run_on_node(node);
    sched_yield();  // Ensure the pin takes effect.

    s.do_release();  // Release any memory from the wrong NUMA node.
}
```

NUMA (Non-Uniform Memory Access) means each CPU has "local" memory that's faster
to access (~80 ns) than "remote" memory on another socket (~150 ns). By pinning
threads and allocating from the local node, memory access latency is halved.

---

## 12. Optimistic Concurrency Control (Version Validation)

**Files:** `src/mako/masstree/nodeversion.hh`

### The Pattern

Instead of locking a node for reading, readers:
1. Read the version number.
2. Read the node's data.
3. Re-read the version number.
4. If the version changed, a writer interfered -- retry from step 1.

This is called an **optimistic lock** or **sequence lock**.

### Implementation

```cpp
// src/mako/masstree/nodeversion.hh

template <typename P>
class nodeversion {
    value_type v_;  // Version word (packed bit fields)

    // Wait until the node is not being modified.
    template <typename SF>
    nodeversion<P> stable(SF spin_function) const {
        value_type x = v_;
        while (x & P::dirty_mask) {  // Locked, inserting, or splitting?
            spin_function();
            x = v_;
        }
        acquire_fence();  // Ensure subsequent reads see consistent data.
        return x;
    }

    // Check if the version has changed since a previous read.
    bool has_changed(nodeversion<P> x) const {
        fence();
        return (x.v_ ^ v_) > P::lock_bit;
    }

    // Writer: lock the node.
    template <typename SF>
    nodeversion<P> lock(nodeversion<P> expected, SF spin_function) {
        while (1) {
            if (!(expected.v_ & P::lock_bit)
                && bool_cmpxchg(&v_, expected.v_, expected.v_ | P::lock_bit))
                break;
            spin_function();
            expected.v_ = v_;
        }
        acquire_fence();
        return expected;
    }

    // Writer: unlock and advance version.
    void unlock(nodeversion<P> x) {
        if (x.v_ & P::splitting_bit)
            x.v_ = (x.v_ + P::vsplit_lowbit) & P::split_unlock_mask;
        else
            x.v_ = (x.v_ + ((x.v_ & P::inserting_bit) << 2)) & P::unlock_mask;
        release_fence();
        v_ = x.v_;
    }
};
```

### The Version Word Bit Layout

The version word packs multiple flags into a single integer:

```
Bit fields in v_:
  [lock_bit]        = 1 bit  -- write lock held
  [inserting_bit]   = 1 bit  -- insertion in progress
  [splitting_bit]   = 1 bit  -- node split in progress
  [deleted_bit]     = 1 bit  -- node has been deleted
  [isleaf_bit]      = 1 bit  -- leaf vs interior node
  [root_bit]        = 1 bit  -- is root node
  [version counter] = remaining bits -- incremented on each modification
```

The `dirty_mask = lock_bit | inserting_bit | splitting_bit`. If any dirty bit is
set, the reader spins until the writer finishes.

### Why This Is Faster Than Reader-Writer Locks

With `pthread_rwlock`:
- **Read lock:** Atomic increment of reader count (~10 ns, causes cache line bounce).
- **Read unlock:** Atomic decrement (~10 ns).

With optimistic versioning:
- **Read "lock":** One plain read of the version word (~1 ns from L1).
- **Read "unlock":** One plain read + comparison (~1 ns).

Under high read concurrency, this is **10x faster** because readers never modify
shared state (no cache line bouncing).

---

## 13. Stack-Allocated Small Containers

**Files:** `src/mako/silo_small_vector.h`, `src/mako/small_unordered_map.h`

### The Problem

`std::vector` and `std::unordered_map` always allocate on the heap. For
transaction read/write sets that are typically small (< 100 entries), this means
a `malloc` + `free` per transaction -- thousands of allocator calls per second.

### The Solution: Small Buffer Optimization (SBO)

```cpp
// src/mako/silo_small_vector.h

template <typename T, size_t SmallSize = SMALL_SIZE_VEC>  // SmallSize = 128
class silo_small_vector {
    size_t n;                         // Current number of elements
    large_vector_type *large_elems;   // Heap fallback (usually nullptr)
    T small_elems_[SmallSize];        // Stack-allocated storage

    inline void push_back(const T &t) {
        if (unlikely(large_elems)) {
            large_elems->push_back(t);
        } else if (unlikely(n == SmallSize)) {
            // Overflow: migrate to heap.
            large_elems = new large_vector_type(begin(), end());
            large_elems->push_back(t);
        } else {
            small_elems_[n++] = t;
        }
    }

    inline size_t size() const {
        if (unlikely(large_elems))  // Branch hint: almost never taken.
            return large_elems->size();
        return n;
    }
};
```

### Performance Impact

- **Common case (n < 128):** Zero heap allocations. All data on the stack (L1 cache).
- **Rare case (n >= 128):** Falls back to `std::vector` with heap allocation.

For TPC-C transactions, which typically touch 5-15 records, this eliminates
all dynamic allocation in the hot path.

---

## 14. Cooperative Scheduling (Fibers/Coroutines)

**Files:** `src/srpc/reactor/fiber_impl.h`, `src/srpc/reactor/reactor.h`

### The Problem

OS thread context switches cost ~1-10 us (save/restore registers, flush TLB, kernel
transition). For an RPC system handling thousands of concurrent requests, each
waiting on network I/O, this overhead dominates.

### The Solution: User-Space Fibers

Fibers (also called green threads or coroutines) are user-space threads that
**cooperatively yield** to each other. A fiber context switch is just
saving/restoring a few registers -- ~10-100 ns, no kernel involvement.

### Implementation

```cpp
// src/srpc/reactor/fiber_impl.h

class Fiber {
    rusty::Cell<Status> status_{INIT};
    rusty::RefCell<rusty::Option<rusty::Box<boost_coro_task_t>>> boost_coro_task_{};

    // Yield: voluntarily give up the CPU to the reactor.
    void yield_() const;

    // Resume: reactor resumes this fiber.
    void continue_() const;

    // Create and immediately start running a new fiber.
    template <typename Func>
    static rusty::Rc<Fiber> create_run(Func&& func, ...);
};
```

### How It Works with the Reactor

1. A fiber issues an RPC and calls `yield_()`.
2. The reactor saves the fiber's stack/registers and picks another runnable fiber.
3. When the RPC response arrives, the reactor marks the waiting fiber as runnable.
4. On the next scheduling round, the reactor calls `continue_()` to resume it.

All of this happens **in user space** -- the OS sees a single thread.

---

## 15. Work-Stealing Thread Pool

**Files:** `src/srpc/base/threading.cpp`

### The Design

Each worker thread has its own private queue. When a worker's queue is empty, it
**steals** from other workers' queues:

```cpp
// src/srpc/base/threading.cpp

void ThreadPool::run_thread(int id_in_pool) {
    // Randomized stealing order (avoids all workers stealing from thread 0).
    std::vector<int> steal_order(n_);
    // ... shuffle steal_order ...

    for (;;) {
        switch (stage) {
        case 0: case 2:
            // Try own queue (fast path).
            if (q_[id_in_pool].try_pop(&job)) { stage = 0; }
            else { stage++; }
            break;

        case 1:
            // Brief sleep (adaptive: 1 us - 50 us).
            nanosleep(&sleep_req, nullptr);
            stage++;
            break;

        case 3:
            // Steal from other workers' queues.
            for (int i = 0; i < n_; i++) {
                if (steal_order[i] != id_in_pool) {
                    if (q_[steal_order[i]].try_pop_but_ignore_invalid(&job)) {
                        stage = 0;
                        break;
                    }
                }
            }
            if (stage != 0) stage++;
            break;

        case 4:
            // Block on own queue (last resort).
            job = q_[id_in_pool].pop();
            stage = 0;
            break;
        }

        // Adaptive sleep: decrease on success, increase on failure.
        if (stage == 0)
            sleep_req.tv_nsec = clamp(sleep_req.tv_nsec - 1000, min, max);
        else
            sleep_req.tv_nsec = clamp(sleep_req.tv_nsec + 1000, min, max);
    }
}
```

### Key Design Decisions

- **Randomized steal order:** Prevents all idle workers from contending on the same
  victim queue.
- **Adaptive sleep:** Under high load, sleep interval shrinks toward 1 us. Under
  low load, it grows toward 50 us. This balances latency vs CPU waste.
- **Staged fallback:** try_pop -> sleep -> try_pop -> steal -> blocking pop. Each
  stage is progressively more expensive but more likely to find work.
- **Death pill:** Shutdown sends a null Box to each worker's queue. Workers exit
  when they dequeue it.

---

## 16. Spin Barrier

**Files:** `src/mako/spinbarrier.h`

A barrier synchronizes N threads -- all must arrive before any can proceed. The
spin variant avoids kernel transitions:

```cpp
// src/mako/spinbarrier.h

class spin_barrier {
    volatile size_t n;

    void count_down() {
        for (;;) {
            size_t copy = n;
            ALWAYS_ASSERT(copy > 0);
            if (__sync_bool_compare_and_swap(&n, copy, copy - 1))
                return;
        }
    }

    void wait_for() {
        while (n > 0)
            nop_pause();  // Spin with PAUSE until all threads arrive.
    }
};
```

Used for synchronizing worker thread startup in benchmarks -- all threads must be
initialized before the benchmark timer starts.

---

## 17. Inline Assembly Atomic Primitives

**Files:** `src/mako/masstree/compiler.hh`

### Why Not Just Use `std::atomic`?

Masstree predates C++11 and uses direct x86 assembly for atomic operations. The
assembly gives precise control over which instructions are emitted:

```cpp
// src/mako/masstree/compiler.hh

// Atomic exchange (XCHG -- always has implicit LOCK prefix on x86).
template <typename B>
struct sized_compiler_operations<4, B> {
    static inline type xchg(type* object, type new_value) {
        asm volatile("xchgl %0,%1"
                     : "+r" (new_value), "+m" (*object));
        B()();  // Barrier function object.
        return new_value;
    }

    // Compare-and-swap (LOCK CMPXCHG -- returns old value).
    static inline type val_cmpxchg(type* object, type expected, type desired) {
        asm volatile("lock; cmpxchgl %2,%1"
                     : "+a" (expected), "+m" (*object)
                     : "r" (desired) : "cc");
        B()();
        return expected;
    }

    // Atomic fetch-and-add (LOCK XADD).
    static inline type fetch_and_add(type* object, type addend) {
        asm volatile("lock; xaddl %0,%1"
                     : "+r" (addend), "+m" (*object) : : "cc");
        B()();
        return addend;
    }

    // Atomic OR (LOCK OR).
    static inline void atomic_or(type* object, type addend) {
        asm volatile("lock; orl %0,%1"
                     : "=r" (addend), "+m" (*object) : : "cc");
        B()();
    }
};
```

The `LOCK` prefix makes the instruction atomic by locking the cache line. The `B()()`
call executes a configurable barrier function (can be a no-op, compiler fence, or
full memory fence) after the operation.

---

## 18. Per-Site Runtime Isolation

**Files:** `src/mako/silo_runtime.h`

### The Design

In multi-shard-single-process mode, each shard gets its own `SiloRuntime` with
**completely isolated** subsystems:

```cpp
// src/mako/silo_runtime.h

class SiloRuntime {
    // Each runtime has its own:
    AllocatorState alloc_state_;     // Memory allocator (separate mmap regions)
    std::atomic<unsigned> core_count_;  // Core ID space
    ticker ticker_;                  // Epoch advancement thread
    rcu rcu_;                        // Deferred memory reclamation
    MasstreeContext masstree_ctx_;   // B-tree state

    // Thread binding (thread-local pointer).
    static rusty::Arc<SiloRuntime> Create();
    static void BindCurrentThread(SiloRuntime* runtime);
    static SiloRuntime* Current();  // Get current thread's runtime.
};
```

### Why Isolation Matters

Without per-site isolation, all shards in a single process would share one global
epoch (ticker), one RCU system, and one allocator. The ticker's background thread
must wait for ALL registered threads -- if shard A has a slow thread, shard B's
memory reclamation is stalled.

With per-site isolation, each shard's ticker only waits for its own threads. Memory
allocators are independent. There is **zero cross-shard contention** on any hot-path
data structure.

---

## 19. Compiler Intrinsic Memory Operations

**Files:** `src/mako/macros.h`

```cpp
// src/mako/macros.h:108-118

#ifdef USE_BUILTIN_MEMFUNCS
#define NDB_MEMCPY __builtin_memcpy
#define NDB_MEMSET __builtin_memset
#else
#define NDB_MEMCPY memcpy
#define NDB_MEMSET memset
#endif
```

### Why Builtins?

When the size argument is a **compile-time constant**, `__builtin_memcpy` allows
the compiler to:

1. **Inline the operation entirely** -- no function call overhead.
2. **Use SIMD instructions** (SSE/AVX) for large, aligned copies.
3. **Eliminate the copy** if the compiler can prove the source/destination are the
   same or the result is unused.

For a 16-byte copy (common for key/value pairs), the compiler emits a single
`movdqu` instruction instead of a function call + loop.

### Additional Compiler Hints

```cpp
// src/mako/macros.h

#define ALWAYS_INLINE __attribute__((always_inline))
// Force inlining even when -Os is used. Critical for hot-path functions
// like spinlock lock/unlock, counter increment, and allocator alloc/free.

#define NEVER_INLINE __attribute__((noinline))
// Prevent inlining of large, cold functions to keep the hot path's
// instruction cache footprint small.
```

---

## 20. Summary: Hierarchy of Techniques

Ordered from most impactful to least impactful for typical multi-core workloads:

| Rank | Technique | Impact | Cost |
|------|-----------|--------|------|
| 1 | **Per-core partitioning** | Eliminates contention entirely | Memory (NMAXCORES copies) |
| 2 | **Lock-free data structures** | Eliminates blocking | Complexity, ABA problem |
| 3 | **RCU epoch reclamation** | Readers never block | Deferred memory growth |
| 4 | **Optimistic versioning** | 10x faster than rwlock for reads | Retry on write conflict |
| 5 | **Per-CPU allocator** | Zero-contention allocation | NUMA-aware setup |
| 6 | **Cache-line padding** | Eliminates false sharing | Memory waste |
| 7 | **Spinlocks** | No kernel transition | CPU waste under contention |
| 8 | **NUMA-aware allocation** | Halves memory latency | Requires thread pinning |
| 9 | **Stack-allocated containers** | Zero malloc in hot path | Stack size limits |
| 10 | **Fibers** | 100x cheaper context switch | Cooperative discipline |
| 11 | **Prefetch instructions** | Hides memory latency | Timing sensitivity |
| 12 | **Memory ordering** | Avoids `MFENCE` cost | Portability risk |
| 13 | **Branch hints** | 1-5% on branch-heavy code | Negligible |
| 14 | **Compiler intrinsics** | Better codegen for memcpy etc. | None |

### The Overarching Philosophy

1. **Avoid sharing** state between cores whenever possible (per-core partitioning).
2. When you must share, **avoid locking** (lock-free, RCU, optimistic versioning).
3. When you must lock, **minimize duration** (spinlocks, short critical sections).
4. **Respect the cache hierarchy** (alignment, prefetch, NUMA awareness).
5. **Help the compiler** (likely/unlikely, ALWAYS_INLINE, builtins).

### Recommended Reading

- *The Art of Multiprocessor Programming* by Herlihy & Shavit -- The definitive
  textbook on concurrent data structures and synchronization.
- *Silo: Speedy Transactions in Multicore In-Memory Databases* (SOSP'13) -- The
  paper that introduced many of these techniques; Mako descends from Silo.
- *Masstree: A Cache-Friendly Mashup of Tries and B-Trees* (EuroSys'12) -- The
  concurrent B-tree used in this codebase.
- *RCU Usage in the Linux Kernel* by Paul McKenney -- Comprehensive guide to
  Read-Copy-Update.
- Intel 64 and IA-32 Architectures Optimization Reference Manual -- Hardware
  details for PAUSE, prefetch, memory ordering on x86.