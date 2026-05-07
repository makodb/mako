/**
 * @file fiber_context_runtime.cc
 * @brief Custom stackful coroutine runtime compatible with Fiber's legacy task/yield flow.
 */

#include "fiber_impl.h"

#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

#include "../base/all.hpp"

namespace rrr {

// fiber_swap_context is implemented in arch-specific files:
//   fiber_context_x86_64.cc  (x86_64)
//   fiber_context_aarch64.cc (AArch64/ARM64)

thread_local boost_coro_task_t* boost_coro_task_t::tls_active_task_ = nullptr;

void boost_coro_yield_t::operator()() {
  verify(task_ != nullptr);
  task_->yield_to_caller();
}

boost_coro_task_t::boost_coro_task_t(TaskFn fn)
    : fn_(std::move(fn)),
      yield_(*this) {
  init_context();
  // Match Boost.Coroutine2 pull_type behavior: run immediately on construction.
  resume();
}

boost_coro_task_t::~boost_coro_task_t() {
  if (stack_mapping_ != nullptr) {
    int rc = munmap(stack_mapping_, stack_mapping_bytes_);
    verify(rc == 0);
    stack_mapping_ = nullptr;
    stack_mapping_bytes_ = 0;
  }
}

void boost_coro_task_t::operator()() {
  resume();
}

void boost_coro_task_t::init_context() {
  std::size_t page_sz = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  if (page_sz == 0) {
    page_sz = 4096;
  }

  stack_mapping_bytes_ = kDefaultStackBytes + page_sz;
  void* mapping = mmap(nullptr,
                       stack_mapping_bytes_,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);
  verify(mapping != MAP_FAILED);
  int protect_rc = mprotect(mapping, page_sz, PROT_NONE);
  verify(protect_rc == 0);
  stack_mapping_ = mapping;

  std::uintptr_t stack_top =
      reinterpret_cast<std::uintptr_t>(static_cast<char*>(stack_mapping_) + stack_mapping_bytes_);
  stack_top &= ~static_cast<std::uintptr_t>(0xF);

  fiber_ctx_ = FiberContext{};
  caller_ctx_ = FiberContext{};
  const auto trampoline_addr =
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(&boost_coro_task_t::entry_trampoline));
#if defined(__x86_64__)
  // SysV x86_64 ABI: %rsp % 16 == 8 on function entry (simulates a call pushing ret addr).
  stack_top -= sizeof(void*);
  *reinterpret_cast<void**>(stack_top) = nullptr;
  fiber_ctx_.rsp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.rip = trampoline_addr;
#elif defined(__aarch64__)
  // AAPCS64: sp must be 16-byte aligned on function entry. No fake return address needed;
  // the trampoline address is stored in pc (= lr) and entered via ret.
  fiber_ctx_.sp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.pc = trampoline_addr;
#endif
}

void boost_coro_task_t::resume() {
  if (state_ == State::FINISHED) {
    return;
  }
  auto* old = tls_active_task_;
  tls_active_task_ = this;
  fiber_swap_context(&caller_ctx_, &fiber_ctx_);
  tls_active_task_ = old;
}

void boost_coro_task_t::yield_to_caller() {
  verify(state_ == State::RUNNING);
  state_ = State::SUSPENDED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  if (state_ != State::FINISHED) {
    state_ = State::RUNNING;
  }
}

void boost_coro_task_t::entry_trampoline() {
  auto* task = tls_active_task_;
  verify(task != nullptr);
  task->entry();
}

[[noreturn]] void boost_coro_task_t::entry() {
  state_ = State::RUNNING;
  verify(fn_);
  fn_(yield_);
  state_ = State::FINISHED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  std::abort();
}

} // namespace rrr
