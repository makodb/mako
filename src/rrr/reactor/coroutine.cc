#include <utility>

#include <functional>
#include <iostream>
#include "../base/all.hpp"
#include "coroutine.h"
#include "reactor.h"

namespace rrr {

Coroutine::Coroutine(rusty::Function<void()> func)
    : status_(INIT),
      func_(std::move(func)),
      boost_coro_task_(rusty::None),
      boost_coro_yield_(boost::none) {
}

Coroutine::~Coroutine() {
  // rusty::Box automatically handles cleanup
//  verify(0);
}

void Coroutine::BoostRunWrapper(boost_coro_yield_t& yield) {
  boost_coro_yield_ = yield;
  verify(func_);
  auto reactor = Reactor::GetReactor();
//  reactor->coros_;
  while (true) {
    auto sz = reactor->coros_.size();
    verify(sz > 0);
    func_();
    func_ = {};
    status_ = FINISHED;
    yield();
  }
}

void Coroutine::Run() const {
  verify(boost_coro_task_.is_none());
  verify(status_ == INIT);
  status_ = STARTED;
  auto reactor = Reactor::GetReactor();
//  reactor->coros_;
  auto sz = reactor->coros_.size();
  verify(sz > 0);
  auto task = std::bind(&Coroutine::BoostRunWrapper, const_cast<Coroutine*>(this), std::placeholders::_1);
  boost_coro_task_ = rusty::Some(rusty::make_box<boost_coro_task_t>(std::move(task)));
#ifdef USE_BOOST_COROUTINE1
  (*boost_coro_task_.as_ref().unwrap())();
#endif
}

void Coroutine::Yield() const {
  verify(boost_coro_yield_);
  verify(status_ == STARTED || status_ == RESUMED);
  status_ = PAUSED;
  boost_coro_yield_.value()();
}

void Coroutine::Continue() const {
  verify(status_ == PAUSED || status_ == RECYCLED);
  verify(boost_coro_task_.is_some());
  status_ = RESUMED;
  (*boost_coro_task_.as_mut().unwrap())();
  // some events might have been triggered from last coroutine,
  // but you have to manually call the scheduler to loop.
}

bool Coroutine::Finished() const {
  return status_ == FINISHED || status_ == RECYCLED;
}

} // namespace rrr
