#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#include <atomic>
#include <stdint.h>

#include "amd64.h"
#include "macros.h"
#include "util.h"

// Hand-rolled spinlock. Used by ticker / rcu / scoped_rcu_base
// machinery as a low-overhead alternative to pthread_mutex_t when
// the critical section is short and contention is expected to be
// rare.
//
// History: previous revisions stored the lock state as a plain
// `volatile uint32_t` with a separate `__sync_bool_compare_and_swap`
// for the actual mutual exclusion. That worked by accident on x86's
// TSO memory model — `volatile` is a compiler-only barrier under
// the C++11+ memory model, so on weakly-ordered hardware
// (ARM / POWER) the spin-loop load could observe stale values for
// an unbounded time and the unlock store could reorder past writes
// inside the critical section. TSan correctly flagged it (see
// docs/masstree-sanitizer-findings.md, Finding 5).
//
// This rewrite uses `std::atomic<uint32_t>` with explicit memory
// orderings:
//   * lock:   compare_exchange_weak(..., acquire, relaxed)
//   * unlock: store(0, release)
//   * relaxed loads in the test-and-test-and-set spin
//
// The acquire on lock pairs with the release on unlock so writes
// inside the critical section by the previous owner happen-before
// any read by the new owner. No COMPILER_MEMORY_FENCE needed —
// the atomic ordering subsumes it.
class spinlock {
public:
  spinlock() : value(0) {}

  spinlock(const spinlock &) = delete;
  spinlock(spinlock &&) = delete;
  spinlock &operator=(const spinlock &) = delete;

  inline void
  lock()
  {
    // Test-and-test-and-set: spin on a relaxed load while the lock
    // is held, then attempt the CAS once it appears free. The
    // outer loop handles both the case where the CAS spuriously
    // fails (compare_exchange_weak) and the case where a different
    // thread won the race.
    while (true) {
      while (value.load(std::memory_order_relaxed) != 0) {
        nop_pause();
      }
      uint32_t expected = 0;
      if (value.compare_exchange_weak(expected, 1,
                                      std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
        return;
      }
      nop_pause();
    }
  }

  inline bool
  try_lock()
  {
    uint32_t expected = 0;
    // Use the strong form: a spurious failure here would falsely
    // report "lock unavailable" to the caller.
    return value.compare_exchange_strong(expected, 1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed);
  }

  inline void
  unlock()
  {
    INVARIANT(value.load(std::memory_order_relaxed) != 0);
    value.store(0, std::memory_order_release);
  }

  // just for debugging
  inline bool
  is_locked() const
  {
    return value.load(std::memory_order_relaxed) != 0;
  }

private:
  std::atomic<uint32_t> value;
};

#endif /* _SPINLOCK_H_ */
