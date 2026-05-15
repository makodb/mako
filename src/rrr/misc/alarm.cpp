module;

#include <rusty/rusty.hpp>
#include <cstdint>

// PollThread is fully defined in reactor.h (still a textual header).
// Forward-declare in GMF so the pointer-only use here has
// global-module attachment that matches reactor.h's declaration.
namespace rrr { class PollThread; }

export module rrr.alarm;

import std;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;

export namespace rrr {

class Alarm: public FrequentJob {
 public:
  bool run_ = true;

  uint64_t next_id_ = 1;

  rrr::PollThread *holder = NULL;

  rusty::BTreeMap<uint64_t,
           std::pair<uint64_t, rusty::Function<void(void)>>> waiting_;

  rusty::BTreeMap<std::pair<uint64_t, uint64_t>,
           rusty::Function<void(void)> > idx_time_;

  Alarm() : waiting_(), idx_time_()
  {
    period_ = 50 * 1000;
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
      const uint64_t id = std::get<0>(item);
      auto& val = std::get<1>(item);
      uint64_t tm_out = val.first;
      ret = (tm_now > tm_out);
      if (ret) {
        auto& func = val.second;
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

  uint64_t add(uint64_t time, rusty::Function<void(void)> func) {
    uint64_t id = next_id_++;
    waiting_.insert(id, std::make_pair(time, std::move(func)));
    return id;
  }

  bool remove(uint64_t id) {
    return waiting_.remove(id).is_some();
  }

};

} // export namespace rrr
