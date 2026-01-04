
#pragma once

#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <set>
#include <unordered_set>
#include <map>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
//#include "../../deptran/client_worker.h"
#include <functional>
#include <memory>
#include <vector>
#include "../base/all.hpp"
#include <rusty/rusty.hpp>
#include "coroutine.h"

// External safety annotations for std::shared_ptr and Event operations
// @external: {
//   std::make_shared: [unsafe, (auto...) -> std::shared_ptr<auto>]
//   std::shared_ptr::operator*: [unsafe, () -> auto&]
//   std::shared_ptr::operator->: [unsafe, () -> auto*]
//   std::shared_ptr::get: [unsafe, () -> auto*]
//   std::shared_ptr::operator=: [unsafe, (const std::shared_ptr<auto>&) -> std::shared_ptr<auto>&]
//   std::vector::push_back: [unsafe, (auto) -> void]
//   std::atomic<int>::operator int: [safe, () -> int]
//   std::atomic<*>::operator T: [safe, () -> T]
//   std::atomic<*>::load: [safe, () -> T]
//   std::atomic<*>::store: [safe, (T) -> void]
// }

// Note: SUCCESS, REPEAT, REJECT macros removed - they conflict with mako's ErrorCode enum
// Use ErrorCode::SUCCESS, etc. instead if needed

#define wait_recordplace(ev, wait_func) do { \
  auto ref_ev = ev; \
  ref_ev->record_place(__FILE__, __LINE__); \
  ref_ev->wait_func; \
} while(0)

namespace rrr {
using std::function;
using std::vector;
using std::list;

class Reactor;
class Event {
 protected:
  // Self-reference for adding to queues (using weak_ptr for shared ownership)
  // Set by CreateSpEvent after construction
  std::weak_ptr<Event> self_;
//class Event {
 public:
  int __debug_creator{0};
  enum EventStatus { INIT = 0, WAIT = 1, READY = 2,
      DONE = 3, TIMEOUT = 4, DEBUG};

#ifdef EVENT_TIMEOUT_CHECK
  bool __debug_timeout_{false};
#endif
  rusty::Cell<EventStatus> status_{INIT};
  void* _dbg_p_scheduler_{nullptr};  // Jetpack: for debugging
  uint64_t type_{0};
  function<bool(int)> test_{};
	bool needs_finalize_{false};
  uint64_t wakeup_time_; // calculated by timeout, unit: microsecond
  bool rcd_wait_ = false;
  std::string wait_place_{"not recorded"};
  bool in_waiting_list_{false};

  // An event is usually allocated on a coroutine stack, thus it cannot own a
  //   shared_ptr to the coroutine it is.
  // In this case there is no shared pointer to the event.
  // When the stack that contains the event frees, the event frees.
  // Weak reference to coroutine using rusty::rc::Weak with proper reference counting
  rusty::rc::Weak<Coroutine> wp_coro_{};

  // @unsafe
  virtual void wait(uint64_t timeout=0) final;

  void wait(function<bool(int)> f) {
    test_ = f;
    wait();
  }

  virtual void log(){return;}
  virtual uint64_t get_coro_id();
  void record_place(const char* file, int line);

  // @safe - Tests if event is ready
  virtual bool test();
  virtual bool is_slow();
  virtual bool is_ready() {
    if (!test_) return false;
    return test_(0);
  }

  // Composite events (AndEvent, OrEvent, QuorumEvent) need periodic polling
  // Added at END to preserve vtable layout for binary compatibility
  virtual bool is_composite_event() { return false; }

  // Self-reference management (uses shared_ptr for polymorphism support)
  void set_self(std::weak_ptr<Event> self) { self_ = self; }
  std::shared_ptr<Event> get_self() const { return self_.lock(); }

  friend Reactor;
// protected:
  Event();
};

template <class Type>
class BoxEvent : public Event {
 public:
  Type content_{};
  bool is_set_{false};
  Type& get() {
    return content_;
  }
  void set(const Type& c) {
    is_set_ = true;
    content_ = c;
    test();
  }
  void clear() {
    is_set_ = false;
    content_ = {};
  }
  virtual bool is_ready() override {
    return is_set_;
  }
};

class IntEvent : public Event {

 public:
  IntEvent() {}
  IntEvent(int tar) :target_(tar) {}
  int value_{0};
  int target_{1};


  bool test_trigger();

  int get() {
    return value_;
  }

  // @unsafe
  int set(int n) {
    int t = value_;
    value_ = n;
    // test_trigger();
    test();
    return t;
  };

  bool is_ready() override {
    if (test_) {
      return test_(value_);
    } else {
      return (value_ >= target_);
    }
  }
};

class SharedIntEvent {
 public:
  int value_{};
  vector<std::shared_ptr<IntEvent>> events_;
  // Declaration only - definition in event.cc
  int set(const int& v);

