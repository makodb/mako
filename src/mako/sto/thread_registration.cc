#include "sto/thread_registration.hh"

#include "masstree/kvthread.hh"
#include "masstree/masstree_context.h"
#include "sto/Transaction.hh"

#include <atomic>
#include <mutex>
#include <pthread.h>

namespace mako::silo {
namespace {

std::atomic<int> next_thread_id{0};
thread_local int current_thread_id = -1;
thread_local int current_runtime = 0;
std::once_flag epoch_once;
std::atomic<bool> epoch_ready{false};

MasstreeContext *sto_masstree_context() {
  // Every MassTrans threadinfo must be in one reclamation domain. A context
  // per adapter/runtime lets a reclaimer ignore active readers in another
  // context. This process-lifetime context deliberately unifies them.
  static auto *context = new MasstreeContext();
  return context;
}

}  // namespace

bool claim_thread_runtime(thread_runtime runtime) noexcept {
  const int requested =
      runtime == thread_runtime::native_mako
          ? 1
          : (runtime == thread_runtime::local_abi ? 2 : 3);
  if (current_runtime == 0) current_runtime = requested;
  return current_runtime == requested;
}

int try_allocate_thread_id() noexcept {
  if (current_thread_id >= 0) return current_thread_id;
  const int id = next_thread_id.fetch_add(1, std::memory_order_acq_rel);
  if (id >= MAX_THREADS) return -1;
  current_thread_id = id;
  return id;
}

bool ensure_epoch_runtime() noexcept {
  try {
    auto *context = sto_masstree_context();
    MasstreeContext::BindCurrentThread(context);
    std::call_once(epoch_once, [context] {
      // This is MassTrans::static_init(), centralised so native Mako and the
      // C ABI cannot race to replace the callback or start two advancers.
      Transaction::epoch_advance_callback = [context](unsigned) {
        globalepoch++;
        context->increment_epoch(2);
      };

      pthread_t advancer;
      if (pthread_create(&advancer, nullptr, Transaction::epoch_advancer,
                         nullptr) != 0) {
        return;
      }
      pthread_detach(advancer);
      epoch_ready.store(true, std::memory_order_release);
    });
  } catch (...) {
    return false;
  }
  return epoch_ready.load(std::memory_order_acquire);
}

}  // namespace mako::silo
