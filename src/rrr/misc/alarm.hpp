#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>
/**
 * This is an alarmer.
 */




// @unsafe




#include "../base/all.hpp"
#include "../reactor/reactor.h"


namespace rrr {

class Alarm: public FrequentJob {
 public:
  bool run_ = true;

  uint64_t next_id_ = 1;

  // either a thread_loop_holder or a epoll holder.
  rrr::PollThread *holder = NULL;


  // id -> <alarm_time, func>;
  rusty::BTreeMap<uint64_t,
           std::pair<uint64_t, rusty::Function<void(void)>>> waiting_;

  // <time, id> -> func
  rusty::BTreeMap<std::pair<uint64_t, uint64_t>,
           rusty::Function<void(void)> > idx_time_;

  Alarm() : waiting_(), idx_time_()
  {
    period_ = 50 * 1000; // 50ms;
  }

  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;

  ~Alarm() {
  }

  void set_holder(rrr::PollThread *mgr) {
  }

  bool exe_next() {
    bool ret = false;
    auto it = waiting_.begin();
    if (it != waiting_.end()) {
      uint64_t tm_now = rrr::Time::now();
      auto item = *it;
      const uint64_t id = item.first;
      uint64_t tm_out = item.second.first;
      ret = (tm_now > tm_out);
      if (ret) {
        auto& func = item.second.second;
        func();
        waiting_.remove(id);
      }
    }
    return ret;
  }

  bool Done() override {
    verify(0);
    return true;
  }

  void Work() override {
    while (exe_next()) { }
  }

//    void alarm_loop() {
//	while (run_) {
//	    run();
//	    apr_sleep(10 * 1000); // triggered every 10 ms
//	}
//    }

  // @unsafe - Adds alarm callback (uses rusty::HashMap::insert and std::make_pair)
  uint64_t add(uint64_t time, rusty::Function<void(void)> func) {
    //Log::debug("add timeout callback");
    uint64_t id = next_id_++;
    waiting_.insert(id, std::make_pair(time, std::move(func)));
    //	idx_time_[std::make_pair(time, id)] = func;
    return id;
  }

  /**
   * NEW: calling this will ensure that the timeout alarm
   * will not be executed. But care for DEADLOCKS !!!
   * If returns true, the callback is successfully removed
   * and will never be invoked. If not, it is not sure that
   * whether this callback will be invoked or not.
   */
  // @unsafe - Removes alarm callback (calls rusty::HashMap::remove)
  bool remove(uint64_t id) {
    return waiting_.remove(id).is_some();
  }

};

} // namespace rrr
