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
/**
 * DragonBall is an interesting abstraction of event driven
 * programming. Typically after you collect all dragon balls you can
 * call for the holy dragon and he will make your wish become true.
 *
 * DragonBall is not thread-safe for now.
 */







#include <rusty/fn.hpp>
#include <rusty/function.hpp>

#include "../base/debugging.hpp"

namespace rrr {

class DragonBall {

 public:

  int64_t n_wait_ = -1;
  int64_t n_ready_ = 0;
  bool called_ = false;
  rusty::Function<void(void)> wish_{}; // this is your wish!

  bool th_safe_ = false;
  bool auto_trigger = true;

  DragonBall(bool th_safe = false)
      : th_safe_(th_safe), n_wait_() { }

  // Takes ownership of the wish callback; rusty::Function is move-only.
  DragonBall(const int32_t n_wait,
             rusty::Function<void(void)> wish,
             bool th_safe = false)
      : n_wait_(n_wait),
        wish_(std::move(wish)),
        th_safe_(th_safe) { };

  DragonBall(const DragonBall&) = delete;
  DragonBall& operator=(const DragonBall&) = delete;

  void set_wait(int64_t n_wait) {
    n_wait_ = n_wait;
  }

  //oid collect(int64_t n) {
  //    n_ready_ += n;
  //    if (auto_trigger) {
  //        trigger();
  //    }
  //}
  //


  /**
   * delete myself after triggered.
   */
  bool trigger() {
    //mtx_.lock();
    n_ready_++;
    verify(n_ready_ <= n_wait_);
    bool ready = (n_ready_ == n_wait_);
    //mtx_.unlock();

    if (ready) {
      verify(!called_);
      called_ = true;
      wish_();
      delete this;
    }
    return ready;
  }

  //only allows deconstructor called by itself.
 private:
  ~DragonBall() { }
};

typedef DragonBall ConcurrentDragonBall;

} // namespace deptran
