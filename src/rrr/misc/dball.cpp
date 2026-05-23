module;

#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <cstdint>

export module rrr.dball;

import std;
import rrr.debugging;

// @safe - DragonBall: counter-trigger that calls a stored
// `rusty::Function` once `n_ready_` reaches `n_wait_`. The only
// genuinely unsafe op is the `delete this` self-destruct at the end
// of `trigger()`, which carries an inline `// @unsafe { }` block.
export namespace rrr {

// @safe - see file header.
class DragonBall {

 public:

  int64_t n_wait_ = -1;
  int64_t n_ready_ = 0;
  bool called_ = false;
  rusty::Function<void(void)> wish_{};

  bool th_safe_ = false;
  bool auto_trigger = true;

  DragonBall(bool th_safe = false)
      : th_safe_(th_safe), n_wait_() { }

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

  bool trigger() {
    n_ready_++;
    verify(n_ready_ <= n_wait_);
    bool ready = (n_ready_ == n_wait_);

    if (ready) {
      verify(!called_);
      called_ = true;
      wish_();
      // @unsafe { self-destructing trigger; DragonBall::trigger is the
      //           single owner so the delete is final by construction. }
      {
        delete this;
      }
    }
    return ready;
  }

 private:
  ~DragonBall() { }
};

typedef DragonBall ConcurrentDragonBall;

} // export namespace rrr
