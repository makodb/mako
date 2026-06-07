module;

#include <rusty/rusty.hpp>
#include <cstdint>

export module rrr.alarm;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;
// PollThread used as a pointer here; the full definition lives in
// `rrr.reactor`. clang 22 rejects a GMF forward-decl
// (`namespace rrr { class PollThread; }`) when another imported module
// also exports PollThread — the GMF decl and the module's exported
// decl can't coexist for the same name. Importing the module that
// owns PollThread is the supported pattern.
import rrr.reactor;

// @safe - Alarm: ordered timed-callback queue. Bodies use
// rusty::BTreeMap + rusty::Function + rrr::Time::now(false) — no raw
// pointer arithmetic, no syscalls, no Marshal chains. Inherits Job
// directly (the old FrequentJob intermediate was flattened away — its
// only consumer was Alarm, and the `Ready()` body is inlined below).
export namespace rrr {

// @safe - see file header.
class Alarm: public Job {
 public:
  bool run_ = true;

  uint64_t next_id_ = 1;

  // FrequentJob-equivalent fields (FrequentJob is gone; Alarm carries
  // them directly).
  uint64_t tm_last_ = 0;
  uint64_t period_ = 50 * 1000;

  // BTreeMap keyed by alarm id (monotonic via next_id_), value is
  // `(tm_out, callback)`. Was std::map until the transpiled BTreeMap
  // port's iter()/remove()/non-copyable-T bug cluster (B1-B4) was
  // fixed upstream and the rusty.cppm umbrella aliases re-synced
  // with the namespace-strip refactor (rusty-cpp e680b1c).
  rusty::BTreeMap<uint64_t,
                  std::pair<uint64_t, rusty::Function<void(void)>>> waiting_;

  Alarm()
      : waiting_(rusty::BTreeMap<uint64_t,
                                 std::pair<uint64_t, rusty::Function<void(void)>>>
                     ::new_in(rusty::alloc::Global{})) { }

  Alarm(const Alarm&) = delete;
  Alarm& operator=(const Alarm&) = delete;

  // Movable so `OnceCell<Alarm>::get_or_init([] { return Alarm{}; })`
  // can install the value-returned lambda result into the cell.
  Alarm(Alarm&& other) noexcept
      : Job(),
        run_(other.run_),
        next_id_(other.next_id_),
        tm_last_(other.tm_last_),
        period_(other.period_),
        waiting_(std::move(other.waiting_)) {}

  ~Alarm() {
  }

  bool exe_next() {
    // Peek the smallest-keyed entry (BTreeMap iterates in key order,
    // and ids are monotonic via next_id_). If its `tm_out` has passed,
    // fire and erase. Otherwise leave it for a later poll.
    auto head = waiting_.first_key_value();
    if (head.is_none()) return false;
    const auto& kv = head.unwrap();
    const uint64_t id     = std::get<0>(kv);
    const uint64_t tm_out = std::get<1>(kv).first;
    const uint64_t tm_now = rrr::Time::now(false);
    if (tm_now <= tm_out) return false;

    // Remove the entry by key (BTreeMap::remove returns Option<V>),
    // then call the stored callback on the moved-out Function. We
    // call after the remove so the callback can re-arm against
    // `*this` without map-mutation-during-iteration concerns.
    auto popped = waiting_.remove(id);
    verify(popped.is_some());
    auto val = popped.unwrap();
    val.second();
    return true;
  }

  // @safe - rrr::Time::now(false) flows through rusty::sys::time::clock_*_us.
  bool Ready() override {
    uint64_t tm_now = rrr::Time::now(false);
    uint64_t s = tm_now - tm_last_;
    if (s > period_) {
      tm_last_ = tm_now;
      return true;
    }
    return false;
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
    // BTreeMap::remove returns Option<V> (Some if the key was present,
    // None otherwise).
    return waiting_.remove(id).is_some();
  }

};

} // export namespace rrr
