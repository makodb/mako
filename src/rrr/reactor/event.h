
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
//   std::enable_shared_from_this::shared_from_this: [unsafe, () -> std::shared_ptr<auto>]
//   std::make_shared: [unsafe, (auto...) -> std::shared_ptr<auto>]
//   std::shared_ptr::operator*: [unsafe, () -> auto&]
//   std::shared_ptr::operator->: [unsafe, () -> auto*]
//   std::shared_ptr::get: [unsafe, () -> auto*]
//   std::shared_ptr::operator=: [unsafe, (const std::shared_ptr<auto>&) -> std::shared_ptr<auto>&]
//   std::vector::push_back: [unsafe, (auto) -> void]
// }

// Note: SUCCESS, REPEAT, REJECT macros removed - they conflict with mako's ErrorCode enum
// Use ErrorCode::SUCCESS, etc. instead if needed

#define Wait_recordplace(sp_ev, wait_func) do { \
  auto ref_ev = sp_ev; \
  ref_ev->RecordPlace(__FILE__, __LINE__); \
  ref_ev->wait_func; \
} while(0)

namespace rrr {
using std::function;
using std::vector;
using std::list;

class Reactor;
class Event : public std::enable_shared_from_this<Event> {
  std::mutex status_mtx_; // This is used for ThreadSafeTest
 protected:
  // Raw pointer to reactor for cross-thread access (safe: reactor lifetime = thread lifetime)
  Reactor* current_reactor_{nullptr};
//class Event {
 public:
  int __debug_creator{0};
  enum EventStatus { INIT = 0, WAIT = 1, READY = 2,
      DONE = 3, TIMEOUT = 4, DEBUG};

#ifdef EVENT_TIMEOUT_CHECK
  bool __debug_timeout_{false};
#endif
  EventStatus status_{INIT};
  void* _dbg_p_scheduler_{nullptr};  // Jetpack: for debugging
  uint64_t type_{0};
  function<bool(int)> test_{};
	bool needs_finalize_{false};
  uint64_t wakeup_time_; // calculated by timeout, unit: microsecond
  bool rcd_wait_ = false;
  std::string wait_place_{"not recorded"};
  bool in_waiting_list_{false};
  std::list<std::shared_ptr<Event>>::iterator waiting_iter_;

  // An event is usually allocated on a coroutine stack, thus it cannot own a
  //   shared_ptr to the coroutine it is.
  // In this case there is no shared pointer to the event.
  // When the stack that contains the event frees, the event frees.
  // Weak reference to coroutine using rusty::rc::Weak with proper reference counting
  rusty::rc::Weak<Coroutine> wp_coro_{}; 

  // @unsafe
  virtual void Wait(uint64_t timeout=0) final;

  void Wait(function<bool(int)> f) {
    test_ = f;
    Wait();
  }

  virtual void log(){return;}
  virtual uint64_t GetCoroId();
  void RecordPlace(const char* file, int line);

  virtual bool Test();
  virtual bool ThreadSafeTest();
  virtual bool IsSlow();
  virtual bool IsReady() {
    if (!test_) return false;
    return test_(0);
  }

  // Composite events (AndEvent, OrEvent, QuorumEvent) need periodic polling
  // Added at END to preserve vtable layout for binary compatibility
  virtual bool IsCompositeEvent() { return false; }

  friend Reactor;
// protected:
  Event();
};

class DiskEvent : public Event {
 public:
	//maybe, instead of enum, this should be a queue
	enum Operation {WRITE=1, READ=2, FSYNC=4, WRITE_SPEC=8, SPECIAL=16};
  bool handled = false;
	bool sync = false;
	std::string file;
	Operation op;
	void* buffer;
	size_t size_;
	size_t count_;
	size_t read_;
	size_t written_;
	std::function<void()> func_;
	//create a more generic write instead of a map
  std::vector<std::map<int, i32>> cmd;
	
	friend inline Operation operator | (Operation op1, Operation op2) {
		return static_cast<Operation>(static_cast<int>(op1) | static_cast<int>(op2));
	}
  
	friend inline Operation operator & (Operation op1, Operation op2) {
		return static_cast<Operation>(static_cast<int>(op1) & static_cast<int>(op2));
	}

  DiskEvent(std::string file_, std::vector<std::map<int, i32>> cmd_, Operation op_);
	DiskEvent(std::string file_, void* ptr, size_t size, size_t count, Operation op_);
	DiskEvent(std::function<void()> f);

  void AddToList();

  void Write() {
		int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0777);
		std::string str;	
		int num1;
		i32 num2;
		for(int i = 0; i < cmd.size(); i++){
			for(auto it2 = cmd[i].begin(); it2 != cmd[i].end(); it2++){	
				num1 = it2->first;
				::write(fd, &num1, sizeof(int));
				str = ": ";
				::write(fd, str.c_str(), str.length());
				num2 = it2->second;
				::write(fd, &num2, sizeof(i32));
				str = "\n";
				::write(fd, str.c_str(), str.length());
			}
		}
		::close(fd);
    //handled = true;
  }

