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

// @safe - Alarm: BTreeMap-backed timed-callback queue. Bodies use
// rusty::BTreeMap + rusty::Function + rrr::Time::now(false) — no raw
// pointer arithmetic, no syscalls, no Marshal chains. The raw
// `rrr::PollThread *holder` field is never dereferenced here and
// `set_holder` is a no-op stub.
export namespace rrr {

// @safe - see file header.
class Alarm: public FrequentJob {
 public:
  bool run_ = true;

  uint64_t next_id_ = 1;

  rrr::PollThread *holder = NULL;

  // std::map (not rusty::BTreeMap) — the transpiled BTreeMap port has a
  // chain of unresolved transpiler bugs (btree_internal: variant ._0
  // access, NodeRef temp binding, non-const member calls, copy-ctor
  // requirement on non-copyable T) that surface when iter() / clone() /
  // remove() are instantiated. std::map is semantically equivalent for
  // this use (ordered map with begin/end, insert, erase). Migrate back
  // when the upstream transpiler bugs are patched.
  std::map<uint64_t,
           std::pair<uint64_t, rusty::Function<void(void)>>> waiting_;

  std::map<std::pair<uint64_t, uint64_t>,
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
      uint64_t tm_now = rrr::Time::now(false);
      // Take a reference (not a copy) — the value holds a non-copyable
      // rusty::Function. `*it` is std::pair<const u64, V>.
      auto& item = *it;
      const uint64_t id = item.first;
      auto& val = item.second;
      uint64_t tm_out = val.first;
      ret = (tm_now > tm_out);
      if (ret) {
        auto& func = val.second;
        func();
        waiting_.erase(id);  // std::map::erase (was BTreeMap::remove)
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
    // std::map::emplace inserts in-place — moves the non-copyable
    // rusty::Function into the map without an intermediate copy.
    waiting_.emplace(id, std::make_pair(time, std::move(func)));
    return id;
  }

  bool remove(uint64_t id) {
    // std::map::erase returns the count of erased elements (0 or 1).
    return waiting_.erase(id) > 0;
  }

};

} // export namespace rrr
