/**
 * @file fiber_impl.cc
 * @brief Implementation of the Fiber class.
 */

#include <utility>

#include <functional>
#include <iostream>
#include <memory>
#include "../base/all.hpp"
#include "fiber_impl.h"
#include "reactor.h"

// #define USE_PROTECTED_STACK

namespace rrr {
thread_local uint64_t Fiber::global_id = 0;

Fiber::Fiber(rusty::Function<void()> func)
    : status_(INIT),
      needs_finalize_(false),
      func_(std::move(func)),
      boost_coro_task_(rusty::None),
      id(Fiber::global_id++) {
}

Fiber::~Fiber() {
  // rusty::Box automatically handles cleanup
//  verify(0);
}

void Fiber::boost_run_wrapper(boost_coro_yield_t& yield) {
  boost_coro_yield_.set(&yield);
  verify(*func_.borrow());
  auto reactor = Reactor::get_reactor();
  while (true) {
    auto sz = reactor->coros_.borrow()->len();
    verify(sz > 0);
    verify(*func_.borrow());
    (*func_.borrow_mut())();  // borrow_mut needed because operator() is non-const
    *func_.borrow_mut() = {};
    status_.set(FINISHED);
    if (needs_finalize_.get()) {
      Log_info("Warning: We did not deal with backlog issues");
      needs_finalize_.set(false);
    }
    auto reactor = Reactor::get_reactor();
    reactor->n_active_coroutines_.set(reactor->n_active_coroutines_.get() - 1);
    yield();
  }
}

// @safe - Initializes and starts a fiber
// SAFETY: Single-threaded fiber execution, no concurrent mutation.
// Uses @unsafe blocks for: RefCell operations, get_reactor, STL, const_cast, std::bind, fiber runtime calls.
void Fiber::run() const {
  // @unsafe
  {
    verify((*boost_coro_task_.borrow()).is_none());
    verify(status_.get() == INIT);
    status_.set(STARTED);
    auto reactor = Reactor::get_reactor();
    auto sz = reactor->coros_.borrow()->len();
    verify(sz > 0);
    auto task = std::bind(&Fiber::boost_run_wrapper, const_cast<Fiber*>(this), std::placeholders::_1);
    *boost_coro_task_.borrow_mut() = rusty::Some(rusty::make_box<boost_coro_task_t>(std::move(task)));
#ifdef USE_BOOST_COROUTINE1
    (*(*boost_coro_task_.borrow()).as_ref().unwrap())();
#endif
  }
}

// @safe - Yields control back to the reactor
// SAFETY: Single-threaded fiber execution
void Fiber::yield_() const {
  // @unsafe
  {
    auto* yield_ptr = boost_coro_yield_.get();
    verify(yield_ptr != nullptr);
    auto s = status_.get();
    verify(s == STARTED || s == RESUMED || s == FINALIZING);
    status_.set(PAUSED);
    {
      auto reactor = Reactor::get_reactor();
      reactor->n_active_coroutines_.set(reactor->n_active_coroutines_.get() - 1);
    }
    (*yield_ptr)();
  }
}

// @safe - Resumes a paused fiber
// SAFETY: Single-threaded fiber execution
void Fiber::continue_() const {
  // @unsafe
  {
    auto s = status_.get();
    verify(s == PAUSED || s == RECYCLED);
    verify((*boost_coro_task_.borrow()).is_some());
    status_.set(RESUMED);
    (*(*boost_coro_task_.borrow_mut()).as_mut().unwrap())();
  }
  // some events might have been triggered from last fiber,
  // but you have to manually call the scheduler to loop.
}

bool Fiber::finished() const {
  auto s = status_.get();
  return s == FINISHED || s == RECYCLED;
}

void Fiber::do_finalize() {
  // Handle finalization logic if needed
  needs_finalize_.set(false);
}

} // namespace rrr