	void Read() {
		FILE* f = fopen(file.c_str(), "rb");
		if (f != NULL) {
			read_ = fread(buffer, size_, count_, f);
			fclose(f);
		}
	}
	
	void FSync() {
		int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT);
		::fsync(fd);
		::close(fd);
	}

	void Special() {
		func_();
	}
	int Write_Spec();

	int Handle() {
		int result = 0;
		if (op & WRITE) {
			Write();
		}
		if (op & READ) {
			Read();
		}
		if (op & FSYNC) {
			sync = true;
		}
		if (op & WRITE_SPEC) {
			result += Write_Spec();
		}
		return result;
	}
  bool IsReady() {
    return handled;
  }
	bool Test(){
		verify(0);
		return false;
	}
};

template <class Type>
class BoxEvent : public Event {
 public:
  Type content_{};
  bool is_set_{false};
  Type& Get() {
    return content_;
  }
  void Set(const Type& c) {
    is_set_ = true;
    content_ = c;
    Test();
  }
  void Clear() {
    is_set_ = false;
    content_ = {};
  }
  virtual bool IsReady() override {
    return is_set_;
  }
};

class IntEvent : public Event {

 public:
  IntEvent() {}
  IntEvent(int tar) :target_(tar) {}
  int value_{0};
  int target_{1};


  bool TestTrigger();

  int get() {
    return value_;
  }

  // @unsafe
  int Set(int n) {
    int t = value_;
    value_ = n;
    // TestTrigger();
    Test();
    return t;
  };

  bool IsReady() override {
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
  int Set(const int& v);

  void Wait(function<bool(int)> f);
  bool WaitUntilGreaterOrEqualThan(int x, int timeout=0);
};


class NeverEvent: public Event {
 public:
  bool IsReady() override {
    return false;
  }
};

class TimeoutEvent : public Event {
 public:
  uint64_t wakeup_time_{0};
  uint64_t wait_us_{0};
  TimeoutEvent(uint64_t wait_us)
      : wakeup_time_{Time::now(true) + wait_us}, wait_us_(wait_us) {}

  bool IsReady() override {
//    Log_debug("test timeout");
    return (Time::now(true) > wakeup_time_);
  }

  // @unsafe
  void Wait() {
    Event::Wait(wait_us_);
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

  bool IsReady() override {
    return std::any_of(events_.begin(), events_.end(), [](const std::shared_ptr<Event>& e){return e->IsReady();});
  }

  // Mark as composite event - will be polled in reactor loop
  bool IsCompositeEvent() override { return true; }
};

class AndEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;

  // Default constructor (mako-dev)
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
  
  void log() {
    for(int i = 0; i < events_.size(); i++){
      events_[i]->log();
    }
  }

  bool IsReady() override {
    // All events must be ready (or DONE) for AndEvent to be ready
    // Include null check for safety
    return std::all_of(events_.begin(), events_.end(),
                       [](const std::shared_ptr<Event>& e) {
                         return e && (e->IsReady() || e->status_ == Event::DONE);
                       });
  }

  // Mark as composite event - will be polled in reactor loop
  bool IsCompositeEvent() override { return true; }
};

class NEvent : public Event {
 public:
  vector<std::shared_ptr<Event>> events_;
  int number;

  void AddEvent() {
    // empty func for recursive variadic parameters
  }

  template<typename... Args>
  void AddEvent(std::shared_ptr<Event> x, Args... rest) {
    events_.push_back(x);
    AddEvent(rest...);
  }

  template<typename... Args>
  NEvent(std::shared_ptr<Event> first, Args... rest) {
    AddEvent(first, rest...);
  }

  bool IsReady() {
    int count = 0;
    for(auto index = events_.begin(); index != events_.end(); index++){
      if((*index)->IsReady()){
        count++;
        if(count == number){
          return true;
        }
      }
    }
    return false;
    
    //return std::all_of(events_.begin(), events_.end(), [](shared_ptr<Event> e){return e->IsReady();});
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

    bool IsReady() override{
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
    bool IsReady() override{
      // SUCCESS=0, REJECT=-10 (macros removed to avoid conflict with mako ErrorCode)
      return res_ == 0 || res_ == -10;
    }
};

class ThreadSafeIntEvent : public Event {
  std::mutex value_mtx_;

 public:
  ThreadSafeIntEvent();
  ThreadSafeIntEvent(int tar);
  std::atomic<int> value_{0};
  int target_{1};


  bool TestTrigger();

  int get() {
    return value_;
  }

  // Threadsafe
  int Set(int n) {
    std::lock_guard<std::mutex> lock(value_mtx_); // To protect value_
    int t = value_;
    value_ = n;
//    TestTrigger();
    ThreadSafeTest();
    return t;
  };

  virtual bool IsReady() override {
    if (test_) {
      return test_(value_);
    } else {
      return (value_ >= target_);
    }
  }
};

} // namespace rrr
