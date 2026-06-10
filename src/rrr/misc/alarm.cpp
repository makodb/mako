module;

#include <rusty/rusty.hpp>
#include <cstdint>

export module rrr.alarm;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;

// @safe - Alarm: ordered timed-callback queue. Bodies use
// rusty::BTreeMap + rusty::Function — no raw pointer arithmetic, no
// syscalls, no Marshal chains.
//
// Alarm no longer inherits Job. The previous Job override surface
// (`Ready()` / `Work()` / `Done()`, plus the `tm_last_` / `period_`
// fields they read) was vestigial: `Arc<Job>` is only ever constructed
// from `OneTimeJob` in rrr today, never from `Alarm`. The historical
// add-Alarm-to-PollThread site at `src/deptran/server_worker.cc:237`
// has been commented out for a while, so `Alarm::Work()` was never
// being called via the polling loop in `PollThreadWorker::trigger_job`.
// Dropped along with the unused `run_` field and the `exe_next()`
// helper that only `Work()` called.
//
// Public API is now just `add(time, callback) -> id` and `remove(id)`,
// which matches the only consumer (`TimeoutALock` in alock.cpp). If
// the periodic-poll path is ever reactivated, wrap Alarm in an
// explicit `Job` adapter at the registration site.
export namespace rrr {

// Type alias for the BTreeMap value pair. Defined outside the DSL
// block so the inline-Rust source can refer to it by an opaque type
// name — the DSL grammar can't parse C++ function-template-arg
// spellings like `<void(void)>` directly.
using AlarmCallback = rusty::Function<void(void)>;
using AlarmEntry    = std::pair<uint64_t, AlarmCallback>;

// `Alarm` — ordered timed-callback queue.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
// a static `Alarm::new_()` factory.
//
// Behavioral diffs from the original C++ class:
//   * No default constructor — callers that previously
//     default-constructed (`Alarm{}`) now write `Alarm::new_()`
//     explicitly. (`OnceCell<Alarm>::get_or_init` in alock.cpp is
//     the one consumer; its lambda is updated to return
//     `Alarm::new_()`.)
//   * `next_id_` is renamed to `next_id` and `waiting_` to `waiting`
//     in the DSL — the DSL emits fields without the trailing-
//     underscore convention. No consumer reaches into either field
//     directly; the public surface is `add()`/`remove()`.
#if RUSTYCPP_RUST
struct Alarm {
    next_id: u64,
    waiting: rusty::BTreeMap<u64, AlarmEntry>,
}

impl Alarm {
    fn new() -> Alarm {
        Alarm {
            next_id: 1u64,
            waiting: rusty::BTreeMap::<u64, AlarmEntry>::new_in(rusty::alloc::Global{}),
        }
    }

    fn add(&mut self, time: u64, func: AlarmCallback) -> u64 {
        let id: u64 = self.next_id;
        self.next_id = id + 1u64;
        self.waiting.insert(id, std::make_pair(time, func));
        id
    }

    fn remove(&mut self, id: u64) -> bool {
        self.waiting.remove(id).is_some()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=alarm.1 version=1 rust_sha256=f8e02ea064f909552c7cf0301e1f41b11782a970f21d1554cd412db9f0e248e5*/
struct Alarm;

struct Alarm {
    uint64_t next_id;
    rusty::BTreeMap<uint64_t, AlarmEntry> waiting;

    static Alarm new_();
    uint64_t add(uint64_t time, AlarmCallback func);
    bool remove(uint64_t id);
};


Alarm Alarm::new_() {
    return Alarm{.next_id = static_cast<uint64_t>(1), .waiting = rusty::BTreeMap<uint64_t, AlarmEntry>::new_in(rusty::alloc::Global{})};
}

uint64_t Alarm::add(uint64_t time, AlarmCallback func) {
    uint64_t id = this->next_id;
    this->next_id = rusty::detail::deref_if_pointer_like(id) + static_cast<uint64_t>(1);
    this->waiting.insert(std::move(id), std::make_pair(std::move(time), std::move(func)));
    return std::move(id);
}

bool Alarm::remove(uint64_t id) {
    return this->waiting.remove(std::move(id)).is_some();
}
/*RUSTYCPP:GEN-END id=alarm.1*/

} // export namespace rrr
