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

void Coroutine::BoostRunWrapper(boost_coro_yield_t& yield) {
  boost_coro_yield_ = yield;
  verify(*func_.borrow());
  auto reactor = Reactor::GetReactor();
//  reactor->coros_;
  while (true) {
    auto sz = reactor->coros_.size();
    verify(sz > 0);
    verify(*func_.borrow());
    (*func_.borrow_mut())();  // borrow_mut needed because operator() is non-const
    *func_.borrow_mut() = {};
    status_.set(FINISHED);
    if (needs_finalize_.get()) {
      Log_info("Warning: We did not deal with backlog issues");
      needs_finalize_.set(false);
    }
    Reactor::GetReactor()->n_active_coroutines_--;
    yield();
  }
}

// @safe - Initializes and starts a coroutine
// SAFETY: Single-threaded coroutine execution, no concurrent mutation.
// Uses @unsafe blocks for: RefCell operations, GetReactor, STL, const_cast, std::bind, boost.
void Coroutine::Run() const {
  // @unsafe
  {
    verify((*boost_coro_task_.borrow()).is_none());
    verify(status_.get() == INIT);
    status_.set(STARTED);
    auto reactor = Reactor::GetReactor();
    auto sz = reactor->coros_.size();
    verify(sz > 0);
    auto task = std::bind(&Coroutine::BoostRunWrapper, const_cast<Coroutine*>(this), std::placeholders::_1);
    *boost_coro_task_.borrow_mut() = rusty::Some(rusty::make_box<boost_coro_task_t>(std::move(task)));
#ifdef USE_BOOST_COROUTINE1
    (*(*boost_coro_task_.borrow()).as_ref().unwrap())();
#endif
  }
}

// @safe - Yields control back to the reactor
// SAFETY: Single-threaded coroutine execution
void Coroutine::Yield() const {
  // @unsafe
  {
    verify(boost_coro_yield_);
    auto s = status_.get();
    verify(s == STARTED || s == RESUMED || s == FINALIZING);
    status_.set(PAUSED);
    Reactor::GetReactor()->n_active_coroutines_--;
    boost_coro_yield_.value()();
  }
}

// @safe - Resumes a paused coroutine
// SAFETY: Single-threaded coroutine execution
void Coroutine::Continue() const {
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

bool Coroutine::Finished() const {
  auto s = status_.get();
  return s == FINISHED || s == RECYCLED;
}

void Coroutine::DoFinalize() {
  // Handle finalization logic if needed
  needs_finalize_.set(false);
}

} // namespace rrr
