module;

#include <rusty/rusty.hpp>
/**
 * This is an alarmer.
 */




// @unsafe

export module rrr:misc.alarm;

import <mutex>;
import <functional>;

import :base.misc;
import :reactor.reactor;


export namespace rrr {

class Alarm: public FrequentJob {
 public:
  bool run_ = true;
  //    std::mutex lock_;

  uint64_t next_id_ = 1;

  // either a thread_loop_holder or a epoll holder.
  rrr::PollThread *holder = NULL;


  // id -> <alarm_time, func>;
  rusty::BTreeMap<uint64_t,
           std::pair<uint64_t, std::function<void(void)>>> waiting_;

  // <time, id> -> func
  rusty::BTreeMap<std::pair<uint64_t, uint64_t>,
           std::function<void(void)> > idx_time_;

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
    //	std::lock_guard<std::mutex> guard(lock_);
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

  // @unsafe - Adds alarm callback (uses std::map::operator[] and std::make_pair)
  uint64_t add(uint64_t time, std::function<void(void)> func) {
    //	std::lock_guard<std::mutex> guard(lock_);
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
  // @unsafe - Removes alarm callback (calls std::map::erase)
  bool remove(uint64_t id) {
    //	std::lock_guard<std::mutex> guard(lock_);
    return waiting_.remove(id).is_some();
  }

};

} // namespace rrr
