#include "runtime/thread_registration.hh"
#include "runtime/worker_context.hh"

namespace mako::silo {
namespace { thread_local int current_runtime = 0; }

bool claim_thread_runtime(thread_runtime runtime) noexcept {
  const int requested = runtime == thread_runtime::native_mako ? 1 : 2;
  if (current_runtime == 0) current_runtime = requested;
  return current_runtime == requested;
}

int try_allocate_thread_id() noexcept {
  try { return TThread::assign_stable_id(); }
  catch (...) { return -1; }
}
} // namespace mako::silo
