// Private test translation unit. Never linked by a measurement or release build.
#if !defined(MAKO_NATIVE_HEALTH_PROOF) || MAKO_NATIVE_HEALTH_PROOF != 1
#error "native health proof must be explicitly enabled for this test-only TU"
#endif

// Compile the real boundary, so the hook and public getter share its singleton.
// No opaque-layout casts, duplicate shadow health state, or recovery/reset seam.
#include "/home/users/shuai/mako/.claude/worktrees/masstree-rocks/src/mako/storage/mtree_abi.cc"

extern "C" mt_status mako_native_health_proof_poison() noexcept {
  mt_runtime &runtime = singleton_runtime();
  if (!runtime.acquired.load(std::memory_order_acquire)) {
    return MT_ERR_INVALID;
  }
  poison(&runtime);
  return MT_OK;
}
