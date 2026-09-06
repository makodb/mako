#ifndef _SPINBARRIER_H_
#define _SPINBARRIER_H_

#include <atomic>

#include "amd64.h"
#include "macros.h"
#include "util.h"

/**
 * Barrier implemented by spinning
 */

class spin_barrier {
public:
  spin_barrier(size_t n)
    : n(n)
  {
    ALWAYS_ASSERT(n > 0);
  }

  spin_barrier(const spin_barrier &) = delete;
  spin_barrier(spin_barrier &&) = delete;
  spin_barrier &operator=(const spin_barrier &) = delete;

  ~spin_barrier()
  {
    ALWAYS_ASSERT(n.load(std::memory_order_relaxed) == 0);
  }

  void
  count_down()
  {
    // Every decrement is a release RMW. A waiter that observes zero with
    // acquire ordering also observes every participant's setup through the
    // resulting release sequence.
    for (;;) {
      size_t copy = n.load(std::memory_order_relaxed);
      ALWAYS_ASSERT(copy > 0);
      if (n.compare_exchange_weak(copy, copy - 1,
                                  std::memory_order_release,
                                  std::memory_order_relaxed))
        return;
    }
  }

  void
  wait_for()
  {
    while (n.load(std::memory_order_acquire) > 0)
      nop_pause();
  }

private:
  std::atomic<size_t> n;
};

#endif /* _SPINBARRIER_H_ */