  void wait(function<bool(int)> f);
  bool wait_until_gte(int x, int timeout=0);
};


class NeverEvent: public Event {
 public:
  bool is_ready() override {
    return false;
  }
};

class TimeoutEvent : public Event {
 public:
  uint64_t wakeup_time_{0};
  uint64_t wait_us_{0};
  TimeoutEvent(uint64_t wait_us)
      : wakeup_time_{Time::now(true) + wait_us}, wait_us_(wait_us) {}

  bool is_ready() override {
//    Log_debug("test timeout");
    return (Time::now(true) > wakeup_time_);
  }

  // @unsafe
  void wait() {
    Event::wait(wait_us_);
  }
};

class OrEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;

  void add_event() {
    // empty func for recursive variadic parameters
  }

  template<typename X, typename... Args>
  void add_event(X& x, Args&... rest) {
    events_.push_back(x);
    add_event(rest...);
  }

  template<typename... Args>
  OrEvent(Args&&... args) {
    add_event(args...);
  }

  bool is_ready() override {
    return std::any_of(events_.begin(), events_.end(), [](const std::shared_ptr<Event>& e){return e->is_ready();});
  }

  // Mark as composite event - will be polled in reactor loop
  bool is_composite_event() override { return true; }
};

class AndEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;

  // Default constructor (mako-dev)
  AndEvent() {}

  // Constructor for vector of events
  explicit AndEvent(const vector<std::shared_ptr<Event>>& evs) : events_(evs) {}

  void add_event() {
    // empty func for recursive variadic parameters
  }

  template<typename... Args>
  void add_event(std::shared_ptr<Event> x, Args... rest) {
    events_.push_back(std::move(x));
    add_event(rest...);
  }

  template<typename... Args>
  AndEvent(std::shared_ptr<Event> first, Args... rest) {
    add_event(std::move(first), rest...);
  }

  void log() {
    for(size_t i = 0; i < events_.size(); i++){
      events_[i]->log();
    }
  }

  bool is_ready() override {
    // All events must be ready (or DONE) for AndEvent to be ready
    // Include null check for safety
    return std::all_of(events_.begin(), events_.end(),
                       [](const std::shared_ptr<Event>& e) {
                         return e && (e->is_ready() || e->status_.get() == Event::DONE);
                       });
  }

  // Mark as composite event - will be polled in reactor loop
  bool is_composite_event() override { return true; }
};

class NEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;
  int number;

  void add_event() {
    // empty func for recursive variadic parameters
  }

  template<typename... Args>
  void add_event(std::shared_ptr<Event> x, Args... rest) {
    events_.push_back(std::move(x));
    add_event(rest...);
  }

  template<typename... Args>
  NEvent(std::shared_ptr<Event> first, Args... rest) {
    add_event(std::move(first), rest...);
  }

  bool is_ready() override {
    int count = 0;
    for(auto index = events_.begin(); index != events_.end(); index++){
      if((*index)->is_ready()){
        count++;
        if(count == number){
          return true;
        }
      }
    }
    return false;
  }
};

class DispatchEvent: public Event{
  public:
    uint32_t n_dispatch_;
    uint32_t n_dispatch_ack_ = 0;
    std::map<uint32_t, bool> dispatch_acks_ = {};
    bool aborted_ = false;
    bool more = false;

    DispatchEvent() : Event(){

    }

    bool is_ready() override{
      if(n_dispatch_ == n_dispatch_ack_){
        if(aborted_){
          return true;
        }
        else{
          return std::all_of(dispatch_acks_.begin(),
                             dispatch_acks_.end(),
                             [](std::pair<uint32_t, bool> pair) {
                             return pair.second;
                             });
        }
      }
      else if(more){
        return true;
      }
      return false;
    }

};

class SingleRPCEvent: public Event{
  public:
    uint32_t cli_id_;
    uint32_t coo_id_;
    int32_t& res_;
    std::string log_file = "logs.txt";
    std::unordered_set<int> dep{};
    SingleRPCEvent(uint32_t cli_id, int32_t res): Event(),
                                                   cli_id_(cli_id),
                                                   res_(res){
    }
    void add_dep(int tgtId){
      auto index = dep.find(tgtId);
      if(index == dep.end()) dep.insert(tgtId);
    }
    void log() override {
      std::ofstream of(log_file, std::fstream::app);
      //of << "hello\n";
      of << "{ " << cli_id_ << ": ";
      for(auto it = dep.begin(); it != dep.end(); it++){
        of << *it << " ";
      }
      of << "}\n";
      of.close();
    }
    bool is_ready() override{
      // SUCCESS=0, REJECT=-10 (macros removed to avoid conflict with mako ErrorCode)
      return res_ == 0 || res_ == -10;
    }
};

} // namespace rrr
