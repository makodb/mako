
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include "../base/all.hpp"
#include <rusty/rusty.hpp>
#include "coroutine.h"

// External safety annotations for std::shared_ptr and Event operations
// @external: {
//   std::enable_shared_from_this::shared_from_this: [unsafe, () -> std::shared_ptr<auto>]
//   std::make_shared: [unsafe, (auto...) -> std::shared_ptr<auto>]
//   std::shared_ptr::operator*: [unsafe, () -> auto&]
//   std::shared_ptr::operator->: [unsafe, () -> auto*]
//   std::shared_ptr::get: [unsafe, () -> auto*]
//   std::shared_ptr::operator=: [unsafe, (const std::shared_ptr<auto>&) -> std::shared_ptr<auto>&]
//   std::vector::push_back: [unsafe, (auto) -> void]
// }

namespace rrr {
using std::function;
using std::vector;

class Reactor;
class Event : public std::enable_shared_from_this<Event> {
//class Event {
 public:
  int __debug_creator{0};
  enum EventStatus { INIT = 0, WAIT = 1, READY = 2, DONE = 3, TIMEOUT = 4, DEBUG};
  EventStatus status_{INIT};
  uint64_t type_{0};
  function<bool(int)> test_{};
  uint64_t wakeup_time_; // calculated by timeout, unit: microsecond

  // An event is usually allocated on a coroutine stack, thus it cannot own a
  //   shared_ptr to the coroutine it is.
  // In this case there is no shared pointer to the event.
  // When the stack that contains the event frees, the event frees.
  // Weak reference to coroutine using rusty::rc::Weak with proper reference counting
  rusty::rc::Weak<Coroutine> wp_coro_{}; 

  virtual void Wait(uint64_t timeout=0) final;

  void Wait(function<bool(int)> f) {
    test_ = f;
    Wait();
  }

  virtual bool Test();
  virtual bool IsReady() const {return test_(0);}

  friend Reactor;
// protected:
  Event();
};

class IntEvent : public Event {

 public:
  IntEvent() {}
  int value_{0};
  int target_{1};


  bool TestTrigger();

  int get() {
    return value_;
  }

  int Set(int n) {
    int t = value_;
    value_ = n;
    TestTrigger();
    return t;
  };

  bool IsReady() const override {
    if (test_) {
      return test_(value_);
    } else {
      return (value_ == target_);
    }
  }
};

class SharedIntEvent {
 public:
  int value_{};
  vector<std::shared_ptr<IntEvent>> events_;
  int Set(int& v) {
    auto ret = value_;
    value_ = v;
    for (auto& sp_ev : events_) {
      if (sp_ev->status_ <= Event::WAIT)
        sp_ev->Set(v);
    }
    return ret;
  }

  void Wait(function<bool(int)> f);
};

class NeverEvent: public Event {
 public:
  bool IsReady() const override {
    return false;
  }
};

class TimeoutEvent : public Event {
 public:
  uint64_t wakeup_time_{0};
  TimeoutEvent(uint64_t wait_us_): wakeup_time_{Time::now()+wait_us_} {}

  bool IsReady() const override {
//    Log_debug("test timeout");
    return (Time::now() > wakeup_time_);
  }
};

class OrEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;

  void AddEvent() {
    // empty func for recursive variadic parameters
  }

  template<typename X, typename... Args>
  void AddEvent(X& x, Args&... rest) {
    events_.push_back(x);
    AddEvent(rest...);
  }

  template<typename... Args>
  OrEvent(Args&&... args) {
    AddEvent(args...);
  }

  bool IsReady() const override {
    return std::any_of(events_.begin(), events_.end(), [](const std::shared_ptr<Event>& e){return e->IsReady();});
  }
};

class AndEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;

  // Default constructor
  AndEvent() {}

  // Constructor for vector of events
  explicit AndEvent(const vector<std::shared_ptr<Event>>& evs) : events_(evs) {}

  void AddEvent() {
    // empty func for recursive variadic parameters
  }

  template<typename... Args>
  void AddEvent(std::shared_ptr<Event> x, Args... rest) {
    events_.push_back(x);
    AddEvent(rest...);
  }

  template<typename... Args>
  AndEvent(std::shared_ptr<Event> first, Args... rest) {
    AddEvent(first, rest...);
  }

  bool IsReady() const override {
    // All events must be ready (or DONE) for AndEvent to be ready
    return std::all_of(events_.begin(), events_.end(),
                       [](const std::shared_ptr<Event>& e) {
                         return e->IsReady() || e->status_ == Event::DONE;
                       });
  }
};

} // namespace rrr
