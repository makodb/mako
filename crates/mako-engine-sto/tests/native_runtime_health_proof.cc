// Test-only boundary replacement. No production target compiles this file.
#if !defined(MAKO_NATIVE_RUNTIME_HEALTH_PROOF) || MAKO_NATIVE_RUNTIME_HEALTH_PROOF != 1
#error "this translation unit requires the explicit native health proof build"
#endif

// The helper and the public getter must use the actual production singleton.
// No assumed opaque layout, replacement health flag, or reset is introduced.
#include "../../../src/mako/storage/mtree_abi.cc"

extern "C" mt_status mako_test_native_runtime_poison() noexcept {
  mt_runtime &runtime = singleton_runtime();
  if (!runtime.acquired.load(std::memory_order_acquire)) {
    return MT_ERR_INVALID;
  }
  poison(&runtime);
  return MT_OK;
}
