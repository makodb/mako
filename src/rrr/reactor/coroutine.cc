#include <utility>

#include <functional>
#include <iostream>
#include <memory>
#include <boost/coroutine2/protected_fixedsize_stack.hpp>
#include "../base/all.hpp"
#include "coroutine.h"
#include "reactor.h"

// #define USE_PROTECTED_STACK

namespace rrr {
uint64_t Coroutine::global_id = 0;

Coroutine::Coroutine(rusty::Function<void()> func)
    : status_(INIT),
      needs_finalize_(false),
      func_(std::move(func)),
      boost_coro_task_(rusty::None),
      boost_coro_yield_(boost::none),
      id(Coroutine::global_id++) {
}

Coroutine::~Coroutine() {
  // rusty::Box automatically handles cleanup
//  verify(0);
}

void Coroutine::boost_run_wrapper(boost_coro_yield_t& yield) {
  boost_coro_yield_ = yield;
  verify(*func_.borrow());
  auto reactor = Reactor::get_reactor();
//  reactor->coros_;
  while (true) {
    auto sz = reactor->coros_.len();
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

// @safe - Initializes and starts a coroutine
// SAFETY: Single-threaded coroutine execution, no concurrent mutation.
// Uses @unsafe blocks for: RefCell operations, get_reactor, STL, const_cast, std::bind, boost.
void Coroutine::run() const {
  // @unsafe
  {
    verify((*boost_coro_task_.borrow()).is_none());
    verify(status_.get() == INIT);
    status_.set(STARTED);
    auto reactor = Reactor::get_reactor();
    auto sz = reactor->coros_.len();
    verify(sz > 0);
    auto task = std::bind(&Coroutine::boost_run_wrapper, const_cast<Coroutine*>(this), std::placeholders::_1);
    *boost_coro_task_.borrow_mut() = rusty::Some(rusty::make_box<boost_coro_task_t>(std::move(task)));
#ifdef USE_BOOST_COROUTINE1
    (*(*boost_coro_task_.borrow()).as_ref().unwrap())();
#endif
  }
}

// @safe - Yields control back to the reactor
// SAFETY: Single-threaded coroutine execution
void Coroutine::yield_() const {
  // @unsafe
  {
    verify(boost_coro_yield_);
    auto s = status_.get();
    verify(s == STARTED || s == RESUMED || s == FINALIZING);
    status_.set(PAUSED);
    {
      auto reactor = Reactor::get_reactor();
      reactor->n_active_coroutines_.set(reactor->n_active_coroutines_.get() - 1);
    }
    boost_coro_yield_.value()();
  }
}

// @safe - Resumes a paused coroutine
// SAFETY: Single-threaded coroutine execution
void Coroutine::continue_() const {
  // @unsafe
  {
    auto s = status_.get();
    verify(s == PAUSED || s == RECYCLED);
    verify((*boost_coro_task_.borrow()).is_some());
    status_.set(RESUMED);
    (*(*boost_coro_task_.borrow_mut()).as_mut().unwrap())();
  }
  // some events might have been triggered from last coroutine,
  // but you have to manually call the scheduler to loop.
}

bool Coroutine::finished() const {
  auto s = status_.get();
  return s == FINISHED || s == RECYCLED;
}

void Coroutine::do_finalize() {
  // Handle finalization logic if needed
  needs_finalize_.set(false);
}

} // namespace rrr
