#include "sto/thread_registration.hh"

#include "masstree/kvthread.hh"
#include "masstree/masstree_context.h"
#include "silo_runtime.h"
#include "sto/Interface.hh"
#include "sto/Transaction.hh"

#include <atomic>
#include <mutex>

namespace mako::silo {
namespace {

thread_local int current_runtime = 0;
std::once_flag epoch_once;
std::atomic<bool> epoch_ready{false};

// Every STO participant shares the process-global Silo runtime's Masstree
// epoch domain. DB::Open, the replay pool, the Rust STO bridge, and the
// Rust-facing local ABI all bind this same context, so a reclamation scan
// sees every reader of every MassTrans table.
MasstreeContext *sto_masstree_context() {
  return SiloRuntime::GlobalDefault()->masstree_context();
}

}  // namespace

// @safe: thread-local bookkeeping only.
bool claim_thread_runtime(thread_runtime runtime) noexcept {
  const int requested =
      runtime == thread_runtime::native_mako
          ? 1
          : (runtime == thread_runtime::local_abi ? 2 : 3);
  if (current_runtime == 0) current_runtime = requested;
  return current_runtime == requested;
}

// @unsafe: TThread::assign_stable_id throws when the fixed tinfo space is
// exhausted; translate that into the documented -1 result.
int try_allocate_thread_id() noexcept {
  // TThread::assign_stable_id owns the one process-wide TThread::id counter.
  // Native Mako, the replay pool, the Rust STO bridge, and the local ABI all
  // draw from it, so two adapters can never hand out the same tinfo slot.
  // IDs are intentionally never reused: per-thread RCU state can outlive the
  // OS thread that created it.
  try {
    return TThread::assign_stable_id();
  } catch (...) {
    return -1;
  }
}

// @unsafe: binds Masstree thread-local context and starts a detached thread.
bool ensure_epoch_runtime() noexcept {
  try {
    auto *context = sto_masstree_context();
    MasstreeContext::BindCurrentThread(context);
    std::call_once(epoch_once, [context] {
      // This is MassTrans::static_init(), centralised so native Mako and the
      // Rust-facing local ABI install one callback and start one advancer.
      Transaction::set_epoch_advance_callback([context](unsigned) {
        globalepoch++;
        context->increment_epoch(2);
      });
      Transaction::start_epoch_advancer();
      epoch_ready.store(true, std::memory_order_release);
    });
  } catch (...) {
    return false;
  }
  return epoch_ready.load(std::memory_order_acquire);
}

}  // namespace mako::silo
