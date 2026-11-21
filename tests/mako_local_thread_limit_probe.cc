// Process-isolated proof that STO's process-lifetime worker budget is exact.

#include "mako/storage/mako_local_abi.h"

#include <thread>

int main() {
  static_assert(MAKO_LOCAL_MAX_WORKERS == 460u);

  // Join each worker before starting the next one. This proves that IDs are
  // process-lifetime reservations, not merely a simultaneous-thread ceiling.
  for (uint32_t worker = 0; worker != MAKO_LOCAL_MAX_WORKERS; ++worker) {
    int first = MAKO_LOCAL_INTERNAL;
    int second = MAKO_LOCAL_INTERNAL;
    std::thread thread([&] {
      first = mako_local_thread_attach();
      second = mako_local_thread_attach();
    });
    thread.join();
    if (first != MAKO_LOCAL_OK || second != MAKO_LOCAL_OK) return 1;
  }

  int overflow = MAKO_LOCAL_INTERNAL;
  std::thread thread([&] { overflow = mako_local_thread_attach(); });
  thread.join();
  return overflow == MAKO_LOCAL_THREAD_LIMIT ? 0 : 2;
}
