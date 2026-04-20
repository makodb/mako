module;

#include <rusty/box.hpp>
#include <rusty/rc.hpp>
#include <rusty/option.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>
#include <rusty/function.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>


#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>


module rrr:impl.reactor.fiber_context_runtime;
import rrr;

/**
 * @file fiber_context_runtime.cc
 * @brief Custom stackful fiber runtime compatible with Fiber's task/yield flow.
 */




namespace rrr {

extern "C" void fiber_swap_context(FiberContext* from, FiberContext* to);

#if !defined(__x86_64__)
extern "C" void fiber_swap_context(FiberContext* from, FiberContext* to) {
  (void)from;
  (void)to;
  verify(0 && "fiber_swap_context is only available on x86_64");
  std::abort();
}
#endif

thread_local fiber_task_t* fiber_task_t::tls_active_task_ = nullptr;

void fiber_yield_t::operator()() {
  verify(task_ != nullptr);
  task_->yield_to_caller();
}

fiber_task_t::fiber_task_t(TaskFn fn)
    : fn_(std::move(fn)),
      yield_(*this) {
  init_context();
  // Match Boost.Coroutine2 pull_type behavior: run immediately on construction.
  resume();
}

fiber_task_t::~fiber_task_t() {
  if (stack_mapping_ != nullptr) {
    int rc = munmap(stack_mapping_, stack_mapping_bytes_);
    verify(rc == 0);
    stack_mapping_ = nullptr;
    stack_mapping_bytes_ = 0;
  }
}

void fiber_task_t::operator()() {
  resume();
}

void fiber_task_t::init_context() {
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
  // SysV x86_64 ABI expects %rsp % 16 == 8 on function entry.
  stack_top -= sizeof(void*);
  *reinterpret_cast<void**>(stack_top) = nullptr;

  fiber_ctx_ = FiberContext{};
  caller_ctx_ = FiberContext{};
  fiber_ctx_.rsp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.rip =
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(&fiber_task_t::entry_trampoline));
}

void fiber_task_t::resume() {
  if (state_ == State::FINISHED) {
    return;
  }
  auto* old = tls_active_task_;
  tls_active_task_ = this;
  fiber_swap_context(&caller_ctx_, &fiber_ctx_);
  tls_active_task_ = old;
}

void fiber_task_t::yield_to_caller() {
  verify(state_ == State::RUNNING);
  state_ = State::SUSPENDED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  if (state_ != State::FINISHED) {
    state_ = State::RUNNING;
  }
}

void fiber_task_t::entry_trampoline() {
  auto* task = tls_active_task_;
  verify(task != nullptr);
  task->entry();
}

[[noreturn]] void fiber_task_t::entry() {
  state_ = State::RUNNING;
  verify(static_cast<bool>(fn_));
  fn_(yield_);
  state_ = State::FINISHED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  std::abort();
}

} // namespace rrr
