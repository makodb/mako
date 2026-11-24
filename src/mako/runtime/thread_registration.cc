#include "runtime/thread_registration.hh"
#include <atomic>

namespace mako::silo {
namespace {
thread_local int current_runtime = 0;
thread_local int stable_id = -1;
std::atomic<unsigned> next_stable_id{0};
// Native RCU slots are process-lived. Never recycle a slot onto another thread.
constexpr unsigned max_registered_threads = 460;
}

bool claim_thread_runtime(thread_runtime runtime) noexcept {
  const int requested = runtime == thread_runtime::native_mako ? 1 : 2;
  if (current_runtime == 0) current_runtime = requested;
  return current_runtime == requested;
}

int try_allocate_thread_id() noexcept {
  if (stable_id >= 0) return stable_id;
  unsigned next = next_stable_id.load(std::memory_order_relaxed);
  while (next < max_registered_threads) {
    if (next_stable_id.compare_exchange_weak(next, next + 1,
                                           std::memory_order_relaxed)) {
      stable_id = static_cast<int>(next);
      return stable_id;
    }
  }
  return -1;
}
} // namespace mako::silo
