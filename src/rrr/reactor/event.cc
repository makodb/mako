
#include <functional>
#include <thread>
#include "coroutine.h"
#include "event.h"
#include "reactor.h"
#include "epoll_wrapper.h"

namespace rrr {
using std::function;

// void Event::Wait(uint64_t timeout) {
// //  verify(__debug_creator); // if this fails, the event is not created by reactor.

//   verify(Reactor::sp_reactor_th_);
//   verify(Reactor::sp_reactor_th_->thread_id_ == std::this_thread::get_id());
//   if (IsReady()) {
//     status_ = DONE; // does not need to wait.
//     return;
//   } else {
//     verify(status_ == INIT);
//     status_= DEBUG;
//     // the event may be created in a different coroutine.
//     // this value is set when wait is called.
//     // for now only one coroutine can wait on an event.
//     auto sp_coro = Coroutine::CurrentCoroutine();
// //    verify(sp_coro);
// //    verify(_dbg_p_scheduler_ == nullptr);
// //    _dbg_p_scheduler_ = Reactor::GetReactor().get();
//     auto& events = Reactor::GetReactor()->waiting_events_;
//     events.push_back(shared_from_this());
//     wp_coro_ = sp_coro;
//     status_ = WAIT;
//     sp_coro->Yield();
//   }
// }

void Event::Wait(uint64_t timeout) {
//  verify(__debug_creator); // if this fails, the event is not created by reactor.
  verify(Reactor::sp_reactor_th_.is_some());
  verify(Reactor::sp_reactor_th_.as_ref().unwrap()->thread_id_ == std::this_thread::get_id());
  if (status_ == DONE) return; // TODO: yidawu add for the second use the event.
  // verify(status_ == INIT);
  if (IsReady()) {
    status_ = DONE; // no need to wait.
    return;
  } else {
//    if (status_ == WAIT) {
//      // this does not look right, fix later
//      Log_fatal("multiple waits on the same event; no support at the moment");
//    }
//    verify(status_ == INIT); // does not support multiple wait so far. maybe we can support it in the future.
//    status_= DEBUG;
    // the event may be created in a different coroutine.
    // this value is set when wait is called.
    // for now only one coroutine can wait on an event.
    auto sp_coro_opt = Coroutine::CurrentCoroutine();
    verify(sp_coro_opt.is_some());  // Can't wait outside a coroutine
    auto sp_coro = sp_coro_opt.unwrap();

    // Rc gives const access, use const_cast for mutation (safe: thread-local)
    auto reactor_rc = Reactor::GetReactor();
    auto& reactor = const_cast<Reactor&>(*reactor_rc);
    auto& waiting_events = reactor.waiting_events_;
    waiting_events.push_back(shared_from_this());

    if (timeout > 0) {
      auto now = Time::now(true);
      wakeup_time_ = now + timeout;
      //Log_info("WAITING: %p", shared_from_this());
      // Log_info("wake up %lld, now %lld", wakeup_time_, now);
      auto& timeout_events = reactor.timeout_events_;
      timeout_events.push_back(shared_from_this());
    }
    // TODO optimize timeout_events, sort by wakeup time.
//      auto it = timeout_events.end();
//      timeout_events.push_back(rc_this_event);
//      while (it != events.begin()) {
//        it--;
//        auto& it_event = *it;
//        if (it_event->wakeup_time_ < wakeup_time_) {
//          it++; // list insert happens before position.
//          break;
//        }
//      }
//      events.insert(it, rc_this_event);
    wp_coro_ = sp_coro;
    status_ = WAIT;
    sp_coro->Yield();
  }
}

bool Event::Test() {
  verify(__debug_creator); // if this fails, the event is not created by reactor.
  if (IsReady()) {
    if (status_ == INIT) {
      // wait has not been called, do nothing until wait happens.
      status_ = DONE;
    } else if (status_ == WAIT) {
      auto option_coro = wp_coro_.upgrade();
      verify(option_coro.is_some());
      verify(status_ != DEBUG);
      status_ = READY;
    } else if (status_ == READY) {
      // This could happen for a quorum event.
      Log_debug("event status ready, triggered?");
    } else if (status_ == DONE) {
      // do nothing
    } else {
      verify(0);
    }
    return true;
  }
  return false;
}

Event::Event() {
  auto coro_opt = Coroutine::CurrentCoroutine();
  // It's OK if no coroutine is running - event might be created outside a coroutine
  // and Wait() called later from within one
  if (coro_opt.is_some()) {
    wp_coro_ = coro_opt.unwrap();
  }
  // Otherwise wp_coro_ stays as default empty weak pointer
}

bool IntEvent::TestTrigger() {
  verify(status_ <= WAIT);
  if (value_ == target_) {
    if (status_ == INIT) {
      // do nothing until wait happens.
      status_ = DONE;
    } else if (status_ == WAIT) {
      status_ = READY;
    } else {
      verify(0);
    }
    return true;
  }
  return false;
}

void SharedIntEvent::Wait(function<bool(int v)> f) {
  auto sp_ev =  Reactor::CreateSpEvent<IntEvent>();
  sp_ev->test_ = f;
  events_.push_back(sp_ev);
  sp_ev->Wait();
}

} // namespace rrr
