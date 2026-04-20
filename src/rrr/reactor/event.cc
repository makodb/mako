module;

#include <algorithm>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <memory>
#include <rusty/rusty.hpp>


#include <functional>
#include <thread>
#include <iostream>
#include <cerrno>
#include <cstring>




module rrr:impl.reactor.event;
import rrr;

namespace rrr {
using std::function;

uint64_t Event::get_fiber_id(){
  auto fiber_opt = Fiber::current_fiber();
  verify(fiber_opt.is_some());
  return fiber_opt.unwrap()->id;
}

bool Event::is_slow() {
	bool result = Reactor::get_reactor()->slow_.get();
	Reactor::get_reactor()->slow_.set(false);
	return result;
}

// void Event::Wait(uint64_t timeoutuint64_t timeout) {
// //  verify(__debug_creator); // if this fails, the event is not created by reactor.

//   verify(Reactor::sp_reactor_th_);
//   verify(Reactor::sp_reactor_th_->thread_id_ == std::this_thread::get_id());
//   if (IsReady()) {
//     status_ = DONE; // does not need to wait.
//     return;
//   } else {
//     verify(status_ == INIT);
//     status_= DEBUG;
//     // the event may be created in a different fiber.
//     // this value is set when wait is called.
//     // for now only one fiber can wait on an event.
//     auto sp_fiber = Fiber::current_fiber();
// //    verify(sp_fiber);
// //    verify(_dbg_p_scheduler_ == nullptr);
// //    _dbg_p_scheduler_ = Reactor::get_reactor().get();
//     auto& events = Reactor::get_reactor()->waiting_events_;
//     events.push_back(shared_from_this());
//     wp_fiber_ = sp_fiber;
//     status_ = WAIT;
//     sp_fiber->yield_();
//   }
// }

void Event::wait(uint64_t timeout) {
//  verify(__debug_creator); // if this fails, the event is not created by reactor.
  verify(Reactor::sp_reactor_th_.is_some());
  verify(Reactor::sp_reactor_th_.as_ref().unwrap()->thread_id_.get() == std::this_thread::get_id());
  if (status_.get() == DONE) return; // TODO: yidawu add for the second use the event.
  // verify(status_.get() == INIT);
  if (is_ready()) {
    status_.set(DONE); // no need to wait.
    return;
  } else {
//    if (status_ == WAIT) {
//      // this does not look right, fix later
//      Log_fatal("multiple waits on the same event; no support at the moment");
//    }
//    verify(status_ == INIT); // does not support multiple wait so far. maybe we can support it in the future.
//    status_= DEBUG;
    // the event may be created in a different fiber.
    // this value is set when wait is called.
    // for now only one fiber can wait on an event.
    auto fiber_opt = Fiber::current_fiber();
    verify(fiber_opt.is_some());  // Can't wait outside a fiber
    auto fiber = fiber_opt.unwrap();

    // Use RefCell borrow_mut() for safe interior mutability
    auto reactor_rc = Reactor::get_reactor();
    reactor_rc->waiting_events_.borrow_mut()->push_back(get_self());

    // Composite events (WaitAll, WaitAny, QuorumEvent) need periodic polling
    // Add them to a separate queue that gets scanned (much smaller than all events)
    // Regular RPC events (Raft) self-notify via test() - zero overhead!
    if (is_composite_event()) {
      Reactor::get_reactor()->composite_events_.borrow_mut()->push_back(get_self());
    }

#ifdef EVENT_TIMEOUT_CHECK
    if (timeout == 0) {
      __debug_timeout_ = true;
      timeout = 200 * 1000 * 1000;
//#ifdef SIMULATE_WAN
//      timeout = 600 * 1000 * 1000;
//#endif
    }
#endif
    if (timeout > 0) {
      auto now = Time::now(true);
      wakeup_time_ = now + timeout;
      //Log_info("WAITING: %p", get_self().get());
      // Log_info("wake up %lld, now %lld", wakeup_time_, now);
      reactor_rc->timeout_events_.borrow_mut()->push_back(get_self());
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
//      events.insert(it, shared_from_this());

    wp_fiber_ = fiber;
    status_.set(WAIT);
    auto fiber_status = fiber->status_.get();
    verify(fiber_status != Fiber::FINISHED && fiber_status != Fiber::RECYCLED);
    fiber->yield_();
#ifdef EVENT_TIMEOUT_CHECK
    if (__debug_timeout_ && status_.get() == TIMEOUT) {
      Log_info("timeout");
      verify(0);
    }
#endif
  }
}

void Event::record_place(const char* file, int line) {
  char buff[200];
  sprintf(buff, "%s:%d", file, line);
  wait_place_ += std::string(buff);
  rcd_wait_ = true;
}

// @unsafe - Tests if event is ready (calls verify/log helpers not marked @safe)
bool Event::test() {
  verify(__debug_creator);
  if (is_ready()) {
    if (status_.get() == INIT) {
      status_.set(DONE);
    } else if (status_.get() == WAIT) {
      auto option_fiber = wp_fiber_.upgrade();
      verify(option_fiber.is_some());
      verify(status_.get() != DEBUG);
      status_.set(READY);
    } else if (status_.get() == READY) {
      Log_debug("event status ready, triggered?");
    } else if (status_.get() == DONE) {
      // do nothing
    } else if (status_.get() == TIMEOUT) {
      // do nothing
    } else {
      verify(0);
    }
    return true;
  } else {
    if (status_.get() == DONE) {
      status_.set(INIT);
    }
  }
  return false;
}

Event::Event() {
  auto fiber_opt = Fiber::current_fiber();
  // It's OK if no fiber is running - event might be created outside a fiber
  // and Wait() called later from within one
  if (fiber_opt.is_some()) {
    wp_fiber_ = fiber_opt.unwrap();
  }
  // Otherwise wp_fiber_ stays as default empty weak pointer
}

bool IntEvent::test_trigger() {
  verify(status_.get() <= WAIT);
  if (value_ == target_) {
    if (status_.get() == INIT) {
      // do nothing until wait happens.
      status_.set(DONE);
    } else if (status_.get() == WAIT) {
      status_.set(READY);
    } else {
      verify(0);
    }
    return true;
  }
  return false;
}

int SharedIntEvent::set(const int& v) {
  auto ret = value_;
  value_ = v;
  for (auto& ev : events_) {
    if (ev->status_.get() <= Event::WAIT) {
      if (ev->target_ <= v) {
        ev->set(v);
      }
    }
  }
  return ret;
}

bool SharedIntEvent::wait_until_gte(int x, int timeout) {
  if (value_ >= x) {
    return false;
  }
  auto ev =  Reactor::create_sp_event<IntEvent>();
  ev->value_ = value_;
  ev->target_ = x;
  events_.push(ev);
  ev->wait(timeout);
  // verify(ev->status_.get() != Event::TIMEOUT);  // why can't it be timeout?
  // remove the event from event vector after it entering a terminate state (READY or TIMEOUT)
  bool if_timeout = (ev->status_.get() == Event::TIMEOUT);
  auto* ev_ptr = ev.get();
  events_.retain(rusty::Function<bool(const std::shared_ptr<IntEvent>&)>(
      [ev_ptr](const std::shared_ptr<IntEvent>& item) {
        return item.get() != ev_ptr;
      }));
  return if_timeout;
}

void SharedIntEvent::wait(function<bool(int v)> f) {
  if (f(value_)) {
    return;
  }
  auto ev =  Reactor::create_sp_event<IntEvent>();
  ev->value_ = value_;
  ev->test_ = f;
  events_.push(ev);
//  ev->wait(1000*1000*1000);
//  verify(ev->status_ != Event::TIMEOUT);
  ev->wait();
}

} // namespace rrr
