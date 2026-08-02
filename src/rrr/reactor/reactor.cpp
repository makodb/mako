// rrr.reactor — consolidated event/fiber/reactor module.
//
// Combines what were event.h+.cc, fiber_impl.h+.cc, quorum_event.h+.cc,
// reactor.h+.cc, and fiber_context_runtime.cc into a single C++23
// named-module interface unit. The cluster's class types
// (`rrr::Event`, `rrr::Fiber`, `rrr::Reactor`, `janus::QuorumEvent`,
// `rrr::fiber_task_t`, etc.) form a mutually-recursive web of forward
// declarations and out-of-line member definitions; a single module unit
// is the natural shape (separate module interfaces would require
// circular `import` lines, which the standard forbids).
//
// The arch-specific context-switch trampolines stay outside the module
// — `fiber_context_x86_64.cc` and `fiber_context_aarch64.cc` are tiny
// `extern "C"` asm-only TUs and don't need module attachment.
module;

#include <std_compat.hpp>
#include <std_annotation.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include <rusty/arc.hpp>
#include <rusty/async.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/mutex.hpp>
#include <rusty/option.hpp>
#include <rusty/refcell.hpp>
#include <rusty/rusty.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/sync/mpsc.hpp>
#include <rusty/thread.hpp>
#include <rusty/vecdeque.hpp>

export module rrr.reactor;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.logging;
import rrr.misc;
import rrr.strop;
import rrr.threading;
import rrr.epoll_wrapper;
import rrr.pollable_proxy;


// ===========================================================================
// Class declarations (from former event.h, fiber_impl.h, reactor.h block 1)
// ===========================================================================
// @safe - Reactor / Event / Fiber declarations. Class declarations
// carry their own annotations; methods that genuinely cross into
// fiber context switching / raw pointer access / thread-local lookup
// have per-method `// @unsafe` overrides. The rest is analyzed as
// @safe by default.
// Forward declarations for the flattened QuorumEvent (defined in `namespace
// janus` far below). Hoisted here so `rrr::event_make` — defined inside the
// `rrr` block, before janus opens — can dispatch `create_sp_event<QuorumEvent>`
// to the janus factory. The leaf events live in `rrr`, but QuorumEvent stays in
// `janus` (its 41 deptran use sites name it there).
/*RUSTYCPP:GEN-DISPATCH-BEGIN*/
namespace rusty { namespace detail {
RUSTY_METHOD_DISPATCH(extract_if)
RUSTY_METHOD_DISPATCH(get_self)
RUSTY_METHOD_DISPATCH(insert)
RUSTY_METHOD_DISPATCH(is_composite_event)
RUSTY_METHOD_DISPATCH(is_ready)
RUSTY_METHOD_DISPATCH(push_back)
RUSTY_METHOD_DISPATCH(retain)
RUSTY_METHOD_DISPATCH(size)
RUSTY_METHOD_DISPATCH(upgrade)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

export namespace janus {
struct QuorumEvent;
rusty::Arc<QuorumEvent> quorum_event_make(int32_t n_total, int32_t quorum);
}

export namespace rrr {

// --- from event.h --------------------------------------------------------

class Reactor;
class Fiber;

// Per-thread scheduler singletons + the running-fiber slot. Namespace-
// scope (not class-static) so the DSL singleton/save/restore logic can
// name them; `inline` keeps vague linkage (same clang-21 dup-symbol
// rationale as the former class members).
inline thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_{};
inline thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_{};
inline thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_fiber_th_{};


// `EventState` — the cleanly-DSL-able portion of an Event's data, factored out
// of the hand-written `class Event` into an inline-Rust struct. These nine
// fields are pure rusty/POD types with no interior-mutability or vtable
// constraint, so they transpile to a plain aggregate composed onto Event as
// the member `state_`. The fields that must stay hand-written remain directly
// on Event: `status_` (rusty::Cell), the `EventStatus` enum, `self_`
// (std::weak_ptr), the `_dbg_p_scheduler_` void*, and the
// `#ifdef EVENT_TIMEOUT_CHECK __debug_timeout_`.
// `wp_fiber_` is a weak ref because an Event usually lives on a fiber stack and
// must not keep its owning fiber alive.
// Event status machine, hoisted out of `class Event` (flattening S4 prep):
// the flat DSL structs each carry a `status_: Cell<EventStatus>`, and a
// DSL `#[repr(i32)] enum` lowers to exactly this `enum class` shape. All
// call sites already spell `EventStatus::X` (S2).
enum class EventStatus : int32_t {
  INIT = 0,
  WAIT = 1,
  READY = 2,
  DONE = 3,
  TIMEOUT = 4,
  DEBUG = 5,
};

using EventTestFn = rusty::Function<bool(int) const>;
#if RUSTYCPP_RUST
struct EventState {
    __debug_creator: i32,
    test_: RefCell<EventTestFn>,
    wakeup_time_: Cell<u64>,
    rcd_wait_: Cell<bool>,
    wait_place_: RefCell<std::string>,
    wp_fiber_: RefCell<rusty::rc::Weak<Fiber>>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.event_state version=1 rust_sha256=074996f76ecb4e468e7e8e788e5f5ecddd91eefd2032b143f448655783c3892e*/
struct EventState;

struct EventState {
    int32_t __debug_creator;
    rusty::RefCell<EventTestFn> test_;
    rusty::Cell<uint64_t> wakeup_time_;
    rusty::Cell<bool> rcd_wait_;
    rusty::RefCell<std::string> wait_place_;
    rusty::RefCell<rusty::rc::Weak<Fiber>> wp_fiber_;
};
/*RUSTYCPP:GEN-END id=reactor.event_state*/

// `EventPollable` — the reactor's polymorphic surface over queued events
// (flattening S4): exactly what the loop/timeout/prune machinery invokes
// through the four event queues, and nothing else. Data-free trait; the
// hand-written `Event` derives it as a bridge during the transition, and
// each flattened per-kind DSL struct will `#[cpp_inherit] impl` it.
// wait()/set_self()/state_ stay OFF the trait: they are only ever touched
// through concrete-typed handles.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
pub trait EventPollable {
    fn test(&self) -> bool;
    fn is_ready(&self) -> bool;
    fn log(&self);
    fn status(&self) -> EventStatus;
    fn set_status(&self, s: EventStatus);
    fn wakeup_time(&self) -> u64;
    fn prunable(&self) -> bool;
    fn set_prunable(&self, v: bool);
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>>;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.event_pollable version=1 rust_sha256=342f3c1646b41349ac6febce95fc5c9a264abe74cb3f7e2d2c3ba25df0feb539*/
class EventPollable;

class EventPollable {
public:
    virtual ~EventPollable() noexcept(false) {}
    virtual bool test() const = 0;
    virtual bool is_ready() const = 0;
    virtual void log() const = 0;
    virtual EventStatus status() const = 0;
    virtual void set_status(EventStatus s) const = 0;
    virtual uint64_t wakeup_time() const = 0;
    virtual bool prunable() const = 0;
    virtual void set_prunable(bool v) const = 0;
    virtual rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const = 0;
    EventPollable(const EventPollable&) = delete;
    EventPollable& operator=(const EventPollable&) = delete;
    EventPollable(EventPollable&&) = delete;
    EventPollable& operator=(EventPollable&&) = delete;
protected:
    EventPollable() = default;
};

template <class U> class EventPollableAdapter;
template <class U> class EventPollableAdapterRef;
template <class U> class EventPollableAdapterRefMut;
/*RUSTYCPP:GEN-END id=reactor.event_pollable*/

// Kernel forward declarations (definitions live with the other out-of-line
// event machinery below): the flattened DSL structs' generated method
// bodies call these by ordinary lookup, so the names must exist first.
template <typename W> void event_wait_impl(const W& self, uint64_t timeout);
template <typename W> bool event_test_impl(const W& self);

// Shared core-field kernels for the flattened DSL structs (each carries
// the same five event-core fields, so one template set serves them all;
// the generated method bodies resolve these by ordinary lookup, and the
// templates instantiate once the concrete struct is complete).
using SrcFileCStr = const char*;
// Duck-typed event-core kernels, authored as inline Rust DSL — convertible
// since rusty-cpp #32/#33. Params renamed `self`->`ev` (a free-function param
// named `self` lowers to a method receiver).
#if RUSTYCPP_RUST
fn event_core_self_lock<W>(ev: &W) -> rusty::Option<rusty::Arc<EventPollable>> {
    ev.self_.upgrade()
}
fn event_core_set_self<W>(ev: &mut W, p: rusty::sync::Weak<EventPollable>) {
    ev.self_ = p;
}
fn event_core_wakeup_time<W>(ev: &W) -> u64 {
    ev.state_.wakeup_time_.get()
}
fn event_core_upgrade_fiber<W>(ev: &W) -> rusty::Option<rusty::Rc<Fiber>> {
    ev.state_.wp_fiber_.borrow().upgrade()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.3 version=1 rust_sha256=8aa535460ee9fdf427c4e23e24455948268424b1d896b69639b4b35047e8e754*/
template<typename W>
uint64_t event_core_wakeup_time(const W& ev);

template<typename W>
rusty::Option<rusty::Arc<EventPollable>> event_core_self_lock(const W& ev) {
    return ev.self_.upgrade();
}

template<typename W>
void event_core_set_self(W& ev, rusty::sync::Weak<EventPollable> p) {
    W* ev_shadow1 = &ev;
    (*ev_shadow1).self_ = std::move(p);
}

template<typename W>
uint64_t event_core_wakeup_time(const W& ev) {
    return ev.state_.wakeup_time_.get();
}

template<typename W>
rusty::Option<rusty::Rc<Fiber>> event_core_upgrade_fiber(const W& ev) {
    return rusty::deref_call(rusty::borrow(ev.state_.wp_fiber_), rusty::detail::__mdisp_upgrade{});
}
/*RUSTYCPP:GEN-END id=reactor.3*/
template <typename W> void event_core_record_place(const W& self, SrcFileCStr file, int line) {
  char buff[200];
  sprintf(buff, "%s:%d", file, line);
  (*self.state_.wait_place_.borrow_mut()) += std::string(buff);
  self.state_.rcd_wait_.set(true);
}
// Current fiber id — matches Event::get_fiber_id (reads the running
// fiber, not event state), declared here for the flat structs that expose
// it (defined after Fiber below).
uint64_t event_core_get_fiber_id();
// Seeds an event's EventState (wait_place_ tag + creating-fiber capture),
// matching the legacy Event constructor. Declared here so the BoxEvent
// hand-bridge's inline ctor (below) can call it; defined after Fiber.
void event_state_seed(const EventState& st);

// Per-type construction factory used by Reactor::create_sp_event. Legacy
// Event subclasses fall through to make_shared (they have real
// constructors); the flattened DSL structs — field-wise aggregates with
// no default arguments — dispatch to plain factory functions that supply
// their defaults in exactly one audited place each.
struct NeverEvent;
struct TimeoutEvent;
struct IntEvent;
struct WaitAny;
struct WaitAll;
rusty::Arc<NeverEvent> never_event_make();
rusty::Arc<TimeoutEvent> timeout_event_make(uint64_t wait_us);
rusty::Arc<IntEvent> int_event_make(int32_t target);
rusty::Arc<WaitAny> waitany_make(rusty::Arc<EventPollable> a, rusty::Arc<EventPollable> b);
rusty::Arc<WaitAll> waitall_make();
rusty::Arc<WaitAll> waitall_make_from(const rusty::Vec<rusty::Arc<EventPollable>>& evs);
template<class Type> struct BoxEvent;
template<class Type> rusty::Arc<BoxEvent<Type>> boxevent_make();
// Detect BoxEvent<T> instantiations so event_make can dispatch to the template
// factory (concrete-type is_same_v can't match a class-template instantiation).
template<class> struct is_box_event : std::false_type {};
template<class T> struct is_box_event<BoxEvent<T>> : std::true_type {};
template<class> struct box_event_payload;
template<class T> struct box_event_payload<BoxEvent<T>> { using type = T; };
template <typename Ev, typename... Args>
rusty::Arc<Ev> event_make(Args&&... args) {
  if constexpr (std::is_same_v<Ev, NeverEvent>) {
    return never_event_make();
  } else if constexpr (std::is_same_v<Ev, TimeoutEvent>) {
    return timeout_event_make(std::forward<Args>(args)...);
  } else if constexpr (std::is_same_v<Ev, IntEvent>) {
    if constexpr (sizeof...(Args) == 0) {
      return int_event_make(1);  // IntEvent() had target_{1}
    } else {
      return int_event_make(std::forward<Args>(args)...);
    }
  } else if constexpr (std::is_same_v<Ev, janus::QuorumEvent>) {
    return janus::quorum_event_make(std::forward<Args>(args)...);
  } else if constexpr (std::is_same_v<Ev, WaitAny>) {
    return waitany_make(std::forward<Args>(args)...);
  } else if constexpr (std::is_same_v<Ev, WaitAll>) {
    if constexpr (sizeof...(Args) == 0) {
      return waitall_make();               // default ctor
    } else {
      return waitall_make_from(std::forward<Args>(args)...);  // vector ctor
    }
  } else if constexpr (is_box_event<Ev>::value) {
    return boxevent_make<typename box_event_payload<Ev>::type>();  // BoxEvent<T>()
  } else {
    return rusty::Arc<Ev>::make(std::forward<Args>(args)...);
  }
}



// `BoxEvent<Type>` — a one-shot slot event (ready once `set()`). FLATTENED (S4):
// a GENERIC inline-Rust DSL struct deriving EventPollable via `#[cpp_inherit]`
// (the transpiler DOES lower generic structs: `struct BoxEvent<Type>` +
// `impl<Type> ... for BoxEvent<Type>` -> `template<class Type> struct BoxEvent :
// public EventPollable`). Carries the five event-core fields + the slot payload,
// driven by the shared event_wait_impl / event_test_impl / event_core_* kernels.
// The generic slot ops (get returns Type by value; set/clear deref-assign the
// RefCell<Type>; clear value-inits Type{}) stay hand-written @unsafe TEMPLATE
// kernels the DSL calls. Construction (the former default ctor) is the
// `boxevent_make<Type>` factory, dispatched from event_make via is_box_event<Ev>.
// Instantiated with <int>/<bool>/<std::string>; the kernels + factory are
// exported templates so cross-TU (deptran) instantiation resolves. The former
// StatusBox (rcc) subclass was removed (dead-convenience — see rcc/tx.h).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block.
// @unsafe - the generic slot-op kernels the DSL body calls.
template<class Type> Type boxevent_get(const BoxEvent<Type>& self);
template<class Type> void boxevent_set(const BoxEvent<Type>& self, const Type& c);
template<class Type> void boxevent_clear(const BoxEvent<Type>& self);
#if RUSTYCPP_RUST
struct BoxEvent<Type> {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
    content_: RefCell<Type>,
    is_set_: Cell<bool>,
}

impl<Type> BoxEvent<Type> {
    fn get(&self) -> Type {
        boxevent_get(self)
    }
    fn set(&self, c: &Type) {
        boxevent_set(self, c)
    }
    fn clear(&self) {
        boxevent_clear(self)
    }
    fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    fn is_composite_event(&self) -> bool {
        false
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl<Type> EventPollable for BoxEvent<Type> {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        self.is_set_.get()
    }
    fn log(&self) {}
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.box_event version=1 rust_sha256=bdc2d8f2dc9495014bacdc53d10a179405f30e640e2649f012051b79adabf633*/
template<typename Type>
struct BoxEvent;

template<typename Type>
struct BoxEvent : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    rusty::RefCell<Type> content_;
    rusty::Cell<bool> is_set_;
    BoxEvent(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init, rusty::RefCell<Type> content__init, rusty::Cell<bool> is_set__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)), content_(std::move(content__init)), is_set_(std::move(is_set__init)) {}
    BoxEvent(BoxEvent&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)), content_(std::move(other.content_)), is_set_(std::move(other.is_set_)) {}


    Type get() const {
        return boxevent_get((*this));
    }
    void set(const Type& c) const {
        boxevent_set((*this), c);
    }
    void clear() const {
        boxevent_clear((*this));
    }
    void wait() const {
        event_wait_impl((*this), static_cast<uint64_t>(0));
    }
    void wait_timeout(uint64_t timeout) const {
        event_wait_impl((*this), std::move(timeout));
    }
    bool is_composite_event() const {
        return false;
    }
    rusty::Option<rusty::Arc<EventPollable>> get_self() const {
        return event_core_self_lock((*this));
    }
    void set_self(rusty::sync::Weak<EventPollable> self_ptr) {
        event_core_set_self((*this), std::move(self_ptr));
    }
    bool test() const {
        return event_test_impl((*this));
    }
    bool is_ready() const {
        return this->is_set_.get();
    }
    void log() const {
    }
    EventStatus status() const {
        return this->status_.get();
    }
    void set_status(EventStatus s) const {
        this->status_.set(std::move(s));
    }
    uint64_t wakeup_time() const {
        return event_core_wakeup_time((*this));
    }
    bool prunable() const {
        return this->prunable_.get();
    }
    void set_prunable(bool v) const {
        this->prunable_.set(std::move(v));
    }
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const {
        return event_core_upgrade_fiber((*this));
    }
};
/*RUSTYCPP:GEN-END id=reactor.box_event*/

// Template factory + slot-op kernels for BoxEvent<Type>, defined after the
// struct is complete and in this exported module region so deptran's
// BoxEvent<int>/<bool>/<std::string> instantiations resolve.
template<class Type>
rusty::Arc<BoxEvent<Type>> boxevent_make() {
  auto sp = rusty::Arc<BoxEvent<Type>>::make(
      rusty::Cell<EventStatus>::new_(EventStatus::INIT),  // status_
      rusty::thread::current_id(),                        // owner_thread_
      EventState{},                                       // state_
      rusty::Cell<bool>::new_(true),                      // prunable_
      rusty::sync::Weak<EventPollable>(),                 // self_
      rusty::RefCell<Type>(),                             // content_ (Type{})
      rusty::Cell<bool>::new_(false));                    // is_set_
  event_state_seed(sp->state_);
  return sp;
}

// @unsafe - returns the slot payload by value (copy out of the RefCell).
template<class Type> Type boxevent_get(const BoxEvent<Type>& self) {
  return *self.content_.borrow();
}
// @unsafe - deref-assign the RefCell<Type> slot + run the readiness test.
template<class Type> void boxevent_set(const BoxEvent<Type>& self, const Type& c) {
  self.is_set_.set(true);
  (*self.content_.borrow_mut()) = c;
  self.test();
}
// @unsafe - value-init the slot back to Type{}.
template<class Type> void boxevent_clear(const BoxEvent<Type>& self) {
  self.is_set_.set(false);
  (*self.content_.borrow_mut()) = Type{};
}

// `IntEvent` — an Event that fires when value_ reaches target_ (or a custom
// inherited `test_` predicate passes). Hand-written subclass of the stateful
// `Event` base (Event is intentionally not trait-ified — it carries data fields
// and non-pure default-bodied virtuals).
// `IntEvent` — fires when value_ reaches target_ (or a custom `test_`
// predicate passes). FLATTENED (S4): flat inline-Rust DSL struct on the
// NeverEvent pattern. Defaults (value_=0, target_=1) live in
// int_event_make; set() runs the shared test kernel and returns the
// previous value, exactly as before.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
int32_t int_event_set(const IntEvent& self, int32_t n);
bool int_event_is_ready(const IntEvent& self);
uint64_t event_core_get_fiber_id();

#if RUSTYCPP_RUST
struct IntEvent {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
    value_: Cell<i32>,
    target_: Cell<i32>,
}

impl IntEvent {
    fn get(&self) -> i32 {
        self.value_.get()
    }
    fn set(&self, n: i32) -> i32 {
        int_event_set(self, n)
    }
    fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    fn record_place(&self, file: SrcFileCStr, line: i32) {
        event_core_record_place(self, file, line)
    }
    fn get_fiber_id(&self) -> u64 {
        event_core_get_fiber_id()
    }
    fn is_composite_event(&self) -> bool {
        false
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl EventPollable for IntEvent {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        int_event_is_ready(self)
    }
    fn log(&self) {}
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.int_event version=1 rust_sha256=51795c7da5e97a13f4a95dc2998b9eb8907aaef9c77b65eb1811026ee70855d8*/
struct IntEvent;

struct IntEvent : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    rusty::Cell<int32_t> value_;
    rusty::Cell<int32_t> target_;
    IntEvent(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init, rusty::Cell<int32_t> value__init, rusty::Cell<int32_t> target__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)), value_(std::move(value__init)), target_(std::move(target__init)) {}
    IntEvent(IntEvent&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)), value_(std::move(other.value_)), target_(std::move(other.target_)) {}


    int32_t get() const;
    int32_t set(int32_t n) const;
    void wait() const;
    void wait_timeout(uint64_t timeout) const;
    void record_place(SrcFileCStr file, int32_t line) const;
    uint64_t get_fiber_id() const;
    bool is_composite_event() const;
    rusty::Option<rusty::Arc<EventPollable>> get_self() const;
    void set_self(rusty::sync::Weak<EventPollable> self_ptr);
    bool test() const;
    bool is_ready() const;
    void log() const;
    EventStatus status() const;
    void set_status(EventStatus s) const;
    uint64_t wakeup_time() const;
    bool prunable() const;
    void set_prunable(bool v) const;
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const;
};


int32_t IntEvent::get() const {
    return this->value_.get();
}

int32_t IntEvent::set(int32_t n) const {
    return int_event_set((*this), std::move(n));
}

void IntEvent::wait() const {
    event_wait_impl((*this), static_cast<uint64_t>(0));
}

void IntEvent::wait_timeout(uint64_t timeout) const {
    event_wait_impl((*this), std::move(timeout));
}

void IntEvent::record_place(SrcFileCStr file, int32_t line) const {
    event_core_record_place((*this), std::move(file), std::move(line));
}

uint64_t IntEvent::get_fiber_id() const {
    return event_core_get_fiber_id();
}

bool IntEvent::is_composite_event() const {
    return false;
}

rusty::Option<rusty::Arc<EventPollable>> IntEvent::get_self() const {
    return event_core_self_lock((*this));
}

void IntEvent::set_self(rusty::sync::Weak<EventPollable> self_ptr) {
    event_core_set_self((*this), std::move(self_ptr));
}

bool IntEvent::test() const {
    return event_test_impl((*this));
}

bool IntEvent::is_ready() const {
    return int_event_is_ready((*this));
}

void IntEvent::log() const {
}

EventStatus IntEvent::status() const {
    return this->status_.get();
}

void IntEvent::set_status(EventStatus s) const {
    this->status_.set(std::move(s));
}

uint64_t IntEvent::wakeup_time() const {
    return event_core_wakeup_time((*this));
}

bool IntEvent::prunable() const {
    return this->prunable_.get();
}

void IntEvent::set_prunable(bool v) const {
    this->prunable_.set(std::move(v));
}

rusty::Option<rusty::Rc<Fiber>> IntEvent::upgrade_fiber() const {
    return event_core_upgrade_fiber((*this));
}
/*RUSTYCPP:GEN-END id=reactor.int_event*/

// @safe - sets value_ and runs the readiness test kernel; returns the
// previous value (verbatim from the legacy IntEvent::set).
inline int32_t int_event_set(const IntEvent& self, int32_t n) {
  int32_t t = self.value_.get();
  self.value_.set(n);
  event_test_impl(self);
  return t;
}

// @unsafe - invokes the state_.test_ rusty::Function (custom predicate).
inline bool int_event_is_ready(const IntEvent& self) {
  auto guard = self.state_.test_.borrow();
  if (*guard) {
    return (*guard)(self.value_.get());
  }
  return self.value_.get() >= self.target_.get();
}

// `SharedIntEvent` — a shared counter that wakes IntEvent waiters when
// it crosses their thresholds. The `rusty::Arc<IntEvent>` element
// type stays std (Reactor::create_sp_event hands out shared_ptr — a
// declared boundary type).

struct SharedIntEvent;

// Hand-written backing free fns for the DSL methods below — the bodies
// drive Reactor::create_sp_event / Event-status machinery (not
// DSL-expressible). Definitions near the bottom of this file.
int shared_int_event_set(SharedIntEvent& self, const int& v);
bool shared_int_event_wait_until_gte(SharedIntEvent& self, int x, int timeout);
void shared_int_event_wait(SharedIntEvent& self, EventTestFn f);

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * `wait_until_gte` lost its `timeout = 0` default argument (DSL fns
//     have no default args); the one-arg call sites now pass 0
//     explicitly.
//   * The struct stays a plain aggregate, so the widespread
//     `SharedIntEvent x{};` member value-init keeps zeroing `value_`.
#if RUSTYCPP_RUST
struct SharedIntEvent {
    value_: i32,
    events_: Vec<rusty::Arc<IntEvent>>,
}

impl SharedIntEvent {
    fn set(&mut self, v: &i32) -> i32 {
        shared_int_event_set(self, v)
    }

    fn wait(&mut self, f: EventTestFn) {
        shared_int_event_wait(self, f)
    }

    fn wait_until_gte(&mut self, x: i32, timeout: i32) -> bool {
        shared_int_event_wait_until_gte(self, x, timeout)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.shared_int_event version=1 rust_sha256=e399535422082fc86a72614021d2a6e3ccfa69d63b28b68d79b0484f48293737*/
struct SharedIntEvent;

struct SharedIntEvent {
    int32_t value_;
    rusty::Vec<rusty::Arc<IntEvent>> events_;

    int32_t set(const int32_t& v);
    void wait(EventTestFn f);
    bool wait_until_gte(int32_t x, int32_t timeout);
};


int32_t SharedIntEvent::set(const int32_t& v) {
    return shared_int_event_set((*this), v);
}

void SharedIntEvent::wait(EventTestFn f) {
    shared_int_event_wait((*this), std::move(f));
}

bool SharedIntEvent::wait_until_gte(int32_t x, int32_t timeout) {
    return shared_int_event_wait_until_gte((*this), std::move(x), std::move(timeout));
}
/*RUSTYCPP:GEN-END id=reactor.shared_int_event*/


// `NeverEvent` — an Event that is never ready, used as a pure timeout/yield
// handle (`create_sp_event<NeverEvent>()->wait(us)`). Hand-written subclass of
// the stateful `Event` base; adds no fields and overrides only `is_ready()`.
// `NeverEvent` — never ready on its own; a pure timeout/yield handle
// (`create_sp_event<NeverEvent>()->wait_timeout(us)`). FIRST FLATTENED
// EVENT TYPE (S4): a flat inline-Rust DSL struct carrying the event core
// fields directly (so the event_wait_impl/event_test_impl kernels see the
// same duck-typed surface as the legacy Event), implementing EventPollable
// via #[cpp_inherit]. Constructed ONLY through Reactor::create_sp_event's
// event_make<NeverEvent>() factory (aggregate defaults live there — the
// DSL has no field initializers).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
struct NeverEvent {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
}

impl NeverEvent {
    fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    fn record_place(&self, file: SrcFileCStr, line: i32) {
        event_core_record_place(self, file, line)
    }
    fn is_composite_event(&self) -> bool {
        false
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl EventPollable for NeverEvent {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        false
    }
    fn log(&self) {}
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.never_event version=1 rust_sha256=208ba006360937022eeaadf8559e6353e25cfc09fdb59a9576ee3f26323d6edb*/
struct NeverEvent;

struct NeverEvent : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    NeverEvent(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)) {}
    NeverEvent(NeverEvent&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)) {}


    void wait_timeout(uint64_t timeout) const;
    void record_place(SrcFileCStr file, int32_t line) const;
    bool is_composite_event() const;
    rusty::Option<rusty::Arc<EventPollable>> get_self() const;
    void set_self(rusty::sync::Weak<EventPollable> self_ptr);
    bool test() const;
    bool is_ready() const;
    void log() const;
    EventStatus status() const;
    void set_status(EventStatus s) const;
    uint64_t wakeup_time() const;
    bool prunable() const;
    void set_prunable(bool v) const;
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const;
};


void NeverEvent::wait_timeout(uint64_t timeout) const {
    event_wait_impl((*this), std::move(timeout));
}

void NeverEvent::record_place(SrcFileCStr file, int32_t line) const {
    event_core_record_place((*this), std::move(file), std::move(line));
}

bool NeverEvent::is_composite_event() const {
    return false;
}

rusty::Option<rusty::Arc<EventPollable>> NeverEvent::get_self() const {
    return event_core_self_lock((*this));
}

void NeverEvent::set_self(rusty::sync::Weak<EventPollable> self_ptr) {
    event_core_set_self((*this), std::move(self_ptr));
}

bool NeverEvent::test() const {
    return event_test_impl((*this));
}

bool NeverEvent::is_ready() const {
    return false;
}

void NeverEvent::log() const {
}

EventStatus NeverEvent::status() const {
    return this->status_.get();
}

void NeverEvent::set_status(EventStatus s) const {
    this->status_.set(std::move(s));
}

uint64_t NeverEvent::wakeup_time() const {
    return event_core_wakeup_time((*this));
}

bool NeverEvent::prunable() const {
    return this->prunable_.get();
}

void NeverEvent::set_prunable(bool v) const {
    this->prunable_.set(std::move(v));
}

rusty::Option<rusty::Rc<Fiber>> NeverEvent::upgrade_fiber() const {
    return event_core_upgrade_fiber((*this));
}
/*RUSTYCPP:GEN-END id=reactor.never_event*/



// `TimeoutEvent` — ready once `wait_us_` microseconds have elapsed past
// construction (`wakeup_time_` is its OWN deadline field, distinct from
// `state_.wakeup_time_` which the wait machinery stamps). FLATTENED (S4):
// flat inline-Rust DSL struct on the NeverEvent pattern; the deadline is
// computed at construction inside timeout_event_make (the DSL has no
// field initializers). Its `wait()` keeps the historical no-argument
// shape: it waits with its own wait_us_ as the timeout.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
bool timeout_event_is_ready(const TimeoutEvent& self);

#if RUSTYCPP_RUST
struct TimeoutEvent {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
    wakeup_time_: u64,
    wait_us_: u64,
}

impl TimeoutEvent {
    fn wait(&self) {
        event_wait_impl(self, self.wait_us_)
    }
    fn is_composite_event(&self) -> bool {
        false
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl EventPollable for TimeoutEvent {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        timeout_event_is_ready(self)
    }
    fn log(&self) {}
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.timeout_event version=1 rust_sha256=e9c4d62ad6f1952d10ee6e47c695016ccc924846aafddfeda0d9c6a0400b2532*/
struct TimeoutEvent;

struct TimeoutEvent : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    uint64_t wakeup_time_;
    uint64_t wait_us_;
    TimeoutEvent(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init, uint64_t wakeup_time__init, uint64_t wait_us__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)), wakeup_time_(std::move(wakeup_time__init)), wait_us_(std::move(wait_us__init)) {}
    TimeoutEvent(TimeoutEvent&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)), wakeup_time_(std::move(other.wakeup_time_)), wait_us_(std::move(other.wait_us_)) {}


    void wait() const;
    bool is_composite_event() const;
    rusty::Option<rusty::Arc<EventPollable>> get_self() const;
    void set_self(rusty::sync::Weak<EventPollable> self_ptr);
    bool test() const;
    bool is_ready() const;
    void log() const;
    EventStatus status() const;
    void set_status(EventStatus s) const;
    uint64_t wakeup_time() const;
    bool prunable() const;
    void set_prunable(bool v) const;
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const;
};


void TimeoutEvent::wait() const {
    event_wait_impl((*this), this->wait_us_);
}

bool TimeoutEvent::is_composite_event() const {
    return false;
}

rusty::Option<rusty::Arc<EventPollable>> TimeoutEvent::get_self() const {
    return event_core_self_lock((*this));
}

void TimeoutEvent::set_self(rusty::sync::Weak<EventPollable> self_ptr) {
    event_core_set_self((*this), std::move(self_ptr));
}

bool TimeoutEvent::test() const {
    return event_test_impl((*this));
}

bool TimeoutEvent::is_ready() const {
    return timeout_event_is_ready((*this));
}

void TimeoutEvent::log() const {
}

EventStatus TimeoutEvent::status() const {
    return this->status_.get();
}

void TimeoutEvent::set_status(EventStatus s) const {
    this->status_.set(std::move(s));
}

uint64_t TimeoutEvent::wakeup_time() const {
    return event_core_wakeup_time((*this));
}

bool TimeoutEvent::prunable() const {
    return this->prunable_.get();
}

void TimeoutEvent::set_prunable(bool v) const {
    this->prunable_.set(std::move(v));
}

rusty::Option<rusty::Rc<Fiber>> TimeoutEvent::upgrade_fiber() const {
    return event_core_upgrade_fiber((*this));
}
/*RUSTYCPP:GEN-END id=reactor.timeout_event*/

// @unsafe - Time::now read; strict `>` preserved from the original.
inline bool timeout_event_is_ready(const TimeoutEvent& self) {
  return Time::now(true) > self.wakeup_time_;
}

// `WaitAny` — a composite event that is ready as soon as ANY of its child
// events is ready (polled in the reactor loop via `is_composite_event()`).
// FLATTENED (S4): an inline-Rust DSL struct deriving EventPollable via
// `#[cpp_inherit]` (its Arc<WaitAny> is upcast to Arc<EventPollable> at the
// create_sp_event site), carrying the five event-core fields inline plus the
// child-event vector, driven by the shared event_wait_impl / event_test_impl /
// event_core_* kernels. Construction (the former 2-arg ctor) is the
// `waitany_make` factory, wired into event_make. Unlike WaitAll it has no
// variadic ctor, so it converts cleanly; the any-ready predicate is a plain
// DSL loop over the child vector.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block.
#if RUSTYCPP_RUST
struct WaitAny {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
    events_: rusty::Vec<rusty::Arc<EventPollable>>,
}

impl WaitAny {
    fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    fn is_composite_event(&self) -> bool {
        true
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl EventPollable for WaitAny {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        for e in self.events_.iter() {
            if (*e).is_ready() {
                return true;
            }
        }
        false
    }
    fn log(&self) {}
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.wait_any version=1 rust_sha256=88cb434be8cb78f9f0b9c117c1c3ec9d374e8c7548a83171687bff129e8e713c*/
struct WaitAny;

struct WaitAny : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    rusty::Vec<rusty::Arc<EventPollable>> events_;
    WaitAny(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init, rusty::Vec<rusty::Arc<EventPollable>> events__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)), events_(std::move(events__init)) {}
    WaitAny(WaitAny&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)), events_(std::move(other.events_)) {}


    void wait() const;
    void wait_timeout(uint64_t timeout) const;
    bool is_composite_event() const;
    rusty::Option<rusty::Arc<EventPollable>> get_self() const;
    void set_self(rusty::sync::Weak<EventPollable> self_ptr);
    bool test() const;
    bool is_ready() const;
    void log() const;
    EventStatus status() const;
    void set_status(EventStatus s) const;
    uint64_t wakeup_time() const;
    bool prunable() const;
    void set_prunable(bool v) const;
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const;
};


void WaitAny::wait() const {
    event_wait_impl((*this), static_cast<uint64_t>(0));
}

void WaitAny::wait_timeout(uint64_t timeout) const {
    event_wait_impl((*this), std::move(timeout));
}

bool WaitAny::is_composite_event() const {
    return true;
}

rusty::Option<rusty::Arc<EventPollable>> WaitAny::get_self() const {
    return event_core_self_lock((*this));
}

void WaitAny::set_self(rusty::sync::Weak<EventPollable> self_ptr) {
    event_core_set_self((*this), std::move(self_ptr));
}

bool WaitAny::test() const {
    return event_test_impl((*this));
}

bool WaitAny::is_ready() const {
    for (auto&& e : rusty::for_in(rusty::iter(this->events_))) {
        if (((rusty::detail::deref_if_pointer_like(e))).is_ready()) {
            return true;
        }
    }
    return false;
}

void WaitAny::log() const {
}

EventStatus WaitAny::status() const {
    return this->status_.get();
}

void WaitAny::set_status(EventStatus s) const {
    this->status_.set(std::move(s));
}

uint64_t WaitAny::wakeup_time() const {
    return event_core_wakeup_time((*this));
}

bool WaitAny::prunable() const {
    return this->prunable_.get();
}

void WaitAny::set_prunable(bool v) const {
    this->prunable_.set(std::move(v));
}

rusty::Option<rusty::Rc<Fiber>> WaitAny::upgrade_fiber() const {
    return event_core_upgrade_fiber((*this));
}
/*RUSTYCPP:GEN-END id=reactor.wait_any*/

// `WaitAll` — composite event ready once ALL child events are ready (or DONE).
// FLATTENED (S4): an inline-Rust DSL struct deriving EventPollable via
// `#[cpp_inherit]`. Like WaitAny it carries the event-core fields + a child
// vector, but its vector is a RefCell (add_event mutates it after construction).
// The variadic ctor + variadic add_event(Args...) the DSL cannot express are
// dropped: construction goes through the `waitall_make` (empty) /
// `waitall_make_from` (vector) factories wired into event_make, and add_event is
// single-arg (every call site already passes one event). The one test that used
// the 3-arg variadic ctor now builds a vector.
//
// add_event is fully DSL now. The push once lived in a hand-written kernel
// because `.push()` chained through a RefCell guard mis-lowers (wrapping the
// element in Vec::from_iter) — but that is an IDIOM problem, not a transpiler
// limitation: binding the guard first and dereferencing (`let mut g = ...;
// (*g).push(x)`) lowers correctly. See §7.33.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block.
struct WaitAll;
#if RUSTYCPP_RUST
struct WaitAll {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
    events_: RefCell<rusty::Vec<rusty::Arc<EventPollable>>>,
}

impl WaitAll {
    fn add_event(&self, x: rusty::Arc<EventPollable>) {
        // Bind the guard, then deref — chaining `.borrow_mut().push(x)`
        // mis-lowers to push(Vec::from_iter(x)). See §7.33.
        let mut g = self.events_.borrow_mut();
        (*g).push(x);
    }
    fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    fn is_composite_event(&self) -> bool {
        true
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl EventPollable for WaitAll {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        for e in self.events_.borrow().iter() {
            if !((*e).is_ready() || (*e).status() == EventStatus::DONE) {
                return false;
            }
        }
        true
    }
    fn log(&self) {
        for e in self.events_.borrow().iter() {
            (*e).log();
        }
    }
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.wait_all version=1 rust_sha256=e69198fa813b48c276f6aae16f1e138c0c9c3038b832c35b3dec16154fc8786a*/
struct WaitAll;

struct WaitAll : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    rusty::RefCell<rusty::Vec<rusty::Arc<EventPollable>>> events_;
    WaitAll(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init, rusty::RefCell<rusty::Vec<rusty::Arc<EventPollable>>> events__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)), events_(std::move(events__init)) {}
    WaitAll(WaitAll&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)), events_(std::move(other.events_)) {}


    void add_event(rusty::Arc<EventPollable> x) const;
    void wait() const;
    void wait_timeout(uint64_t timeout) const;
    bool is_composite_event() const;
    rusty::Option<rusty::Arc<EventPollable>> get_self() const;
    void set_self(rusty::sync::Weak<EventPollable> self_ptr);
    bool test() const;
    bool is_ready() const;
    void log() const;
    EventStatus status() const;
    void set_status(EventStatus s) const;
    uint64_t wakeup_time() const;
    bool prunable() const;
    void set_prunable(bool v) const;
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const;
};


void WaitAll::add_event(rusty::Arc<EventPollable> x) const {
    auto g = this->events_.borrow_mut();
    ((*g)).push(std::move(x));
}

void WaitAll::wait() const {
    event_wait_impl((*this), static_cast<uint64_t>(0));
}

void WaitAll::wait_timeout(uint64_t timeout) const {
    event_wait_impl((*this), std::move(timeout));
}

bool WaitAll::is_composite_event() const {
    return true;
}

rusty::Option<rusty::Arc<EventPollable>> WaitAll::get_self() const {
    return event_core_self_lock((*this));
}

void WaitAll::set_self(rusty::sync::Weak<EventPollable> self_ptr) {
    event_core_set_self((*this), std::move(self_ptr));
}

bool WaitAll::test() const {
    return event_test_impl((*this));
}

bool WaitAll::is_ready() const {
    for (auto&& e : rusty::for_in(rusty::iter(this->events_.borrow()))) {
        if (!(((rusty::detail::deref_if_pointer_like(e))).is_ready() || (((rusty::detail::deref_if_pointer_like(e))).status() == rusty::clone(EventStatus::DONE)))) {
            return false;
        }
    }
    return true;
}

void WaitAll::log() const {
    for (auto&& e : rusty::for_in(rusty::iter(this->events_.borrow()))) {
        ((rusty::detail::deref_if_pointer_like(e))).log();
    }
}

EventStatus WaitAll::status() const {
    return this->status_.get();
}

void WaitAll::set_status(EventStatus s) const {
    this->status_.set(std::move(s));
}

uint64_t WaitAll::wakeup_time() const {
    return event_core_wakeup_time((*this));
}

bool WaitAll::prunable() const {
    return this->prunable_.get();
}

void WaitAll::set_prunable(bool v) const {
    this->prunable_.set(std::move(v));
}

rusty::Option<rusty::Rc<Fiber>> WaitAll::upgrade_fiber() const {
    return event_core_upgrade_fiber((*this));
}
/*RUSTYCPP:GEN-END id=reactor.wait_all*/

// --- from fiber_impl.h ---------------------------------------------------

// Forward declaration
class Fiber;

/**
 * CPU context for x86_64 SysV user-space context switching.
 *
 * Stores callee-saved registers plus stack/instruction pointer.
 */
struct FiberContext {
#if defined(__x86_64__)
  void* rsp{nullptr};
  void* rip{nullptr};
  std::uintptr_t rbx{0};
  std::uintptr_t rbp{0};
  std::uintptr_t r12{0};
  std::uintptr_t r13{0};
  std::uintptr_t r14{0};
  std::uintptr_t r15{0};
#elif defined(__aarch64__)
  // AAPCS64 callee-saved: x19-x28, x29 (fp), x30 (lr used as pc), sp.
  void* sp{nullptr};          // offset  0
  void* pc{nullptr};          // offset  8 (lr on entry = resume address)
  std::uintptr_t x19{0};      // offset 16
  std::uintptr_t x20{0};      // offset 24
  std::uintptr_t x21{0};      // offset 32
  std::uintptr_t x22{0};      // offset 40
  std::uintptr_t x23{0};      // offset 48
  std::uintptr_t x24{0};      // offset 56
  std::uintptr_t x25{0};      // offset 64
  std::uintptr_t x26{0};      // offset 72
  std::uintptr_t x27{0};      // offset 80
  std::uintptr_t x28{0};      // offset 88
  std::uintptr_t fp{0};       // offset 96 (x29)
#endif
};

extern "C" void fiber_swap_context(FiberContext* from, FiberContext* to);

// Default stack size for stackless fibers (1 MiB). Lifted out of
// `fiber_task_t` class scope (was `private static constexpr`) because
// DSL constants live at namespace scope. The one use site
// (`fiber_task_t::init_context`) references it unqualified, so
// namespace lookup still resolves to this constant.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
const kDefaultStackBytes: usize = 1usize << 20;
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_default_stack version=1 rust_sha256=573a148f9a126f68ff3cd154018259cab614444f2c74a62837b70635855b9e68*/
constexpr size_t kDefaultStackBytes = static_cast<size_t>(1) << 20;
/*RUSTYCPP:GEN-END id=reactor.fiber_default_stack*/

class fiber_task_t;

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// `fiber_yield_t` is now a pure-POD pointer wrapper. The previous
// member method `void operator()()` (renamed to `void yield_now()` in
// an earlier DSL-prep commit) is now the free function
// `fiber_yield_invoke(fiber_yield_t&)`. (It was kept outside the DSL
// block for a while because the body raw-dereferences `task_`; that
// transpiler limitation is gone and the fn is DSL now.) The two
// call sites (`yield()` in `Fiber::run_wrapper`, `(*yield_ptr)()` in
// `Fiber::yield_`) now use `fiber_yield_invoke(yield)` /
// `fiber_yield_invoke(*yield_ptr)`. The DSL `fn new(task)` factory
// lowers to the `static new_()` the one in-tree member init in
// `fiber_task_t::fiber_task_t` already calls.
#if RUSTYCPP_RUST
struct fiber_yield_t {
    task_: *mut fiber_task_t,
}

impl fiber_yield_t {
    fn new(task: &mut fiber_task_t) -> fiber_yield_t {
        fiber_yield_t {
            task_: task as *mut fiber_task_t,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_yield version=1 rust_sha256=ab6ef3f623333af344b247aab5325e40a15188044251222c9884ef258de66b5d*/
struct fiber_yield_t;

struct fiber_yield_t {
    fiber_task_t* task_;

    static fiber_yield_t new_(fiber_task_t& task);
};


fiber_yield_t fiber_yield_t::new_(fiber_task_t& task) {
    return fiber_yield_t{.task_ = static_cast<fiber_task_t*>(rusty::detail::ptr_or_addr(task))};
}
/*RUSTYCPP:GEN-END id=reactor.fiber_yield*/

// @unsafe { raw fiber_task_t* deref + private yield_to_caller() call;
// the friend declaration on fiber_task_t still applies. } Free
// function, authored as DSL further down (§7.30).
void fiber_yield_invoke(fiber_yield_t& self);

class fiber_task_t {
 public:
  using TaskFn = rusty::Function<void(fiber_yield_t&)>;

  explicit fiber_task_t(TaskFn fn);

  template <typename Fn,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, fiber_task_t>>>
  explicit fiber_task_t(Fn&& fn)
      : fiber_task_t(TaskFn(std::forward<Fn>(fn))) {}

  ~fiber_task_t();

  fiber_task_t(const fiber_task_t&) = delete;
  fiber_task_t& operator=(const fiber_task_t&) = delete;
  fiber_task_t(fiber_task_t&&) = delete;
  fiber_task_t& operator=(fiber_task_t&&) = delete;

  void operator()();

 private:
  friend class fiber_yield_t;
  friend void fiber_yield_invoke(fiber_yield_t& self);

  enum class State : uint8_t {
    NEW = 0,
    RUNNING,
    SUSPENDED,
    FINISHED
  };

  static thread_local fiber_task_t* tls_active_task_;

  static void entry_trampoline();
  [[noreturn]] void entry();

  void init_context();
  void resume();
  void yield_to_caller();

  TaskFn fn_;
  fiber_yield_t yield_;
  FiberContext caller_ctx_{};
  FiberContext fiber_ctx_{};
  void* stack_mapping_{nullptr};
  std::size_t stack_mapping_bytes_{0};
  State state_{State::NEW};
};

class Reactor;

/**
 * Fiber - A stackful fiber (execution context).
 *
 * Fibers provide cooperative multitasking within a single thread.
 * They are scheduled by the Reactor and can yield/resume execution.
 *
 * Key differences from C++20 coroutines:
 *   - C++20 coroutines are stackless (state machines)
 *   - Fibers use custom stackful execution
 *   - Stackful contexts are properly called "fibers"
 *
 * QUARANTINE — the stackful-fiber context-switch primitive lives in
 * `fiber_context_{x86_64,aarch64}.cc` as raw assembly, and is invoked
 * through `fiber_task_t::resume()`/`yield_to_caller()`/`entry()` in
 * the impl section of this file. Those callers carry `// @unsafe`
 * annotations.
 *
 * Public API on Fiber (`run`, `yield_`, `continue_`, `create_run`,
 * `current_fiber`, `sleep`, ctor/dtor/finished/do_finalize) is `@safe`
 * — callers can use it from @safe code. Each method's body wraps its
 * genuinely-unsafe internals (Rc/RefCell unwrap, fiber-runtime dispatch,
 * std::bind / function-pointer construction) in inline `@unsafe { ... }`
 * blocks. Two implementation-detail methods stay `@unsafe`:
 *   - `run_wrapper(yield)`: invoked from the asm trampoline; the
 *     contract is fixed.
 *   - `create_run_impl(...)`: builds the heap-allocated task via raw
 *     `new chunk` shapes the analyzer can't yet see through.
 */
// @safe
class Fiber {
 public:
  /**
   * Get the currently executing fiber.
   *
   * @return Some(fiber) if in fiber context, None otherwise
   */
  static rusty::Option<rusty::Rc<Fiber>> current_fiber();

  /**
   * Create and run a new fiber with the given function.
   *
   * @param func The function to execute in the fiber
   * @param file Source file (for debugging)
   * @param line Source line (for debugging)
   * @return Rc<Fiber> handle to the created fiber
   *
   * @safe - Wraps callable and delegates to create_run_impl. Memory-safe:
   *   - rusty::Function safely captures the callable
   *   - Returns rusty::Rc for safe reference counting
   *   - Internal fiber state is managed by Reactor
   */
  template <typename Func>
  static rusty::Rc<Fiber> create_run(Func&& func, const char* file = "", int64_t line = 0) {
    // @unsafe - create_run_impl uses raw pointer operations internally
    { return create_run_impl(rusty::Function<void()>(std::forward<Func>(func)), file, line); }
  }

  /**
   * Sleep the current fiber for the specified duration.
   *
   * @param microseconds Duration to sleep in microseconds
   */
  static void sleep(uint64_t microseconds);

  static thread_local uint64_t global_id;
  uint64_t dep_id_{0};
  bool need_finalize_{false};
  // Cell: the id is stamped once on a Fiber reached through a shared
  // handle, so interior mutability replaces the const_cast that used to
  // do it (the old comment there said "id is not Cell yet").
  rusty::Cell<uint64_t> id{0};

  enum Status { INIT = 0, STARTED, PAUSED, RESUMED, FINISHED, FINALIZING, RECYCLED };

  // Interior mutability using Cell/RefCell for use with rusty::Rc
  // Cell<T> for Copy types, RefCell<T> for non-Copy types
  rusty::Cell<Status> status_{INIT};
  rusty::Cell<bool> needs_finalize_{false};
  rusty::RefCell<rusty::Function<void()>> func_{};

  // Uses rusty::Box with Option for nullable semantics
  rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>> fiber_task_{};
  // Non-owning pointer to the yield handle owned by fiber_task_t.
  // Cell provides interior mutability for const yield_().
  rusty::Cell<fiber_yield_t*> fiber_yield_{nullptr};

  Fiber() = delete;
  explicit Fiber(rusty::Function<void()> func);
  ~Fiber();

  // @unsafe - Uses std::bind and function pointers
  void run_wrapper(fiber_yield_t& yield);

  /**
   * Initialize and start the fiber.
   * @safe - Uses Cell/RefCell for interior mutability, Box for ownership.
   */
  void run() const;

  /**
   * Yield control back to the reactor.
   * @safe - Uses non-owning yield pointer and Cell for status.
   */
  void yield_() const;

  /**
   * Resume a paused fiber.
   * @safe - Uses RefCell for fiber_task_ access.
   */
  void continue_() const;

  bool finished() const;
  void do_finalize();

  // Comparison operator for rusty::BTreeSet<rusty::Rc<Fiber>>
  friend bool operator<(const rusty::Rc<Fiber>& lhs, const rusty::Rc<Fiber>& rhs) {
    return lhs.get() < rhs.get();
  }

 private:
  // @unsafe - Creates and runs a new fiber (uses raw pointer operations)
  static rusty::Rc<Fiber> create_run_impl(rusty::Function<void()> func, const char* file, int64_t line);
};


// --- from reactor.h (block 1: Reactor, PollCommand) ----------------------

// Note: Fiber is the primary class (defined in fiber_impl.h)
// The full definition is available via #include "fiber_impl.h" above

/**
 * @class Reactor
 * @brief Thread-local event loop and fiber scheduler
 *
 * MEMORY SAFETY MODEL:
 *
 * 1. THREAD AFFINITY
 *    - Each Reactor is pinned to its creating thread via thread_id_
 *    - Loop() verifies thread ownership at entry
 *    - Thread-local storage (sp_reactor_th_) prevents cross-thread access
 *
 * 2. INTERIOR MUTABILITY (RustyCpp Patterns)
 *    - Cell<T>: Used for primitive counters and flags (looping_, thread_id_, etc.)
 *    - RefCell<T>: Used for complex types (sp_running_fiber_th_)
 *    - mutable containers: STL containers with const method access
 *
 * 3. SMART POINTER USAGE
 *    - Rc<Fiber>: Single-threaded reference counting for fibers
 *    - shared_ptr<Event>: For polymorphic event types
 *    - Weak<Fiber>: Events hold weak refs to avoid cycles
 *
 * 4. SYNCHRONIZATION
 *    - All reactor operations are single-threaded (no locks needed)
 *
 * SAFETY INVARIANTS:
 * - One active fiber per reactor at any time
 * - Events never outlive their fibers (weak refs)
 * - Loop() only called from owning thread
 */
// @safe - Single-threaded reactor; data lives in RefCell / Cell / Rc /
// HashMap with rusty borrow rules. Methods that genuinely cross into
// unsafe territory (fiber context switching via Fiber::yield_ /
// continue_, raw pointer access through the class-static thread_local
// fields, get_reactor returning thread-local Rc) carry their own
// `// @unsafe` overrides; the rest is now analyzed as @safe by default.
// One slot in the reactor's stackless-task table. Hoisted out of
// `class Reactor` (Rust does not allow item declarations inside an
// `impl`, so a nested struct could never convert in place -- same move
// server.cpp made for ShutdownState). The `= false` field defaults are
// gone (Rust has no default field initializers); the one construction
// site aggregate-initialises instead.
#if RUSTYCPP_RUST
struct StacklessTaskEntry {
    active: bool,
    queued: bool,
    poll_once: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.14 version=1 rust_sha256=ade01046467a0dbcee0cfcc44953857b6c2ec4348e591ad6961d68a9c00eacbf*/
struct StacklessTaskEntry;

struct StacklessTaskEntry {
    bool active;
    bool queued;
    rusty::Function<bool(rusty::Context&)> poll_once;
};
/*RUSTYCPP:GEN-END id=reactor.14*/

// Reactor -- converted to inline-Rust DSL (Goal 0 Stage B). The struct is
// deliberately NON-movable and NON-copyable: it is thread-affine
// (thread_id_ is verified inside the loop) and held through Rc<Reactor>,
// so `_pin: PhantomPinned` makes the transpiler emit deleted move
// operations, and `#[cpp_ctor] fn new()` emits a real in-place default
// constructor so `Rc<Reactor>::make()` keeps working. Method bodies with
// real logic live in the `reactor_*_impl` kernels below the class; the
// DSL methods delegate. `loop` was renamed `run_loop` (Rust keyword) and
// its two default arguments became explicit at the ~43 call sites;
// `create_run_fiber` lost its never-used file/line default parameters.
#if defined(REUSE_FIBER) || defined(REUSE_CORO)
#define REUSING_FIBER (true)
#else
#define REUSING_FIBER (false)
#endif

// Hoisted class statics (a DSL struct holds no statics -- same move as
// sp_reactor_th_ / g_current_poll_worker). `reactor_clients_th_` is the
// old `Reactor::clients_`, still consumed by deptran/communicator.cc;
// `dangling_ips_` was dead and is deleted.
inline thread_local rusty::HashMap<std::string, rusty::Vec<PollableProxy>> reactor_clients_th_{};

// Stackless-profile observability shim, defined next to g_stackless_profile
// further down (the DSL method cannot name the later-defined global).
void stackless_profile_note_enqueue();

// The thread-local singleton/running-fiber accessors, defined (in DSL)
// later in this file; the GEN method bodies below call them.
rusty::Rc<Reactor> reactor_tls_get();
rusty::Rc<Reactor> reactor_tls_get_disk();
rusty::Option<rusty::Rc<Fiber>> reactor_tls_save_running();
void reactor_tls_restore_running(rusty::Option<rusty::Rc<Fiber>> old_fiber);
void reactor_tls_set_running(const rusty::Rc<Fiber>& fiber);

// @unsafe kernels the DSL methods (and each other) delegate to; bodies
// below the class, renamed from the old member definitions.
rusty::Rc<Fiber> reactor_get_or_create_fiber_impl(const Reactor& self, rusty::Function<void()> func, const char* file, int64_t line);
size_t reactor_register_stackless_poller_impl(const Reactor& self, rusty::Function<bool(rusty::Context&)> poller);
bool reactor_process_stackless_tasks_impl(const Reactor& self);
void reactor_prune_finished_events_impl(const Reactor& self);
void reactor_run_loop_impl(const Reactor& self, bool infinite, bool do_check_timeout);
void reactor_spawn_stackless_task_impl(const Reactor& self, rusty::Task<void> task);
rusty::Rc<Fiber> reactor_create_run_fiber_impl(const Reactor& self, rusty::Function<void()> func);
rusty::Rc<Fiber> reactor_create_run_fiber_at_impl(const Reactor& self, rusty::Function<void()> func, const char* file, int64_t line);

#if RUSTYCPP_RUST
struct Reactor {
    server_id_: rusty::Cell<i32>,
    all_events_: rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>,
    waiting_events_: rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>,
    timeout_events_: rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>,
    composite_events_: rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>,
    fibers_: rusty::RefCell<std::set<rusty::Rc<Fiber>>>,
    available_fibers_: rusty::RefCell<rusty::Vec<rusty::Rc<Fiber>>>,
    looping_: rusty::Cell<bool>,
    slow_: rusty::Cell<bool>,
    slow_count_: rusty::Cell<i32>,
    trying_count_: rusty::Cell<i32>,
    thread_id_: rusty::Cell<rusty::thread::ThreadId>,
    n_created_fibers_: rusty::Cell<i64>,
    n_busy_fibers_: rusty::Cell<i64>,
    n_active_fibers_: rusty::Cell<i64>,
    n_active_fibers_2_: rusty::Cell<i64>,
    n_idle_fibers_: rusty::Cell<i64>,
    stackless_tasks_: rusty::RefCell<rusty::Vec<StacklessTaskEntry>>,
    free_stackless_task_slots_: rusty::RefCell<rusty::Vec<usize>>,
    ready_stackless_tasks_: rusty::RefCell<rusty::VecDeque<usize>>,
    _pin: rusty::marker::PhantomPinned,
}

impl Reactor {
    #[cpp_ctor]
    fn new() -> Reactor {
        Reactor {
            server_id_: Default::default(),
            all_events_: Default::default(),
            waiting_events_: Default::default(),
            timeout_events_: Default::default(),
            composite_events_: Default::default(),
            fibers_: Default::default(),
            available_fibers_: Default::default(),
            looping_: Default::default(),
            slow_: Default::default(),
            slow_count_: Default::default(),
            trying_count_: Default::default(),
            thread_id_: Default::default(),
            n_created_fibers_: Default::default(),
            n_busy_fibers_: Default::default(),
            n_active_fibers_: Default::default(),
            n_active_fibers_2_: Default::default(),
            n_idle_fibers_: Default::default(),
            stackless_tasks_: Default::default(),
            free_stackless_task_slots_: Default::default(),
            ready_stackless_tasks_: Default::default(),
            _pin: rusty::marker::PhantomPinned {},
        }
    }

    fn get_reactor() -> rusty::Rc<Reactor> {
        reactor_tls_get()
    }
    fn get_disk_reactor() -> rusty::Rc<Reactor> {
        reactor_tls_get_disk()
    }
    fn save_running_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        reactor_tls_save_running()
    }
    fn restore_running_fiber(&self, old_fiber: rusty::Option<rusty::Rc<Fiber>>) {
        reactor_tls_restore_running(old_fiber);
    }
    fn set_running_fiber(&self, fiber: &rusty::Rc<Fiber>) {
        reactor_tls_set_running(fiber);
    }
    fn run_loop(&self, infinite: bool, do_check_timeout: bool) {
        reactor_run_loop_impl(self, infinite, do_check_timeout);
    }
    fn create_run_fiber(&self, func: rusty::Function<dyn FnMut()>) -> rusty::Rc<Fiber> {
        reactor_create_run_fiber_impl(self, func)
    }
    fn continue_fiber(&self, fiber: &rusty::Rc<Fiber>) {
        // Save current running fiber for nesting support.
        let mut old_fiber: rusty::Option<rusty::Rc<Fiber>> = None;
        {
            let guard = sp_running_fiber_th_.borrow();
            if (*guard).is_some() {
                old_fiber = rusty::Some((*guard).as_ref().unwrap().clone());
            }
        }
        {
            let mut guard = sp_running_fiber_th_.borrow_mut();
            *guard = rusty::Some(fiber.clone());
        }
        {
            let guard = sp_running_fiber_th_.borrow();
            let running: &rusty::Rc<Fiber> = (*guard).as_ref().unwrap();
            verify(!running.finished());
        }
        self.n_active_fibers_.set(self.n_active_fibers_.get() + 1i64);
        if fiber.status_.get() == Fiber::INIT {
            fiber.run();
        } else {
            // Don't hold a borrow across continue_(): the fiber may call
            // create_run() (RefCell double-borrow crash during restart).
            fiber.continue_();
        }
        {
            let guard = sp_running_fiber_th_.borrow();
            let running: &rusty::Rc<Fiber> = (*guard).as_ref().unwrap();
            if running.finished() {
                let mut fiber_ref = running.clone();
                self.recycle(&mut fiber_ref);
            }
        }
        {
            let mut guard = sp_running_fiber_th_.borrow_mut();
            *guard = old_fiber;
        }
    }

    fn display_waiting_ev(&self) {
        Log_info("waiting_events_: {}, composite_events_: {}",
                 self.waiting_events_.borrow().len(), self.composite_events_.borrow().len());
    }

    fn register_fiber(&self, fiber: &rusty::Rc<Fiber>) {
        // std::set::insert returns pair<iterator, bool>; `.second` is true
        // when the value was newly inserted.
        let mut guard = self.fibers_.borrow_mut();
        let inserted = guard.insert(fiber.clone()).second;
        if !inserted {
            unsafe { Log_error("[DEBUG] RegisterFiber: Failed to insert fiber into fibers_ set!"); }
            unsafe { Log_error("[DEBUG] fibers_ size: {}, REUSING_FIBER: {}", guard.size(), REUSING_FIBER); }
        }
        verify(inserted);
        verify(guard.size() > 0usize);
    }

    fn recycle(&self, fiber: &mut rusty::Rc<Fiber>) {
        // Fixes fibers not being recycled when they don't finish immediately.
        if REUSING_FIBER {
            fiber.status_.set(Fiber::RECYCLED);
            let empty_fn: rusty::Function<dyn FnMut()> = Default::default();
            *fiber.func_.borrow_mut() = empty_fn;
            self.n_idle_fibers_.set(self.n_idle_fibers_.get() + 1i64);
            self.available_fibers_.borrow_mut().push(fiber.clone());
        }
        self.n_busy_fibers_.set(self.n_busy_fibers_.get() - 1i64);
        self.fibers_.borrow_mut().erase(fiber);
    }

    fn enqueue_stackless_task(&self, idx: usize) {
        stackless_profile_note_enqueue();
        {
            let guard = self.stackless_tasks_.borrow();
            if idx >= guard.len() {
                return;
            }
            if !(*guard)[idx].active || (*guard)[idx].queued {
                return;
            }
        }
        {
            let mut guard = self.stackless_tasks_.borrow_mut();
            if idx >= guard.len() {
                return;
            }
            if !(*guard)[idx].active || (*guard)[idx].queued {
                return;
            }
            (*guard)[idx].queued = true;
        }
        self.ready_stackless_tasks_.borrow_mut().push_back(idx);
    }

    fn check_timeout(&self, ready_events: &mut rusty::VecDeque<rusty::Arc<EventPollable>>) {
        let time_now: i64 = Time::now(true);
        let mut guard = self.timeout_events_.borrow_mut();
        // First pass: update the status of timed-out events. The Arc is
        // cloned per slot so no reference into the guard is held across
        // the status mutations.
        let mut i: usize = 0usize;
        while i < guard.len() {
            let event = (*guard)[i].clone();
            if (*event).status() == EventStatus::WAIT {
                let wakeup_time = (*event).wakeup_time();
                verify(wakeup_time > 0u64);
                if time_now >= wakeup_time as i64 {
                    if (*event).is_ready() {
                        (*event).set_status(EventStatus::READY);
                    } else {
                        (*event).set_status(EventStatus::TIMEOUT);
                    }
                }
            }
            i += 1usize;
        }
        // Extract events that are READY or TIMEOUT.
        ready_events.append(guard.extract_if(move |sp: &rusty::Arc<EventPollable>| -> bool {
            let status = (*sp).status();
            status == EventStatus::READY || status == EventStatus::TIMEOUT
        }));
        // Drop events that are DONE.
        guard.retain(move |sp: &rusty::Arc<EventPollable>| -> bool {
            (*sp).status() != EventStatus::DONE
        });
    }
}

impl Drop for Reactor {
    fn drop(&mut self) {
        Log_debug("[Reactor::~Reactor] Starting destruction, all_events_.len()={}, fibers_.size()={}",
                  self.all_events_.borrow().len(), self.fibers_.borrow().size());
        Log_debug("[Reactor::~Reactor] Destructor body complete, about to destroy member variables");
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.15 version=1 rust_sha256=1577ad15ab517ad9c306b57da141231db86f2cf4460f6de7c3dda5824b1f3997*/
struct Reactor;

struct Reactor {
    rusty::Cell<int32_t> server_id_;
    rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> all_events_;
    rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> waiting_events_;
    rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> timeout_events_;
    rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> composite_events_;
    rusty::RefCell<std::set<rusty::Rc<Fiber>>> fibers_;
    rusty::RefCell<rusty::Vec<rusty::Rc<Fiber>>> available_fibers_;
    rusty::Cell<bool> looping_;
    rusty::Cell<bool> slow_;
    rusty::Cell<int32_t> slow_count_;
    rusty::Cell<int32_t> trying_count_;
    rusty::Cell<rusty::thread::ThreadId> thread_id_;
    rusty::Cell<int64_t> n_created_fibers_;
    rusty::Cell<int64_t> n_busy_fibers_;
    rusty::Cell<int64_t> n_active_fibers_;
    rusty::Cell<int64_t> n_active_fibers_2_;
    rusty::Cell<int64_t> n_idle_fibers_;
    rusty::RefCell<rusty::Vec<StacklessTaskEntry>> stackless_tasks_;
    rusty::RefCell<rusty::Vec<size_t>> free_stackless_task_slots_;
    rusty::RefCell<rusty::VecDeque<size_t>> ready_stackless_tasks_;
    rusty::marker::PhantomPinned _pin;
    mutable bool _rusty_forgotten = false;
    Reactor(rusty::Cell<int32_t> server_id__init, rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> all_events__init, rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> waiting_events__init, rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> timeout_events__init, rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> composite_events__init, rusty::RefCell<std::set<rusty::Rc<Fiber>>> fibers__init, rusty::RefCell<rusty::Vec<rusty::Rc<Fiber>>> available_fibers__init, rusty::Cell<bool> looping__init, rusty::Cell<bool> slow__init, rusty::Cell<int32_t> slow_count__init, rusty::Cell<int32_t> trying_count__init, rusty::Cell<rusty::thread::ThreadId> thread_id__init, rusty::Cell<int64_t> n_created_fibers__init, rusty::Cell<int64_t> n_busy_fibers__init, rusty::Cell<int64_t> n_active_fibers__init, rusty::Cell<int64_t> n_active_fibers_2__init, rusty::Cell<int64_t> n_idle_fibers__init, rusty::RefCell<rusty::Vec<StacklessTaskEntry>> stackless_tasks__init, rusty::RefCell<rusty::Vec<size_t>> free_stackless_task_slots__init, rusty::RefCell<rusty::VecDeque<size_t>> ready_stackless_tasks__init, rusty::marker::PhantomPinned _pin_init) : server_id_(std::move(server_id__init)), all_events_(std::move(all_events__init)), waiting_events_(std::move(waiting_events__init)), timeout_events_(std::move(timeout_events__init)), composite_events_(std::move(composite_events__init)), fibers_(std::move(fibers__init)), available_fibers_(std::move(available_fibers__init)), looping_(std::move(looping__init)), slow_(std::move(slow__init)), slow_count_(std::move(slow_count__init)), trying_count_(std::move(trying_count__init)), thread_id_(std::move(thread_id__init)), n_created_fibers_(std::move(n_created_fibers__init)), n_busy_fibers_(std::move(n_busy_fibers__init)), n_active_fibers_(std::move(n_active_fibers__init)), n_active_fibers_2_(std::move(n_active_fibers_2__init)), n_idle_fibers_(std::move(n_idle_fibers__init)), stackless_tasks_(std::move(stackless_tasks__init)), free_stackless_task_slots_(std::move(free_stackless_task_slots__init)), ready_stackless_tasks_(std::move(ready_stackless_tasks__init)), _pin(std::move(_pin_init)) {}
    Reactor(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor& operator=(Reactor&&) = delete;
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->server_id_); rusty::detail::mark_forgotten_if_supported(this->all_events_); rusty::detail::mark_forgotten_if_supported(this->waiting_events_); rusty::detail::mark_forgotten_if_supported(this->timeout_events_); rusty::detail::mark_forgotten_if_supported(this->composite_events_); rusty::detail::mark_forgotten_if_supported(this->fibers_); rusty::detail::mark_forgotten_if_supported(this->available_fibers_); rusty::detail::mark_forgotten_if_supported(this->looping_); rusty::detail::mark_forgotten_if_supported(this->slow_); rusty::detail::mark_forgotten_if_supported(this->slow_count_); rusty::detail::mark_forgotten_if_supported(this->trying_count_); rusty::detail::mark_forgotten_if_supported(this->thread_id_); rusty::detail::mark_forgotten_if_supported(this->n_created_fibers_); rusty::detail::mark_forgotten_if_supported(this->n_busy_fibers_); rusty::detail::mark_forgotten_if_supported(this->n_active_fibers_); rusty::detail::mark_forgotten_if_supported(this->n_active_fibers_2_); rusty::detail::mark_forgotten_if_supported(this->n_idle_fibers_); rusty::detail::mark_forgotten_if_supported(this->stackless_tasks_); rusty::detail::mark_forgotten_if_supported(this->free_stackless_task_slots_); rusty::detail::mark_forgotten_if_supported(this->ready_stackless_tasks_); rusty::detail::mark_forgotten_if_supported(this->_pin); }


    Reactor();
    static rusty::Rc<Reactor> get_reactor();
    static rusty::Rc<Reactor> get_disk_reactor();
    rusty::Option<rusty::Rc<Fiber>> save_running_fiber() const;
    void restore_running_fiber(rusty::Option<rusty::Rc<Fiber>> old_fiber) const;
    void set_running_fiber(const rusty::Rc<Fiber>& fiber) const;
    void run_loop(bool infinite, bool do_check_timeout) const;
    rusty::Rc<Fiber> create_run_fiber(rusty::Function<void()> func) const;
    void continue_fiber(const rusty::Rc<Fiber>& fiber) const;
    void display_waiting_ev() const;
    void register_fiber(const rusty::Rc<Fiber>& fiber) const;
    void recycle(rusty::Rc<Fiber>& fiber) const;
    void enqueue_stackless_task(size_t idx) const;
    void check_timeout(rusty::VecDeque<rusty::Arc<EventPollable>>& ready_events) const;
    ~Reactor() noexcept(false);
};


Reactor::Reactor()
    : server_id_(rusty::default_like<rusty::Cell<int32_t>>())
    , all_events_(rusty::default_like<rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>>())
    , waiting_events_(rusty::default_like<rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>>())
    , timeout_events_(rusty::default_like<rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>>())
    , composite_events_(rusty::default_like<rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>>>())
    , fibers_(rusty::default_like<rusty::RefCell<std::set<rusty::Rc<Fiber>>>>())
    , available_fibers_(rusty::default_like<rusty::RefCell<rusty::Vec<rusty::Rc<Fiber>>>>())
    , looping_(rusty::default_like<rusty::Cell<bool>>())
    , slow_(rusty::default_like<rusty::Cell<bool>>())
    , slow_count_(rusty::default_like<rusty::Cell<int32_t>>())
    , trying_count_(rusty::default_like<rusty::Cell<int32_t>>())
    , thread_id_(rusty::default_like<rusty::Cell<rusty::thread::ThreadId>>())
    , n_created_fibers_(rusty::default_like<rusty::Cell<int64_t>>())
    , n_busy_fibers_(rusty::default_like<rusty::Cell<int64_t>>())
    , n_active_fibers_(rusty::default_like<rusty::Cell<int64_t>>())
    , n_active_fibers_2_(rusty::default_like<rusty::Cell<int64_t>>())
    , n_idle_fibers_(rusty::default_like<rusty::Cell<int64_t>>())
    , stackless_tasks_(rusty::default_like<rusty::RefCell<rusty::Vec<StacklessTaskEntry>>>())
    , free_stackless_task_slots_(rusty::default_like<rusty::RefCell<rusty::Vec<size_t>>>())
    , ready_stackless_tasks_(rusty::default_like<rusty::RefCell<rusty::VecDeque<size_t>>>())
    , _pin(rusty::marker::PhantomPinned{})
{}

rusty::Rc<Reactor> Reactor::get_reactor() {
    return reactor_tls_get();
}

rusty::Rc<Reactor> Reactor::get_disk_reactor() {
    return reactor_tls_get_disk();
}

rusty::Option<rusty::Rc<Fiber>> Reactor::save_running_fiber() const {
    return reactor_tls_save_running();
}

void Reactor::restore_running_fiber(rusty::Option<rusty::Rc<Fiber>> old_fiber) const {
    reactor_tls_restore_running(std::move(old_fiber));
}

void Reactor::set_running_fiber(const rusty::Rc<Fiber>& fiber) const {
    reactor_tls_set_running(fiber);
}

void Reactor::run_loop(bool infinite, bool do_check_timeout) const {
    reactor_run_loop_impl((*this), std::move(infinite), std::move(do_check_timeout));
}

rusty::Rc<Fiber> Reactor::create_run_fiber(rusty::Function<void()> func) const {
    return reactor_create_run_fiber_impl((*this), std::move(func));
}

void Reactor::continue_fiber(const rusty::Rc<Fiber>& fiber) const {
    rusty::Option<rusty::Rc<Fiber>> old_fiber = rusty::Option<rusty::Rc<Fiber>>{rusty::None};
    {
        auto&& guard = rusty::borrow(sp_running_fiber_th_);
        if (((rusty::detail::deref_if_pointer_like(guard))).is_some()) {
            old_fiber = rusty::Option<rusty::Rc<Fiber>>(rusty::clone(((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap()));
        }
    }
    {
        auto&& guard = sp_running_fiber_th_.borrow_mut();
        rusty::detail::deref_if_pointer_like(guard) = rusty::Option<rusty::Rc<Fiber>>(rusty::clone(fiber));
    }
    {
        auto&& guard = rusty::borrow(sp_running_fiber_th_);
        const rusty::Rc<Fiber>& running = ((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap();
        verify(rusty::detail::rust_not(running->finished()));
    }
    this->n_active_fibers_.set(this->n_active_fibers_.get() + static_cast<int64_t>(1));
    if ((*fiber).status_.get() == rusty::clone(Fiber::INIT)) {
        fiber->run();
    } else {
        fiber->continue_();
    }
    {
        auto&& guard = rusty::borrow(sp_running_fiber_th_);
        const rusty::Rc<Fiber>& running = ((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap();
        if (running->finished()) {
            auto fiber_ref = rusty::clone(running);
            this->recycle(fiber_ref);
        }
    }
    {
        auto&& guard = sp_running_fiber_th_.borrow_mut();
        rusty::detail::deref_if_pointer_like(guard) = std::move(old_fiber);
    }
}

void Reactor::display_waiting_ev() const {
    Log_info("waiting_events_: {}, composite_events_: {}", rusty::len(this->waiting_events_.borrow()), rusty::len(this->composite_events_.borrow()));
}

void Reactor::register_fiber(const rusty::Rc<Fiber>& fiber) const {
    auto guard = this->fibers_.borrow_mut();
    const auto inserted = rusty::deref_call(guard, rusty::detail::__mdisp_insert{}, rusty::clone(fiber)).second;
    if (rusty::detail::rust_not(inserted)) {
        // @unsafe
        {
            Log_error("[DEBUG] RegisterFiber: Failed to insert fiber into fibers_ set!");
        }
        // @unsafe
        {
            Log_error("[DEBUG] fibers_ size: {}, REUSING_FIBER: {}", rusty::deref_call(guard, rusty::detail::__mdisp_size{}), REUSING_FIBER);
        }
    }
    verify(std::move(inserted));
    verify(rusty::deref_call(guard, rusty::detail::__mdisp_size{}) > static_cast<size_t>(0));
}

void Reactor::recycle(rusty::Rc<Fiber>& fiber) const {
    if (REUSING_FIBER) {
        (*fiber).status_.set(rusty::clone(rusty::clone(Fiber::RECYCLED)));
        rusty::Function<void()> empty_fn = rusty::default_like<rusty::Function<void()>>();
        rusty::detail::deref_if_pointer_like((*fiber).func_.borrow_mut()) = std::move(empty_fn);
        this->n_idle_fibers_.set(this->n_idle_fibers_.get() + static_cast<int64_t>(1));
        this->available_fibers_.borrow_mut()->push(rusty::clone(fiber));
    }
    this->n_busy_fibers_.set(this->n_busy_fibers_.get() - static_cast<int64_t>(1));
    this->fibers_.borrow_mut()->erase(fiber);
}

void Reactor::enqueue_stackless_task(size_t idx) const {
    stackless_profile_note_enqueue();
    {
        const auto guard = this->stackless_tasks_.borrow();
        if (rusty::detail::deref_if_pointer_like(idx) >= rusty::len(guard)) {
            return;
        }
        if (rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.active); }) { return (__r.active); } else if constexpr (requires { (__r.active_field); }) { return (__r.active_field); } else if constexpr (requires { ((*__r).active); }) { return ((*__r).active); } else { return ((*__r).active_field); } }((rusty::detail::deref_if_pointer_like(guard))[idx])) || rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.queued); }) { return (__r.queued); } else if constexpr (requires { (__r.queued_field); }) { return (__r.queued_field); } else if constexpr (requires { ((*__r).queued); }) { return ((*__r).queued); } else { return ((*__r).queued_field); } }((rusty::detail::deref_if_pointer_like(guard))[idx]))) {
            return;
        }
    }
    {
        auto guard = this->stackless_tasks_.borrow_mut();
        if (rusty::detail::deref_if_pointer_like(idx) >= rusty::len(guard)) {
            return;
        }
        if (rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.active); }) { return (__r.active); } else if constexpr (requires { (__r.active_field); }) { return (__r.active_field); } else if constexpr (requires { ((*__r).active); }) { return ((*__r).active); } else { return ((*__r).active_field); } }((rusty::detail::deref_if_pointer_like(guard))[idx])) || rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.queued); }) { return (__r.queued); } else if constexpr (requires { (__r.queued_field); }) { return (__r.queued_field); } else if constexpr (requires { ((*__r).queued); }) { return ((*__r).queued); } else { return ((*__r).queued_field); } }((rusty::detail::deref_if_pointer_like(guard))[idx]))) {
            return;
        }
        [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.queued); }) { return (__r.queued); } else if constexpr (requires { (__r.queued_field); }) { return (__r.queued_field); } else if constexpr (requires { ((*__r).queued); }) { return ((*__r).queued); } else { return ((*__r).queued_field); } }((rusty::detail::deref_if_pointer_like(guard))[idx]) = true;
    }
    this->ready_stackless_tasks_.borrow_mut()->push_back(std::move(idx));
}

void Reactor::check_timeout(rusty::VecDeque<rusty::Arc<EventPollable>>& ready_events) const {
    const int64_t time_now = Time::now(true);
    auto guard = this->timeout_events_.borrow_mut();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(guard)) {
        const auto event = rusty::clone((rusty::detail::deref_if_pointer_like(guard))[i]);
        if (((rusty::detail::deref_if_pointer_like(event))).status() == rusty::clone(EventStatus::WAIT)) {
            const auto wakeup_time = ((rusty::detail::deref_if_pointer_like(event))).wakeup_time();
            verify(rusty::detail::deref_if_pointer_like(wakeup_time) > static_cast<uint64_t>(0));
            if (rusty::detail::deref_if_pointer_like(time_now) >= (static_cast<int64_t>(wakeup_time))) {
                if (((rusty::detail::deref_if_pointer_like(event))).is_ready()) {
                    ((rusty::detail::deref_if_pointer_like(event))).set_status(rusty::clone(rusty::clone(EventStatus::READY)));
                } else {
                    ((rusty::detail::deref_if_pointer_like(event))).set_status(rusty::clone(rusty::clone(EventStatus::TIMEOUT)));
                }
            }
        }
        i += static_cast<size_t>(1);
    }
    ready_events.append(rusty::deref_call(guard, rusty::detail::__mdisp_extract_if{}, [=](const rusty::Arc<EventPollable>& sp) -> bool {
const auto status = ((rusty::detail::deref_if_pointer_like(sp))).status();
return (rusty::detail::deref_if_pointer_like(status) == rusty::clone(EventStatus::READY)) || (rusty::detail::deref_if_pointer_like(status) == rusty::clone(EventStatus::TIMEOUT));
}));
    rusty::deref_call(guard, rusty::detail::__mdisp_retain{}, [=](const rusty::Arc<EventPollable>& sp) -> bool {
return ((rusty::detail::deref_if_pointer_like(sp))).status() != rusty::clone(EventStatus::DONE);
});
}

Reactor::~Reactor() noexcept(false) {
    if (_rusty_forgotten) { return; }
    Log_debug("[Reactor::~Reactor] Starting destruction, all_events_.len()={}, fibers_.size()={}", rusty::len(this->all_events_.borrow()), this->fibers_.borrow()->size());
    Log_debug("[Reactor::~Reactor] Destructor body complete, about to destroy member variables");
}
/*RUSTYCPP:GEN-END id=reactor.15*/


// ==== Member templates hoisted out of `class Reactor` (Goal 0 Stage A:
// a DSL struct's GEN cannot mix in hand-written members, so the class's
// template members become free function templates; they stay hand-written
// C++ and remain in the variadic rewrite backlog). ====

// @safe - Arc::make is @safe in the library. Hoisted out of `class Reactor`
// (was the private member template `make_arc`).
template <typename U, typename... Args>
inline rusty::Arc<U> reactor_make_arc(Args&&... args) {
  return rusty::Arc<U>::make(std::forward<Args>(args)...);
}

// @safe - Spawn a stackless task with a completion callback when ready.
// Hoisted out of `class Reactor` (member template; a DSL struct's GEN is
// fully generated so hand-written members cannot remain). Behaviour is
// identical; `self` replaces the implicit `this`.
template <typename T, typename OnReady>
inline void reactor_spawn_stackless_task_with_result(const Reactor& self, rusty::Task<T> task, OnReady on_ready) {
  constexpr size_t kUnregisteredSlot = std::numeric_limits<size_t>::max();
  struct EarlyWakeState {
    explicit EarlyWakeState(const Reactor* reactor_ptr) : reactor(reactor_ptr) {}
    const Reactor* reactor;
    mutable std::atomic<size_t> idx{kUnregisteredSlot};
    mutable std::atomic<bool> pending_wake{false};
  };

  // SAFETY: shared state is heap-owned; reactor outlives callback execution.
  auto early_wake = reactor_make_arc<EarlyWakeState>(&self);

  rusty::Waker early_waker{[early_wake, kUnregisteredSlot]() {
    size_t idx = early_wake->idx.load(std::memory_order_acquire);
    if (idx == kUnregisteredSlot) {
      early_wake->pending_wake.store(true, std::memory_order_release);
      return;
    }
    early_wake->reactor->enqueue_stackless_task(idx);
  }};
  rusty::Context early_ctx{&early_waker};
  auto early_poll = task.poll(early_ctx);
  if (early_poll.is_ready()) {
    on_ready(std::move(early_poll.value));
    return;
  }

  struct TaskState {
    mutable rusty::Task<T> task;
    mutable rusty::Option<OnReady> on_ready;
    rusty::Arc<EarlyWakeState> early_wake;

    TaskState(rusty::Task<T> t, OnReady cb, rusty::Arc<EarlyWakeState> ew)
        : task(std::move(t)), on_ready(std::move(cb)), early_wake(std::move(ew)) {}
  };

  // SAFETY: TaskState is only accessed through the Arc captured by the poller.
  auto state = reactor_make_arc<TaskState>(std::move(task), std::move(on_ready), std::move(early_wake));
  auto idx = reactor_register_stackless_poller_impl(self, [state](rusty::Context& ctx) mutable {
    auto poll_result = state->task.poll(ctx);
    if (!poll_result.is_ready()) {
      return false;
    }
    state->early_wake->idx.store(kUnregisteredSlot, std::memory_order_release);
    if (state->on_ready.is_some()) {
      // unwrap() consumes: moves out and sets to None in one step.
      auto cb = state->on_ready.unwrap();
      cb(std::move(poll_result.value));
    }
    return true;
  });
  state->early_wake->idx.store(idx, std::memory_order_release);
  if (state->early_wake->pending_wake.exchange(false, std::memory_order_acq_rel)) {
    self.enqueue_stackless_task(idx);
  }
}

// @unsafe - Creates std::shared_ptr<Event> with perfect forwarding and polymorphism support
// SAFETY: Uses std::shared_ptr for mutable access and polymorphism. Lifetime is safe because:
//   1. shared_ptr is stored in all_events_ list (an owner of the reactor)
//      while the event is live
//   2. Reactor lives for entire program duration
//   3. Finished events (sole-owned by all_events_, i.e. use_count()==1) are
//      pruned amortized via prune_finished_events(), so the list stays bounded
//      under sustained event churn (e.g. one IntEvent per recv_frame).
// Cross-thread notification reaches an event via its weak_ptr self-ref
// (get_self()), so a pruned/freed event is observed as null — no use-after-free.
template <typename Ev, typename... Args>
inline rusty::Arc<Ev> reactor_create_sp_event(Args&&... args) {  // @unsafe
  auto ev = event_make<Ev>(args...);
  // Unique-owner init window: ev is freshly minted (strong_count 1),
  // so get_mut() gives the one mutable access needed to stamp
  // __debug_creator and install the self weak-ref before ev is ever
  // shared. The self-ref is a sync::Weak<EventPollable> obtained by
  // upcasting Arc<Ev> -> Arc<EventPollable> (single-base) then
  // downgrading (sync::Weak has no derived->base converting ctor).
  {
    auto mut_opt = ev.get_mut();
    verify(mut_opt.is_some());
    Ev& m = mut_opt.unwrap();
    m.state_.__debug_creator = 1;
    m.set_self(rusty::sync::downgrade(rusty::Arc<EventPollable>(ev)));
  }
  // Store the canonical strong ref in all_events_ (upcast clone).
  auto reactor = Reactor::get_reactor();
  reactor->all_events_.borrow_mut()->push_back(rusty::Arc<EventPollable>(ev));
  // Clear out finished events the reactor is the sole owner of (bounded growth).
  reactor_prune_finished_events_impl(*reactor);
  return ev;
}

// @unsafe - Creates event and returns reference to shared_ptr content
// SAFETY: Returned reference is valid because:
//   1. Event is created via create_sp_event and stored in all_events_
//   2. The event is marked NON-prunable so all_events_ retains it (the
//      returned bare Event& is the caller's only handle — there is no
//      shared_ptr to keep it alive, so it must not be pruned)
//   3. Returned reference points to heap-allocated Event managed by shared_ptr
// Manual verification required: reference lifetime extends beyond function scope
template <typename Ev, typename... Args>
inline const Ev& reactor_create_event(Args&&... args) {  // @unsafe
  auto sp = reactor_create_sp_event<Ev>(args...);
  sp->set_prunable(false);
  return *sp;
}


// Forward declarations
class PollThread;
class PollThreadWorker;

// =============================================================================
// Channel-based communication between PollThread and PollThreadWorker
// =============================================================================

// Commands sent from PollThread to PollThreadWorker via channel
// Using std::variant for type-safe discriminated union. All seven
// emit through the DSL block below; the `pollable` field's DSL form
// `Box<PollableBase>` lowers to `rusty::Box<PollableBase>`, which the
// `PollableProxy = rusty::Box<PollableBase>` using-alias in
// `rrr.pollable_proxy` keeps backward-compatible with prior call sites.
#if RUSTYCPP_RUST
struct CmdAddPollable { pollable: Box<PollableBase> }
struct CmdRemovePollable { fd: i32 }
struct CmdClosePollable { fd: i32 }
struct CmdUpdateMode { fd: i32, new_mode: i32 }
struct CmdAddJob { job: Arc<Job> }
struct CmdRemoveJob { job: Arc<Job> }
struct CmdShutdown {}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.poll_cmds version=1 rust_sha256=322b492b439ebf16ebbdfa4e0177a363ef006de129416083fa97070e3002de7f*/
struct CmdAddPollable;
struct CmdRemovePollable;
struct CmdClosePollable;
struct CmdUpdateMode;
struct CmdAddJob;
struct CmdRemoveJob;
struct CmdShutdown;

struct CmdAddPollable {
    rusty::Box<PollableBase> pollable;
};

struct CmdRemovePollable {
    int32_t fd;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct CmdClosePollable {
    int32_t fd;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct CmdUpdateMode {
    int32_t fd;
    int32_t new_mode;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct CmdAddJob {
    rusty::Arc<Job> job;
};

struct CmdRemoveJob {
    rusty::Arc<Job> job;
};

struct CmdShutdown {
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=reactor.poll_cmds*/

using PollCommand = std::variant<
    CmdAddPollable,
    CmdRemovePollable,
    CmdClosePollable,
    CmdUpdateMode,
    CmdAddJob,
    CmdRemoveJob,
    CmdShutdown
>;

}  // export namespace rrr

// --- from reactor.h (trait spec for PollCommand) -------------------------
namespace rusty {
template<>
struct is_send<rrr::PollCommand> : std::true_type {};
} // namespace rusty

// @safe - PollThreadWorker / PollThread declarations. Class-level
// annotations + per-method `// @unsafe` overrides on the syscalls
// (epoll_wait, eventfd_write, futex) and raw pointer paths.
export namespace rrr {
// --- from reactor.h (block 2: PollThreadWorker, PollThread) -------------

// =============================================================================
// PollThreadWorker - Owns all polling state, runs in dedicated thread
// =============================================================================
// TLS slot for the worker running on this thread (set around
// poll_loop in pollthread_create's spawn lambda). Namespace-scope:
// a DSL struct cannot carry static data. `inline` keeps vague linkage.
class PollThreadWorker;
inline thread_local PollThreadWorker* g_current_poll_worker = nullptr;

// Field-type aliases for the DSL (angle-bracketed args).
using PollCmdReceiver = rusty::sync::mpsc::Receiver<PollCommand>;
using FdPollableMap = rusty::HashMap<int, PollableProxy>;
using FdModeMap = rusty::HashMap<int, int>;
using FdSet = rusty::HashSet<int>;
// std::set (not rusty::BTreeSet) — the transpiled BTreeSet drags in
// broken btree_internal clone templates; migrate when upstream fixes.
using JobSet = std::set<rusty::Arc<Job>>;

// Lifecycle + epoll/fiber kernels for the DSL methods below.
rusty::Rc<rusty::RefCell<PollThreadWorker>> pollworker_create(PollCmdReceiver receiver);
void pollworker_poll_loop(PollThreadWorker& self);
void pollworker_update_mode(PollThreadWorker& self, Pollable& poll, int new_mode);

// `PollThreadWorker` — the poll-loop state machine: epoll instance,
// fd->pollable ownership, jobs, deferred removals. Single-threaded by
// construction (owned by its poll thread through Rc<RefCell<>>).
// Authored as inline Rust DSL. Behavioral diffs:
//   * The dead zero-caller statics (both add_pollable_from_current_thread
//     overloads, private get_remove_count) are deleted.
//   * current_worker_ hoists to the namespace-scope thread_local
//     g_current_poll_worker (a DSL struct holds no statics).
//   * The public 1-arg ctor is gone; pollworker_create aggregate-
//     initializes inside Rc<RefCell<>> directly.
#if RUSTYCPP_RUST
struct PollThreadWorker {
    receiver_: PollCmdReceiver,
    poll_: Epoll,
    fd_to_pollable_: FdPollableMap,
    mode_: FdModeMap,
    pending_remove_: FdSet,
    jobs_: JobSet,
    stop_: bool,
}

impl PollThreadWorker {
    // Factory: worker wrapped in Rc<RefCell<>> for its thread.
    fn create(receiver: PollCmdReceiver) -> rusty::Rc<rusty::RefCell<PollThreadWorker>> {
        pollworker_create(receiver)
    }

    // Main polling loop — epoll events + channel commands.
    fn poll_loop(&mut self) {
        pollworker_poll_loop(self)
    }

    // Direct mode update (bypasses the channel; poll-thread only).
    fn update_mode(&mut self, poll: &mut Pollable, new_mode: i32) {
        pollworker_update_mode(self, poll, new_mode)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.poll_thread_worker version=1 rust_sha256=f647aa922d81a406d7c95b737347d8f8b915e1f0a2b2038c6c8ae6aee040bdf6*/
struct PollThreadWorker;

struct PollThreadWorker {
    PollCmdReceiver receiver_;
    Epoll poll_;
    FdPollableMap fd_to_pollable_;
    FdModeMap mode_;
    FdSet pending_remove_;
    JobSet jobs_;
    bool stop_;

    static rusty::Rc<rusty::RefCell<PollThreadWorker>> create(PollCmdReceiver receiver);
    void poll_loop();
    void update_mode(Pollable& poll, int32_t new_mode);
};


rusty::Rc<rusty::RefCell<PollThreadWorker>> PollThreadWorker::create(PollCmdReceiver receiver) {
    return pollworker_create(std::move(receiver));
}

void PollThreadWorker::poll_loop() {
    pollworker_poll_loop((*this));
}

void PollThreadWorker::update_mode(Pollable& poll, int32_t new_mode) {
    pollworker_update_mode((*this), poll, std::move(new_mode));
}
/*RUSTYCPP:GEN-END id=reactor.poll_thread_worker*/

// @safe - Check if the current thread is a poll thread.
// @unsafe - doubly blocked: reads the impl-namespace `thread_local`
// g_current_poll_worker (§7.20), and the `!= nullptr` test would emit a
// non-existent `nullptr_` (§7.31 -- `.is_null()` is the DSL spelling, but
// the static read blocks it anyway).
inline bool pollworker_is_on_poll_thread() { return g_current_poll_worker != nullptr; }

// =============================================================================
// PollThread - Handle for controlling the poll thread
// =============================================================================
// `rusty::Unit` (= std::tuple<>), NOT `void`: Rust has no void, so a
// closure returning nothing returns `()`, and thread::spawn DEDUCES
// `JoinHandle<std::tuple<>>` (see detail::SpawnResultType in
// rusty/thread.hpp). `JoinHandle<void>` still exists for code that names
// it directly, but there is no conversion between the two — so storing a
// spawn result into an `Option<JoinHandle<void>>` selected Option's
// incompatible-type ctor, which panics at RUNTIME rather than failing to
// compile ("invalid Option conversion with value" out of
// pollthread_create, taking every TcpFactoryTest down in SetUp).
// Moved ABOVE its first use: the DSL shutdown() below calls it, and an
// `inline` definition emits no external symbol, so a forward declaration
// links only if the definition is non-inline. Relocating is simpler than
// changing its linkage.
// @unsafe - C++ template metaprogramming: `decltype(std::declval<...>())`
// to name the native id type, plus std::bit_cast. Neither is DSL-
// expressible.
inline rusty::thread::ThreadId u64_to_thread_id(std::uint64_t bits) noexcept {
    using NativeId = decltype(std::declval<rusty::thread::ThreadId>().as_native());
    return rusty::thread::ThreadId{std::bit_cast<NativeId>(bits)};
}

using PollJoinSlot =
    rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<rusty::Unit>>>;

struct PollThread;

// Lifecycle + channel-send kernels for the DSL methods below (thread
// spawn/join, mpsc sends, syscall logging). Definitions near the
// original impl site.
rusty::Arc<PollThread> pollthread_create();
void pollthread_drop(const PollThread& self);

// `PollThread` — the poll-loop thread handle: an mpsc command sender,
// the join slot, and shutdown/identity state. Authored as inline Rust
// DSL. Behavioral diffs from the original C++ class:
//   * The private ctor + `friend rusty::Arc` in-place-construction
//     machinery is gone: rusty atomics are MOVABLE (value-moving move
//     ctor), so pollthread_create Arc::new_'s a plain aggregate.
//   * shutdown_called_ becomes AtomicBool (rusty) — const ops, no
//     `mutable`; the exchange() gate becomes swap().
//   * The zero-caller remove(Arc<Job>) overload and the
//     one-dead-test-caller update_mode(const Pollable&) overload are
//     dropped (a Rust impl holds no overloads); remove(Pollable&)
//     keeps its name.
#if RUSTYCPP_RUST
struct PollThread {
    sender_: rusty::sync::mpsc::Sender<PollCommand>,
    join_handle_: PollJoinSlot,
    // Thread id of the poll thread as raw u64 bits (bit_cast of the
    // native id) — used to detect self-join attempts in shutdown.
    poll_thread_id_bits_: AtomicU64,
    shutdown_called_: AtomicBool,
}

impl PollThread {
    // Factory: spawns the worker thread; returns the Arc handle.
    fn create() -> Arc<PollThread> {
        pollthread_create()
    }

    // Explicit shutdown: send CmdShutdown, join unless self-join.
    fn shutdown(&self) {
        let main_tid: i64 = unsafe { syscall(SYS_gettid) };
        Log_debug("[PollThread::shutdown] Called from TID={}", main_tid as i32);
        if self.shutdown_called_.swap(true) {
            Log_debug("[PollThread::shutdown] Already called, returning");
            return;
        }
        Log_debug("[PollThread::shutdown] Sending CmdShutdown");
        self.sender_.send(CmdShutdown {});
        Log_debug("[PollThread::shutdown] CmdShutdown sent");
        // Thread-safe read of the poll thread's id.
        let current_tid = rusty::thread::current_id();
        let poll_tid = u64_to_thread_id(
            self.poll_thread_id_bits_.load(rusty::sync::atomic::Ordering::Acquire));
        if current_tid == poll_tid {
            Log_debug("[PollThread::shutdown] Called from poll thread, skipping join");
            return;
        }
        Log_debug("[PollThread::shutdown] Acquiring join_handle lock...");
        // Scoped so the guard drops BEFORE the "Released" log below, as the
        // C++ block did.
        {
            let mut guard = self.join_handle_.lock().unwrap();
            Log_debug("[PollThread::shutdown] join_handle lock acquired");
            if (*guard).is_some() {
                Log_debug("[PollThread::shutdown] Calling thread.join()...");
                (*guard).take().unwrap().join();
                Log_debug("[PollThread::shutdown] thread.join() completed!");
            } else {
                Log_debug("[PollThread::shutdown] join_handle is None, thread already joined");
            }
        }
        Log_debug("[PollThread::shutdown] Released join_handle lock");
        Log_debug("[PollThread::shutdown] Complete");
    }

    fn add_proxy(&self, poll: PollableProxy) {
        self.sender_.send(CmdAddPollable { pollable: poll });
    }

    fn remove(&self, poll: &mut Pollable) {
        self.sender_.send(CmdRemovePollable { fd: poll.fd() });
    }

    // fd-keyed variant (remove only reads .fd() anyway); lets
    // shim-only callers avoid the Pollable base entirely.
    fn remove_fd(&self, fd: i32) {
        self.sender_.send(CmdRemovePollable { fd: fd });
    }

    // Thread-safe close: removes from epoll, closes socket, drops
    // proxy ownership.
    fn request_close(&self, fd: i32) {
        self.sender_.send(CmdClosePollable { fd: fd });
    }

    fn update_mode(&self, fd: i32, new_mode: i32) {
        let result = self.sender_.send(CmdUpdateMode { fd: fd, new_mode: new_mode });
        if result.is_err() {
            unsafe { Log_error("PollThread::update_mode: send failed! Channel disconnected?"); }
        }
    }

    fn add(&self, job: Arc<Job>) {
        self.sender_.send(CmdAddJob { job: job });
    }

    // For testing — worker state is not reachable across the channel.
    fn get_remove_count(&self) -> i32 {
        0
    }
}

impl Drop for PollThread {
    fn drop(&mut self) {
        pollthread_drop(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.poll_thread version=1 rust_sha256=71ba4dfeb8629bff1325aaddd1cc943302bfd07265293f1612c4ea1fcba134bb*/
struct PollThread;

struct PollThread {
    rusty::sync::mpsc::Sender<PollCommand> sender_;
    PollJoinSlot join_handle_;
    rusty::sync::atomic::AtomicU64 poll_thread_id_bits_;
    rusty::sync::atomic::AtomicBool shutdown_called_;
    mutable bool _rusty_forgotten = false;
    PollThread(rusty::sync::mpsc::Sender<PollCommand> sender__init, PollJoinSlot join_handle__init, rusty::sync::atomic::AtomicU64 poll_thread_id_bits__init, rusty::sync::atomic::AtomicBool shutdown_called__init) : sender_(std::move(sender__init)), join_handle_(std::move(join_handle__init)), poll_thread_id_bits_(std::move(poll_thread_id_bits__init)), shutdown_called_(std::move(shutdown_called__init)) {}
    PollThread(const PollThread&) = default;
    PollThread(PollThread&& other) noexcept : sender_(std::move(other.sender_)), join_handle_(std::move(other.join_handle_)), poll_thread_id_bits_(std::move(other.poll_thread_id_bits_)), shutdown_called_(std::move(other.shutdown_called_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    PollThread& operator=(const PollThread&) = default;
    PollThread& operator=(PollThread&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~PollThread();
        new (this) PollThread(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->sender_); rusty::detail::mark_forgotten_if_supported(this->join_handle_); rusty::detail::mark_forgotten_if_supported(this->poll_thread_id_bits_); rusty::detail::mark_forgotten_if_supported(this->shutdown_called_); }


    static rusty::Arc<PollThread> create();
    void shutdown() const;
    void add_proxy(PollableProxy poll) const;
    void remove(Pollable& poll) const;
    void remove_fd(int32_t fd) const;
    void request_close(int32_t fd) const;
    void update_mode(int32_t fd, int32_t new_mode) const;
    void add(rusty::Arc<Job> job) const;
    int32_t get_remove_count() const;
    ~PollThread() noexcept(false);
};


rusty::Arc<PollThread> PollThread::create() {
    return pollthread_create();
}

void PollThread::shutdown() const {
    const int64_t main_tid = syscall(SYS_gettid);
    Log_debug("[PollThread::shutdown] Called from TID={}", static_cast<int32_t>(main_tid));
    if (this->shutdown_called_.swap(true)) {
        Log_debug("[PollThread::shutdown] Already called, returning");
        return;
    }
    Log_debug("[PollThread::shutdown] Sending CmdShutdown");
    this->sender_.send(CmdShutdown{});
    Log_debug("[PollThread::shutdown] CmdShutdown sent");
    const auto current_tid = rusty::thread::current_id();
    const auto poll_tid = u64_to_thread_id(this->poll_thread_id_bits_.load(rusty::sync::atomic::Ordering::Acquire));
    if (rusty::detail::deref_if_pointer_like(current_tid) == rusty::detail::deref_if_pointer_like(poll_tid)) {
        Log_debug("[PollThread::shutdown] Called from poll thread, skipping join");
        return;
    }
    Log_debug("[PollThread::shutdown] Acquiring join_handle lock...");
    {
        auto guard = this->join_handle_.lock().unwrap();
        Log_debug("[PollThread::shutdown] join_handle lock acquired");
        if (((rusty::detail::deref_if_pointer_like(guard))).is_some()) {
            Log_debug("[PollThread::shutdown] Calling thread.join()...");
            ((rusty::detail::deref_if_pointer_like(guard))).take().unwrap().join();
            Log_debug("[PollThread::shutdown] thread.join() completed!");
        } else {
            Log_debug("[PollThread::shutdown] join_handle is None, thread already joined");
        }
    }
    Log_debug("[PollThread::shutdown] Released join_handle lock");
    Log_debug("[PollThread::shutdown] Complete");
}

void PollThread::add_proxy(PollableProxy poll) const {
    this->sender_.send(CmdAddPollable{.pollable = std::move(poll)});
}

void PollThread::remove(Pollable& poll) const {
    this->sender_.send(CmdRemovePollable{.fd = poll.fd()});
}

void PollThread::remove_fd(int32_t fd) const {
    this->sender_.send(CmdRemovePollable{.fd = std::move(fd)});
}

void PollThread::request_close(int32_t fd) const {
    this->sender_.send(CmdClosePollable{.fd = std::move(fd)});
}

void PollThread::update_mode(int32_t fd, int32_t new_mode) const {
    const auto result = this->sender_.send(CmdUpdateMode{.fd = std::move(fd), .new_mode = std::move(new_mode)});
    if (result.is_err()) {
        // @unsafe
        {
            Log_error("PollThread::update_mode: send failed! Channel disconnected?");
        }
    }
}

void PollThread::add(rusty::Arc<Job> job) const {
    this->sender_.send(CmdAddJob{.job = std::move(job)});
}

int32_t PollThread::get_remove_count() const {
    return static_cast<int32_t>(0);
}

PollThread::~PollThread() noexcept(false) {
    if (_rusty_forgotten) { return; }
    pollthread_drop((*this));
}
/*RUSTYCPP:GEN-END id=reactor.poll_thread*/

}  // export namespace rrr

// --- from reactor.h (trait specs for PollThread) -------------------------
namespace rusty {
template<>
struct is_send<rrr::PollThread> : std::true_type {};

template<>
struct is_sync<rrr::PollThread> : std::true_type {};
} // namespace rusty

// --- from quorum_event.h --------------------------------------------------
// @safe - QuorumEvent declarations under the janus namespace.
export namespace janus {

// Pulled in from former `quorum_event.h` (lines 26-29). Folded into the
// janus-namespace purview of the consolidated `rrr.reactor` module so
// `QuorumEvent` can name `Event` / `IntEvent` / `verify` / `shared_ptr`
// unqualified, matching the original source.
using rrr::IntEvent;
using rrr::verify;
using std::shared_ptr;
// S4 flatten: QuorumEvent derives EventPollable and drives the shared event
// kernels directly, so it now names these rrr entities unqualified. (The
// kernels can't be found by ADL here — `*this` is janus::QuorumEvent — so the
// using-declarations are required, not just convenient.)
using rrr::EventPollable;
using rrr::EventStatus;
using rrr::EventState;
using rrr::Reactor;
using rrr::Fiber;
using rrr::event_state_seed;
using rrr::event_core_get_fiber_id;
using rrr::event_core_self_lock;
using rrr::event_core_set_self;
using rrr::event_core_wakeup_time;
using rrr::event_core_upgrade_fiber;
using rrr::event_wait_impl;
using rrr::event_test_impl;

// Quorum-math specialization (composition-flattening S3): the former
// per-protocol QuorumEvent subclasses expressed their yes()/no()/is_ready()
// variations by overriding; those variations are DATA, captured exactly by
// this policy enum. The full live override matrix across all 19 protocol
// subclasses reduces to:
//   DEFAULT         — yes: n_voted_yes_ >= quorum_;
//                     no: n_voted_no_ > n_total_ - quorum_;
//                     ready: timeouted_ || yes || no
//   ALL_NO          — (GetLeader) no: every voter said no
//                     (n_voted_no_ == n_total_); ready: yes || no. The
//                     dropped timeouted_ check is equivalent to DEFAULT
//                     because timeouted_ has zero writers repo-wide (do not
//                     add one without revisiting this policy).
//   LEADER_AND      — (RuleSpeculativeExecute) yes additionally requires
//                     n_leader_yes_ >= num_leader_; no additionally trips
//                     on any leader-no (n_leader_no_ > 0).
//   COMMITTED_SHORT — (CopilotPrepare) ready short-circuits on
//                     committed_seen_ (a committed reply obviates the
//                     quorum), then falls back to DEFAULT's shape.
//   ALWAYS_READY    — (CopilotFake) no quorum semantics at all.
enum class QuorumPolicy : int {
  DEFAULT = 0,
  ALL_NO = 1,
  LEADER_AND = 2,
  COMMITTED_SHORT = 3,
  ALWAYS_READY = 4,
};

// FLATTENED (S4): QuorumEvent is now an inline-Rust DSL struct that derives
// EventPollable via `#[cpp_inherit]` — the `Arc<QuorumEvent> ->
// Arc<EventPollable>` upcast at its create_sp_event / finalize_event_ sites
// needs the transpiler's opt-in direct-inheritance mode (untagged impls emit an
// adapter, which cannot upcast). It carries the five event-core fields inline
// and is driven by the shared event_wait_impl / event_test_impl / event_core_*
// kernels, like the other flattened events. Two bodies stay hand-written
// @unsafe C++ kernels the DSL calls: `quorum_event_finalize` (spawns a fiber
// with a move-capturing closure) and `quorum_event_is_slow` (reads/clears the
// reactor's shared slow_ flag). Construction goes through `quorum_event_make`
// (wired into rrr::event_make). The owning QuorumEventWrapper is unchanged.

// The finalize callback type: the DSL cannot parse a bare fn-type template
// argument as a field/param signature, so alias it outside the block (the
// established rrr-dsl idiom for Function-typed members/params).
using QuorumFinalizeFn =
    rusty::Function<bool(rusty::Vec<std::pair<uint16_t, rrr::i64> >&)>;

// Hand-written @unsafe kernels the DSL bodies call. Declared before the DSL
// block so the generated method bodies resolve them by ordinary lookup.
// @unsafe - fiber-spawning finalize closure; reactor slow_ poke.
struct QuorumEvent;
void quorum_event_finalize(const QuorumEvent& self, uint64_t timeout,
                           QuorumFinalizeFn finalize_func);
bool quorum_event_is_slow(const QuorumEvent& self);

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block.
#if RUSTYCPP_RUST
struct QuorumEvent {
    status_: Cell<EventStatus>,
    owner_thread_: rusty::thread::ThreadId,
    state_: EventState,
    prunable_: Cell<bool>,
    self_: rusty::sync::Weak<EventPollable>,
    n_voted_yes_: Cell<i32>,
    n_voted_no_: Cell<i32>,
    xids_: RefCell<rusty::HashMap<u16, rrr::i64>>,
    n_total_: i32,
    quorum_: i32,
    policy_: Cell<QuorumPolicy>,
    committed_seen_: Cell<bool>,
    num_leader_: Cell<i32>,
    n_leader_yes_: Cell<i32>,
    n_leader_no_: Cell<i32>,
    highest_term_: Cell<i64>,
    timeouted_: Cell<bool>,
    leader_id_: Cell<u32>,
    par_id_: Cell<i64>,
    id_: Cell<u64>,
    finalize_event_: rusty::Arc<IntEvent>,
}

impl QuorumEvent {
    fn add_xid(&self, site: u16, xid: rrr::i64) {
        self.xids_.borrow_mut().insert(site, xid);
    }
    fn remove_xid(&self, site: u16) {
        self.xids_.borrow_mut().remove(site);
    }
    fn finalize(&self, timeout: u64, finalize_func: QuorumFinalizeFn) {
        quorum_event_finalize(self, timeout, finalize_func)
    }
    fn yes(&self) -> bool {
        let base = self.n_voted_yes_.get() >= self.quorum_;
        if self.policy_.get() == QuorumPolicy::LEADER_AND {
            return base && self.n_leader_yes_.get() >= self.num_leader_.get();
        }
        base
    }
    fn no(&self) -> bool {
        if self.policy_.get() == QuorumPolicy::ALL_NO {
            return self.n_voted_no_.get() == self.n_total_;
        }
        verify(self.n_total_ >= self.quorum_);
        let base = self.n_voted_no_.get() > (self.n_total_ - self.quorum_);
        if self.policy_.get() == QuorumPolicy::LEADER_AND {
            return base || self.n_leader_no_.get() > 0;
        }
        base
    }
    fn vote_yes(&self) {
        self.n_voted_yes_.set(self.n_voted_yes_.get() + 1);
        event_test_impl(self);
        let fe = self.finalize_event_.clone();
        if (*fe).status_.get() != EventStatus::TIMEOUT && (*fe).status_.get() != EventStatus::DONE {
            (*fe).set(self.n_voted_yes_.get() + self.n_voted_no_.get());
        }
    }
    fn vote_no(&self) {
        self.n_voted_no_.set(self.n_voted_no_.get() + 1);
        event_test_impl(self);
        let fe = self.finalize_event_.clone();
        if (*fe).status_.get() != EventStatus::TIMEOUT && (*fe).status_.get() != EventStatus::DONE {
            (*fe).set(self.n_voted_yes_.get() + self.n_voted_no_.get());
        }
    }
    fn is_composite_event(&self) -> bool {
        true
    }
    fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    fn get_fiber_id(&self) -> u64 {
        event_core_get_fiber_id()
    }
    fn is_slow(&self) -> bool {
        quorum_event_is_slow(self)
    }
    fn get_self(&self) -> rusty::Option<rusty::Arc<EventPollable>> {
        event_core_self_lock(self)
    }
    fn set_self(&mut self, self_ptr: rusty::sync::Weak<EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl EventPollable for QuorumEvent {
    fn test(&self) -> bool {
        event_test_impl(self)
    }
    fn is_ready(&self) -> bool {
        let p = self.policy_.get();
        if p == QuorumPolicy::ALWAYS_READY {
            return true;
        }
        if p == QuorumPolicy::ALL_NO {
            return self.yes() || self.no();
        }
        if p == QuorumPolicy::COMMITTED_SHORT {
            if self.timeouted_.get() {
                return true;
            }
            if self.committed_seen_.get() {
                return true;
            }
            return self.yes() || self.no();
        }
        if self.timeouted_.get() {
            return true;
        }
        self.yes() || self.no()
    }
    fn log(&self) {}
    fn status(&self) -> EventStatus {
        self.status_.get()
    }
    fn set_status(&self, s: EventStatus) {
        self.status_.set(s)
    }
    fn wakeup_time(&self) -> u64 {
        event_core_wakeup_time(self)
    }
    fn prunable(&self) -> bool {
        self.prunable_.get()
    }
    fn set_prunable(&self, v: bool) {
        self.prunable_.set(v)
    }
    fn upgrade_fiber(&self) -> rusty::Option<rusty::Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.quorum_event version=1 rust_sha256=b4a1ee9c2e71a307974f3f62652465e3b24b823075b9a1002d41aeeb480cdbe3*/
struct QuorumEvent;

struct QuorumEvent : public EventPollable {
    rusty::Cell<EventStatus> status_;
    rusty::thread::ThreadId owner_thread_;
    EventState state_;
    rusty::Cell<bool> prunable_;
    rusty::sync::Weak<EventPollable> self_;
    rusty::Cell<int32_t> n_voted_yes_;
    rusty::Cell<int32_t> n_voted_no_;
    rusty::RefCell<rusty::HashMap<uint16_t, rrr::i64>> xids_;
    int32_t n_total_;
    int32_t quorum_;
    rusty::Cell<QuorumPolicy> policy_;
    rusty::Cell<bool> committed_seen_;
    rusty::Cell<int32_t> num_leader_;
    rusty::Cell<int32_t> n_leader_yes_;
    rusty::Cell<int32_t> n_leader_no_;
    rusty::Cell<int64_t> highest_term_;
    rusty::Cell<bool> timeouted_;
    rusty::Cell<uint32_t> leader_id_;
    rusty::Cell<int64_t> par_id_;
    rusty::Cell<uint64_t> id_;
    rusty::Arc<IntEvent> finalize_event_;
    QuorumEvent(rusty::Cell<EventStatus> status__init, rusty::thread::ThreadId owner_thread__init, EventState state__init, rusty::Cell<bool> prunable__init, rusty::sync::Weak<EventPollable> self__init, rusty::Cell<int32_t> n_voted_yes__init, rusty::Cell<int32_t> n_voted_no__init, rusty::RefCell<rusty::HashMap<uint16_t, rrr::i64>> xids__init, int32_t n_total__init, int32_t quorum__init, rusty::Cell<QuorumPolicy> policy__init, rusty::Cell<bool> committed_seen__init, rusty::Cell<int32_t> num_leader__init, rusty::Cell<int32_t> n_leader_yes__init, rusty::Cell<int32_t> n_leader_no__init, rusty::Cell<int64_t> highest_term__init, rusty::Cell<bool> timeouted__init, rusty::Cell<uint32_t> leader_id__init, rusty::Cell<int64_t> par_id__init, rusty::Cell<uint64_t> id__init, rusty::Arc<IntEvent> finalize_event__init) : EventPollable(), status_(std::move(status__init)), owner_thread_(std::move(owner_thread__init)), state_(std::move(state__init)), prunable_(std::move(prunable__init)), self_(std::move(self__init)), n_voted_yes_(std::move(n_voted_yes__init)), n_voted_no_(std::move(n_voted_no__init)), xids_(std::move(xids__init)), n_total_(std::move(n_total__init)), quorum_(std::move(quorum__init)), policy_(std::move(policy__init)), committed_seen_(std::move(committed_seen__init)), num_leader_(std::move(num_leader__init)), n_leader_yes_(std::move(n_leader_yes__init)), n_leader_no_(std::move(n_leader_no__init)), highest_term_(std::move(highest_term__init)), timeouted_(std::move(timeouted__init)), leader_id_(std::move(leader_id__init)), par_id_(std::move(par_id__init)), id_(std::move(id__init)), finalize_event_(std::move(finalize_event__init)) {}
    QuorumEvent(QuorumEvent&& other) noexcept : EventPollable(), status_(std::move(other.status_)), owner_thread_(std::move(other.owner_thread_)), state_(std::move(other.state_)), prunable_(std::move(other.prunable_)), self_(std::move(other.self_)), n_voted_yes_(std::move(other.n_voted_yes_)), n_voted_no_(std::move(other.n_voted_no_)), xids_(std::move(other.xids_)), n_total_(std::move(other.n_total_)), quorum_(std::move(other.quorum_)), policy_(std::move(other.policy_)), committed_seen_(std::move(other.committed_seen_)), num_leader_(std::move(other.num_leader_)), n_leader_yes_(std::move(other.n_leader_yes_)), n_leader_no_(std::move(other.n_leader_no_)), highest_term_(std::move(other.highest_term_)), timeouted_(std::move(other.timeouted_)), leader_id_(std::move(other.leader_id_)), par_id_(std::move(other.par_id_)), id_(std::move(other.id_)), finalize_event_(std::move(other.finalize_event_)) {}


    void add_xid(uint16_t site, rrr::i64 xid) const;
    void remove_xid(uint16_t site) const;
    void finalize(uint64_t timeout, QuorumFinalizeFn finalize_func) const;
    bool yes() const;
    bool no() const;
    void vote_yes() const;
    void vote_no() const;
    bool is_composite_event() const;
    void wait() const;
    void wait_timeout(uint64_t timeout) const;
    uint64_t get_fiber_id() const;
    bool is_slow() const;
    rusty::Option<rusty::Arc<EventPollable>> get_self() const;
    void set_self(rusty::sync::Weak<EventPollable> self_ptr);
    bool test() const;
    bool is_ready() const;
    void log() const;
    EventStatus status() const;
    void set_status(EventStatus s) const;
    uint64_t wakeup_time() const;
    bool prunable() const;
    void set_prunable(bool v) const;
    rusty::Option<rusty::Rc<Fiber>> upgrade_fiber() const;
};


void QuorumEvent::add_xid(uint16_t site, rrr::i64 xid) const {
    this->xids_.borrow_mut()->insert(std::move(site), std::move(xid));
}

void QuorumEvent::remove_xid(uint16_t site) const {
    this->xids_.borrow_mut()->remove(std::move(site));
}

void QuorumEvent::finalize(uint64_t timeout, QuorumFinalizeFn finalize_func) const {
    quorum_event_finalize((*this), std::move(timeout), std::move(finalize_func));
}

bool QuorumEvent::yes() const {
    auto base = this->n_voted_yes_.get() >= rusty::detail::deref_if_pointer_like(this->quorum_);
    if (this->policy_.get() == rusty::clone(QuorumPolicy::LEADER_AND)) {
        return rusty::detail::deref_if_pointer_like(base) && (this->n_leader_yes_.get() >= this->num_leader_.get());
    }
    return std::move(base);
}

bool QuorumEvent::no() const {
    if (this->policy_.get() == rusty::clone(QuorumPolicy::ALL_NO)) {
        return this->n_voted_no_.get() == rusty::detail::deref_if_pointer_like(this->n_total_);
    }
    verify(rusty::detail::deref_if_pointer_like(this->n_total_) >= rusty::detail::deref_if_pointer_like(this->quorum_));
    auto base = this->n_voted_no_.get() > ((rusty::detail::deref_if_pointer_like(this->n_total_) - rusty::detail::deref_if_pointer_like(this->quorum_)));
    if (this->policy_.get() == rusty::clone(QuorumPolicy::LEADER_AND)) {
        return rusty::detail::deref_if_pointer_like(base) || (this->n_leader_no_.get() > 0);
    }
    return std::move(base);
}

void QuorumEvent::vote_yes() const {
    this->n_voted_yes_.set(this->n_voted_yes_.get() + static_cast<int32_t>(1));
    event_test_impl((*this));
    const auto fe = rusty::clone(this->finalize_event_);
    if (((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(EventStatus::TIMEOUT)) && ((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(EventStatus::DONE))) {
        ((rusty::detail::deref_if_pointer_like(fe))).set(this->n_voted_yes_.get() + this->n_voted_no_.get());
    }
}

void QuorumEvent::vote_no() const {
    this->n_voted_no_.set(this->n_voted_no_.get() + static_cast<int32_t>(1));
    event_test_impl((*this));
    const auto fe = rusty::clone(this->finalize_event_);
    if (((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(EventStatus::TIMEOUT)) && ((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(EventStatus::DONE))) {
        ((rusty::detail::deref_if_pointer_like(fe))).set(this->n_voted_yes_.get() + this->n_voted_no_.get());
    }
}

bool QuorumEvent::is_composite_event() const {
    return true;
}

void QuorumEvent::wait() const {
    event_wait_impl((*this), static_cast<uint64_t>(0));
}

void QuorumEvent::wait_timeout(uint64_t timeout) const {
    event_wait_impl((*this), std::move(timeout));
}

uint64_t QuorumEvent::get_fiber_id() const {
    return event_core_get_fiber_id();
}

bool QuorumEvent::is_slow() const {
    return quorum_event_is_slow((*this));
}

rusty::Option<rusty::Arc<EventPollable>> QuorumEvent::get_self() const {
    return event_core_self_lock((*this));
}

void QuorumEvent::set_self(rusty::sync::Weak<EventPollable> self_ptr) {
    event_core_set_self((*this), std::move(self_ptr));
}

bool QuorumEvent::test() const {
    return event_test_impl((*this));
}

bool QuorumEvent::is_ready() const {
    const auto p = this->policy_.get();
    if (rusty::detail::deref_if_pointer_like(p) == rusty::clone(QuorumPolicy::ALWAYS_READY)) {
        return true;
    }
    if (rusty::detail::deref_if_pointer_like(p) == rusty::clone(QuorumPolicy::ALL_NO)) {
        return this->yes() || this->no();
    }
    if (rusty::detail::deref_if_pointer_like(p) == rusty::clone(QuorumPolicy::COMMITTED_SHORT)) {
        if (this->timeouted_.get()) {
            return true;
        }
        if (this->committed_seen_.get()) {
            return true;
        }
        return this->yes() || this->no();
    }
    if (this->timeouted_.get()) {
        return true;
    }
    return this->yes() || this->no();
}

void QuorumEvent::log() const {
}

EventStatus QuorumEvent::status() const {
    return this->status_.get();
}

void QuorumEvent::set_status(EventStatus s) const {
    this->status_.set(std::move(s));
}

uint64_t QuorumEvent::wakeup_time() const {
    return event_core_wakeup_time((*this));
}

bool QuorumEvent::prunable() const {
    return this->prunable_.get();
}

void QuorumEvent::set_prunable(bool v) const {
    this->prunable_.set(std::move(v));
}

rusty::Option<rusty::Rc<Fiber>> QuorumEvent::upgrade_fiber() const {
    return event_core_upgrade_fiber((*this));
}
/*RUSTYCPP:GEN-END id=reactor.quorum_event*/

// Composition base for the per-protocol quorum events (flattening S3b).
// The former `class XQuorumEvent : public QuorumEvent` subclasses become
// `class XQuorumEvent : public QuorumEventWrapper` — they OWN the reactor-
// registered QuorumEvent instead of BEING it, so nothing outside rrr
// inherits the event type (a hard requirement for flattening QuorumEvent
// to an inline-Rust DSL struct, which cannot be a base class). The wrapper
// itself is not an Event and is never registered; waiting/voting forward
// to the owned, registered `q_`.
//
// Field access through a wrapper goes via `q()`:  e->timeouted_  becomes
// e->q().timeouted_. The common verb surface is forwarded so method call
// sites compile unchanged. `q_` is set once at construction and never
// reseated.
class QuorumEventWrapper {
 public:
  rusty::Arc<QuorumEvent> q_;

  QuorumEventWrapper(int n_total, int quorum)
      : q_(rrr::reactor_create_sp_event<QuorumEvent>(n_total, quorum)) {}

  // Arc is const-view; every QuorumEvent field mutation now goes
  // through Cell::set / RefCell (both const), so a const ref suffices.
  const QuorumEvent& q() { return *q_; }
  const QuorumEvent& q() const { return *q_; }

  // Forwarded verb surface (matches the former inherited methods):
  void wait() { q_->wait(); }
  void wait_timeout(uint64_t timeout) { q_->wait_timeout(timeout); }
  void log() { q_->log(); }
  uint64_t get_fiber_id() { return q_->get_fiber_id(); }
  void vote_yes() { q_->vote_yes(); }
  void vote_no() { q_->vote_no(); }
  bool yes() { return q_->yes(); }
  bool no() { return q_->no(); }
  bool is_ready() { return q_->is_ready(); }
  bool is_slow() { return q_->is_slow(); }
  void test() { q_->test(); }
  void add_xid(uint16_t site, rrr::i64 xid) { q_->add_xid(site, xid); }
  void remove_xid(uint16_t site) { q_->remove_xid(site); }
  void finalize(uint64_t timeout,
                rusty::Function<bool(rusty::Vec<std::pair<uint16_t, rrr::i64> >&)> f) {
    q_->finalize(timeout, std::move(f));
  }
};

}  // export namespace janus

// ===========================================================================
// Out-of-line definitions (from former event.cc, fiber_impl.cc, reactor.cc,
// fiber_context_runtime.cc)
// ===========================================================================
// @safe - Implementation namespace. Out-of-class definitions inherit
// per-method `// @unsafe` annotations from the declarations above.
// The anonymous-namespace `stat_*` / `stackless_profile_*` helpers
// (line 1582+) and other free-function impl details carry their own
// `// @unsafe` markers individually where needed.
namespace rrr {

// --- from event.cc -------------------------------------------------------



// void Event::Wait(uint64_t timeoutuint64_t timeout) {
// //  verify(__debug_creator); // if this fails, the event is not created by reactor.

//   verify(sp_reactor_th_);
//   verify(sp_reactor_th_->thread_id_ == rusty::thread::current_id());
//   if (IsReady()) {
//     status_ = DONE; // does not need to wait.
//     return;
//   } else {
//     verify(status_ == INIT);
//     status_= DEBUG;
//     // the event may be created in a different fiber.
//     // this value is set when wait is called.
//     // for now only one fiber can wait on an event.
//     auto sp_fiber = Fiber::current_fiber();
// //    verify(sp_fiber);
// //    verify(_dbg_p_scheduler_ == nullptr);
// //    _dbg_p_scheduler_ = Reactor::get_reactor().get();
//     auto& events = Reactor::get_reactor()->waiting_events_;
//     events.push_back(shared_from_this());
//     wp_fiber_ = sp_fiber;
//     status_ = WAIT;
//     sp_fiber->yield_();
//   }
// }

// Flattening S4: the wait machinery, extracted from Event::wait as a generic
// kernel over the concrete event type W. Duck-typed surface: W provides
// status_, state_, is_ready(), is_composite_event(), get_self(). Works
// identically for the legacy Event hierarchy (virtual dispatch through
// W=Event) and the flattened per-kind DSL structs (static dispatch).
//
// Authored as inline Rust DSL (docs/porting-cpp-to-rust-dsl.md §7.9).
// Convertible since rusty-cpp #32/#33 (guard-producing calls on generic
// receivers → deref dispatch), #34 (deref through a generic guard receiver on
// an assignment LHS), and #35 (keep the guard deref for a CONCRETE receiver
// too — the `borrow_mut().push_back()` enqueues) all landed. Param is `ev`,
// not `self` — a free-function param named `self` lowers to a method receiver.
// Rc field/method access uses the explicit `(*rc).member` deref form; a value
// binding (`.clone()`) is required for `*` to lower — a reference binding or an
// inline `*<call-chain>` drops the deref. The dead `#ifdef EVENT_TIMEOUT_CHECK`
// branches (macro never defined) are dropped.
#if RUSTYCPP_RUST
fn event_wait_impl<W>(ev: &W, timeout: u64) {
    verify(sp_reactor_th_.is_some());
    // `.clone()` binds a *value* Rc (not a reference): `*ident` lowers to a
    // deref only for value bindings, so `(*reactor_th).thread_id_` reaches
    // through the Rc.
    let reactor_th = sp_reactor_th_.as_ref().unwrap().clone();
    verify((*reactor_th).thread_id_.get() == rusty::thread::current_id());
    if ev.status_.get() == EventStatus::DONE {
        return; // second use of the event
    }
    if ev.is_ready() {
        ev.status_.set(EventStatus::DONE); // no need to wait
        return;
    } else {
        // The event may be created in a different fiber; for now only one
        // fiber can wait on an event. Capture the running fiber to wake later.
        let fiber_opt = Fiber::current_fiber();
        verify(fiber_opt.is_some()); // can't wait outside a fiber
        let fiber = fiber_opt.unwrap();

        let reactor_rc = Reactor::get_reactor();
        // Inline `borrow_mut().push_back(…)`: the RefMut temporary releases at
        // the end of each statement — before the yield below — so the reactor
        // loop can re-borrow these queues while this fiber sleeps. (#35 keeps
        // the guard deref for these concrete-receiver calls.)
        (*reactor_rc).waiting_events_.borrow_mut().push_back(ev.get_self().unwrap());

        // Composite events (WaitAll/WaitAny/Quorum) need periodic polling; add
        // them to a smaller scanned queue. Regular RPC events self-notify.
        if ev.is_composite_event() {
            (*reactor_rc).composite_events_.borrow_mut().push_back(ev.get_self().unwrap());
        }

        if timeout > 0 {
            let now = Time::now(true);
            ev.state_.wakeup_time_.set(now + timeout);
            (*reactor_rc).timeout_events_.borrow_mut().push_back(ev.get_self().unwrap());
        }

        // Transpiled Weak has no implicit Rc→Weak conversion; use the static
        // Rc::downgrade(rc) factory (mirrors std::rc::Rc::downgrade). `fiber` is
        // cloned (a refcount bump) so the factory consumes the temporary and the
        // original `fiber` stays live for the checks below.
        *ev.state_.wp_fiber_.borrow_mut() = ::rusty::port::rc::Rc::<Fiber>::downgrade(fiber.clone());
        ev.status_.set(EventStatus::WAIT);
        let fiber_status = (*fiber).status_.get();
        verify(fiber_status != Fiber::FINISHED && fiber_status != Fiber::RECYCLED);
        (*fiber).yield_();
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.13 version=1 rust_sha256=ea6cbb771479971d143f60f862b2af9bfa86340585731f4a80a84b1c99c4f87c*/
template<typename W>
void event_wait_impl(const W& ev, uint64_t timeout);

template<typename W>
void event_wait_impl(const W& ev, uint64_t timeout) {
    verify(sp_reactor_th_.is_some());
    const auto reactor_th = rusty::clone(sp_reactor_th_.as_ref().unwrap());
    verify((rusty::detail::deref_if_pointer_like(reactor_th)).thread_id_.get() == rusty::thread::current_id());
    if (ev.status_.get() == rusty::clone(EventStatus::DONE)) {
        return;
    }
    if (rusty::deref_call(ev, rusty::detail::__mdisp_is_ready{})) {
        ev.status_.set(rusty::clone(rusty::clone(EventStatus::DONE)));
        return;
    } else {
        auto fiber_opt = Fiber::current_fiber();
        verify(fiber_opt.is_some());
        const auto fiber = fiber_opt.unwrap();
        const auto reactor_rc = Reactor::get_reactor();
        rusty::deref_call((rusty::detail::deref_if_pointer_like(reactor_rc)).waiting_events_.borrow_mut(), rusty::detail::__mdisp_push_back{}, rusty::deref_call(ev, rusty::detail::__mdisp_get_self{}).unwrap());
        if (rusty::deref_call(ev, rusty::detail::__mdisp_is_composite_event{})) {
            rusty::deref_call((rusty::detail::deref_if_pointer_like(reactor_rc)).composite_events_.borrow_mut(), rusty::detail::__mdisp_push_back{}, rusty::deref_call(ev, rusty::detail::__mdisp_get_self{}).unwrap());
        }
        if (rusty::detail::deref_if_pointer_like(timeout) > 0) {
            const auto now = Time::now(true);
            ev.state_.wakeup_time_.set(rusty::detail::deref_if_pointer_like(now) + rusty::detail::deref_if_pointer_like(timeout));
            rusty::deref_call((rusty::detail::deref_if_pointer_like(reactor_rc)).timeout_events_.borrow_mut(), rusty::detail::__mdisp_push_back{}, rusty::deref_call(ev, rusty::detail::__mdisp_get_self{}).unwrap());
        }
        rusty::detail::deref_if_pointer_like(ev.state_.wp_fiber_.borrow_mut()) = std::conditional_t<true, ::rusty::port::rc::Rc<Fiber>, W>::downgrade(rusty::clone(fiber));
        ev.status_.set(rusty::clone(rusty::clone(EventStatus::WAIT)));
        const auto fiber_status = (rusty::detail::deref_if_pointer_like(fiber)).status_.get();
        verify((rusty::detail::deref_if_pointer_like(fiber_status) != rusty::clone(Fiber::FINISHED)) && (rusty::detail::deref_if_pointer_like(fiber_status) != rusty::clone(Fiber::RECYCLED)));
        ((rusty::detail::deref_if_pointer_like(fiber))).yield_();
    }
}
/*RUSTYCPP:GEN-END id=reactor.13*/



// @safe - verify(), is_ready(), Cell::get/set, Weak::upgrade, Option::is_some
// and Log_debug are all @safe.
// Authored as inline Rust DSL (docs/porting-cpp-to-rust-dsl.md §7.9): duck-typed
// test() machinery as a generic kernel over the concrete event type W (see
// event_wait_impl for the surface contract). Convertible since rusty-cpp #32
// (guard-producing calls on generic receivers → deref dispatch) and #33
// (deref-dispatch functor hoisted to global scope) landed. Param is `ev`, not
// `self` — a free-function param named `self` lowers to a method receiver.
#if RUSTYCPP_RUST
fn event_test_impl<W>(ev: &W) -> bool {
    verify(ev.state_.__debug_creator);
    if ev.is_ready() {
        if ev.status_.get() == EventStatus::INIT {
            ev.status_.set(EventStatus::DONE);
        } else if ev.status_.get() == EventStatus::WAIT {
            if rusty::thread::current_id() == ev.owner_thread_ {
                // Owner-thread-only: upgrading the weak fiber ref mutates a plain
                // (non-atomic) Rc strong count; doing this from a foreign thread
                // races the owner's own Rc<Fiber> clones and corrupts the count.
                // The upgraded handle is used only for this liveness assertion.
                let option_fiber = ev.state_.wp_fiber_.borrow().upgrade();
                verify(option_fiber.is_some());
                verify(ev.status_.get() != EventStatus::DEBUG);
            }
            ev.status_.set(EventStatus::READY);
        } else if ev.status_.get() == EventStatus::READY {
            Log_debug("event status ready, triggered?");
        } else if ev.status_.get() == EventStatus::DONE {
            // do nothing
        } else if ev.status_.get() == EventStatus::TIMEOUT {
            // do nothing
        } else {
            verify(0);
        }
        return true;
    } else {
        if ev.status_.get() == EventStatus::DONE {
            ev.status_.set(EventStatus::INIT);
        }
    }
    false
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.12 version=1 rust_sha256=6b878e0df1b9513246de90e18b8e92999fa8a1faf47c7d256d8c6274fbd92b95*/
template<typename W>
bool event_test_impl(const W& ev);

template<typename W>
bool event_test_impl(const W& ev) {
    verify(ev.state_.__debug_creator);
    if (rusty::deref_call(ev, rusty::detail::__mdisp_is_ready{})) {
        if (ev.status_.get() == rusty::clone(EventStatus::INIT)) {
            ev.status_.set(rusty::clone(rusty::clone(EventStatus::DONE)));
        } else if (ev.status_.get() == rusty::clone(EventStatus::WAIT)) {
            if (rusty::thread::current_id() == rusty::detail::deref_if_pointer_like(ev.owner_thread_)) {
                const auto option_fiber = rusty::deref_call(rusty::borrow(ev.state_.wp_fiber_), rusty::detail::__mdisp_upgrade{});
                verify(option_fiber.is_some());
                verify(ev.status_.get() != rusty::clone(EventStatus::DEBUG));
            }
            ev.status_.set(rusty::clone(rusty::clone(EventStatus::READY)));
        } else if (ev.status_.get() == rusty::clone(EventStatus::READY)) {
            Log_debug("event status ready, triggered?");
        } else if (ev.status_.get() == rusty::clone(EventStatus::DONE)) {
        } else if (ev.status_.get() == rusty::clone(EventStatus::TIMEOUT)) {
        } else {
            verify(0);
        }
        return true;
    } else {
        if (ev.status_.get() == rusty::clone(EventStatus::DONE)) {
            ev.status_.set(rusty::clone(rusty::clone(EventStatus::INIT)));
        }
    }
    return false;
}
/*RUSTYCPP:GEN-END id=reactor.12*/



// Flattened-struct factories (declared next to event_make): each
// replicates the legacy Event constructor's seeding — wait_place_ tag and
// the creating-fiber capture — on top of the aggregate's zero state, plus
// the type's own defaults. Field order matches the DSL struct exactly.
uint64_t event_core_get_fiber_id() {
  auto fiber_opt = Fiber::current_fiber();
  verify(fiber_opt.is_some());
  return fiber_opt.unwrap()->id.get();
}

void event_state_seed(const EventState& st) {
  (*st.wait_place_.borrow_mut()) = "not recorded";
  auto fiber_opt = Fiber::current_fiber();
  if (fiber_opt.is_some()) {
    auto rc_fiber = fiber_opt.unwrap();
    (*st.wp_fiber_.borrow_mut()) = ::rusty::port::rc::Rc<Fiber>::downgrade(rc_fiber);
  }
}

// Authored as inline Rust DSL. The default `rusty::sync::Weak<T>()` that
// used to need a C++ helper is spellable now (§7.30 table); the
// `event_state_seed(sp.state_)` emission carries a std::move around the
// field access, which is fine because event_state_seed takes
// `const EventState&` (reactor.cpp:276) and a const ref binds an rvalue.
#if RUSTYCPP_RUST
fn never_event_make() -> Arc<NeverEvent> {
    let sp = rusty::Arc::<NeverEvent>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),
        rusty::thread::current_id(),
        EventState {},
        rusty::Cell::<bool>::new(true),
        rusty::sync::Weak::<EventPollable>(),
    );
    event_state_seed(sp.state_);
    return sp;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.19 version=1 rust_sha256=dcf171ad810f03fdb53d9fbbee59cb455e8fd685ecd8e85eee9bf7732dac065e*/
rusty::Arc<NeverEvent> never_event_make() {
    auto sp = rusty::Arc<NeverEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus::INIT))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>());
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.19*/

// Authored as inline Rust DSL (§7.30 table: the default
// `rusty::sync::Weak<T>()` that once needed a C++ helper is spellable now).
// Arc/Cell MUST be `rusty::`-qualified here -- the unqualified forms do not
// resolve in this TU (build-verified, not assumed).
#if RUSTYCPP_RUST
fn timeout_event_make(wait_us: u64) -> Arc<TimeoutEvent> {
    let sp = rusty::Arc::<TimeoutEvent>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),
        rusty::thread::current_id(),
        EventState {},
        rusty::Cell::<bool>::new(true),
        rusty::sync::Weak::<EventPollable>(),
        Time::now(true) + wait_us,
        wait_us,
    );
    event_state_seed(sp.state_);
    return sp;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.20 version=1 rust_sha256=019ef05ec016e8a5f5727a02ea265bd283f4fdcf798a5fc6996be21925f6c9a6*/
rusty::Arc<TimeoutEvent> timeout_event_make(uint64_t wait_us) {
    auto sp = rusty::Arc<TimeoutEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus::INIT))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), Time::now(true) + rusty::detail::deref_if_pointer_like(wait_us), std::move(wait_us));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.20*/

// Authored as inline Rust DSL (§7.30 table: the default
// `rusty::sync::Weak<T>()` that once needed a C++ helper is spellable now).
// Arc/Cell MUST be `rusty::`-qualified here -- the unqualified forms do not
// resolve in this TU (build-verified, not assumed).
#if RUSTYCPP_RUST
fn int_event_make(target: i32) -> Arc<IntEvent> {
    let sp = rusty::Arc::<IntEvent>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),
        rusty::thread::current_id(),
        EventState {},
        rusty::Cell::<bool>::new(true),
        rusty::sync::Weak::<EventPollable>(),
        rusty::Cell::<i32>::new(0i32),
        rusty::Cell::<i32>::new(target),
    );
    event_state_seed(sp.state_);
    return sp;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.21 version=1 rust_sha256=86a25dfd2b425c608b5aff36d8bdc2aec61e0b78096d66f64fd02af0d4456812*/
rusty::Arc<IntEvent> int_event_make(int32_t target) {
    auto sp = rusty::Arc<IntEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus::INIT))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<int32_t>::new_(std::move(target)));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.21*/

// Flattened (S4): the former WaitAny(a, b) ctor, as the aggregate factory
// rrr::event_make dispatches to. Build the child vector, then the aggregate,
// then seed the event-core state.
// Authored as inline Rust DSL. NOTE the pre-seeded block id below: a new
// DSL block in this file auto-numbers into an id an existing block already
// holds ("duplicate inline block id=reactor.22"), and the failed rewrite
// DELETES the hand-written body before erroring. Pre-seeding an explicit
// id avoids the collision — see §7.32.
#if RUSTYCPP_RUST
fn waitany_make(a: rusty::Arc<EventPollable>, b: rusty::Arc<EventPollable>) -> Arc<WaitAny> {
    let mut events: rusty::Vec<rusty::Arc<EventPollable>> =
        rusty::Vec::<rusty::Arc<EventPollable>>::new();
    events.push(a);
    events.push(b);
    let sp = rusty::Arc::<WaitAny>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),
        rusty::thread::current_id(),
        EventState {},
        rusty::Cell::<bool>::new(true),
        rusty::sync::Weak::<EventPollable>(),
        events,
    );
    event_state_seed(sp.state_);
    return sp;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.waitany_make version=1 rust_sha256=d7e9803118eefaea14289895c2a4bbaf7b3917b34f69330505222b903c2a109a*/
rusty::Arc<WaitAny> waitany_make(rusty::Arc<EventPollable> a, rusty::Arc<EventPollable> b) {
    rusty::Vec<rusty::Arc<EventPollable>> events = rusty::Vec<rusty::Arc<EventPollable>>::new_();
    events.push(std::move(a));
    events.push(std::move(b));
    auto sp = rusty::Arc<WaitAny>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus::INIT))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), std::move(events));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.waitany_make*/

// Flattened (S4): the former WaitAll default ctor (empty child list).
// Authored as inline Rust DSL (§7.30 table: the default
// `rusty::sync::Weak<T>()` that once needed a C++ helper is spellable now).
// Arc/Cell MUST be `rusty::`-qualified here -- the unqualified forms do not
// resolve in this TU (build-verified, not assumed).
#if RUSTYCPP_RUST
fn waitall_make() -> Arc<WaitAll> {
    let sp = rusty::Arc::<WaitAll>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),
        rusty::thread::current_id(),
        EventState {},
        rusty::Cell::<bool>::new(true),
        rusty::sync::Weak::<EventPollable>(),
        rusty::RefCell::<rusty::Vec<rusty::Arc<EventPollable>>>(),
    );
    event_state_seed(sp.state_);
    return sp;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.22 version=1 rust_sha256=5863d50f57ab83757887c8720481329e39577b09f1bc7c95d0239e2f2a72ce34*/
rusty::Arc<WaitAll> waitall_make() {
    auto sp = rusty::Arc<WaitAll>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus::INIT))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::RefCell<rusty::Vec<rusty::Arc<EventPollable>>>());
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.22*/

// Flattened (S4): the former WaitAll(const Vec&) ctor.
// Authored as inline Rust DSL. `ev.clone()` in the loop is the faithful
// translation of the C++ `events.push(ev)` over a const& — it lowers to
// rusty::clone(ev), NOT std::move(ev), so the caller's vector is not
// gutted (probe-verified). Pre-seeded block id: see §7.32.
#if RUSTYCPP_RUST
fn waitall_make_from(evs: &rusty::Vec<rusty::Arc<EventPollable>>) -> Arc<WaitAll> {
    let mut events: rusty::Vec<rusty::Arc<EventPollable>> =
        rusty::Vec::<rusty::Arc<EventPollable>>::new();
    events.reserve(evs.len());
    for ev in evs {
        events.push(ev.clone());
    }
    let sp = rusty::Arc::<WaitAll>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),
        rusty::thread::current_id(),
        EventState {},
        rusty::Cell::<bool>::new(true),
        rusty::sync::Weak::<EventPollable>(),
        rusty::RefCell::<rusty::Vec<rusty::Arc<EventPollable>>>(events),
    );
    event_state_seed(sp.state_);
    return sp;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.waitall_make_from version=1 rust_sha256=c9f97353ab92163d2e04ff034a904697585f229ef1864335a574e3c3e594e888*/
rusty::Arc<WaitAll> waitall_make_from(const rusty::Vec<rusty::Arc<EventPollable>>& evs) {
    rusty::Vec<rusty::Arc<EventPollable>> events = rusty::Vec<rusty::Arc<EventPollable>>::new_();
    events.reserve(rusty::len(evs));
    for (auto&& ev : rusty::for_in(rusty::iter(evs))) {
        events.push(rusty::clone(ev));
    }
    auto sp = rusty::Arc<WaitAll>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus::INIT))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::RefCell<rusty::Vec<rusty::Arc<EventPollable>>>(std::move(events)));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.waitall_make_from*/



int shared_int_event_set(SharedIntEvent& self, const int& v) {
  auto ret = self.value_;
  self.value_ = v;
  for (auto& ev : self.events_) {
    if (ev->status_.get() <= EventStatus::WAIT) {
      if (ev->target_.get() <= v) {
        ev->set(v);
      }
    }
  }
  return ret;
}

// @unsafe - holds a raw `IntEvent*` (`ev_ptr = ev.get()`) across the
// retain() lambda capture to identity-compare against shared_ptr<IntEvent>
// entries in `events_`. The shared_ptr keeps the target alive for the
// duration of the call.
bool shared_int_event_wait_until_gte(SharedIntEvent& self, int x, int timeout) {
  if (self.value_ >= x) {
    return false;
  }
  auto ev =  reactor_create_sp_event<IntEvent>();
  ev->value_.set(self.value_);
  ev->target_.set(x);
  self.events_.push(ev);
  ev->wait_timeout(timeout);
  // verify(ev->status_.get() != EventStatus::TIMEOUT);  // why can't it be timeout?
  // remove the event from event vector after it entering a terminate state (READY or TIMEOUT)
  bool if_timeout = (ev->status_.get() == EventStatus::TIMEOUT);
  auto* ev_ptr = ev.get();
  self.events_.retain(rusty::Function<bool(const rusty::Arc<IntEvent>&)>(
      [ev_ptr](const rusty::Arc<IntEvent>& item) {
        return item.get() != ev_ptr;
      }));
  return if_timeout;
}

void shared_int_event_wait(SharedIntEvent& self, EventTestFn f) {
  if (f(self.value_)) {
    return;
  }
  auto ev =  reactor_create_sp_event<IntEvent>();
  ev->value_.set(self.value_);
  (*ev->state_.test_.borrow_mut()) = std::move(f);
  self.events_.push(ev);
//  ev->wait(1000*1000*1000);
//  verify(ev->status_ != EventStatus::TIMEOUT);
  ev->wait();
}


// --- from fiber_impl.cc --------------------------------------------------
thread_local uint64_t Fiber::global_id = 0;

// @safe - Trivial member-initializer ctor; std::move + post-increment of
// a thread-local uint64_t. Cells/RefCells default-construct via class
// initializers above; func_ takes a moved-in rusty::Function.
Fiber::Fiber(rusty::Function<void()> func)
    : status_(INIT),
      needs_finalize_(false),
      func_(std::move(func)),
      fiber_task_(rusty::None),
      id(Fiber::global_id++) {
}

// @safe - Empty dtor; rusty::Box / rusty::RefCell members release on drop.
Fiber::~Fiber() {
  // rusty::Box automatically handles cleanup
//  verify(0);
}

void Fiber::run_wrapper(fiber_yield_t& yield) {
  fiber_yield_.set(&yield);
  verify(static_cast<bool>(*func_.borrow()));
  auto reactor = Reactor::get_reactor();
  while (true) {
    auto sz = reactor->fibers_.borrow()->size();  // std::set::size
    verify(sz > 0);
    verify(static_cast<bool>(*func_.borrow()));
    (*func_.borrow_mut())();  // borrow_mut needed because operator() is non-const
    *func_.borrow_mut() = {};
    status_.set(FINISHED);
    if (needs_finalize_.get()) {
      Log_info("Warning: We did not deal with backlog issues");
      needs_finalize_.set(false);
    }
    auto reactor = Reactor::get_reactor();
    reactor->n_active_fibers_.set(reactor->n_active_fibers_.get() - 1);
    fiber_yield_invoke(yield);
  }
}

// @safe - Initializes and starts a fiber
// SAFETY: Single-threaded fiber execution, no concurrent mutation.
// Uses @unsafe blocks for: RefCell operations, get_reactor, STL, const_cast, std::bind, fiber runtime calls.
void Fiber::run() const {
  // @unsafe
  {
    verify((*fiber_task_.borrow()).is_none());
    verify(status_.get() == INIT);
    status_.set(STARTED);
    auto reactor = Reactor::get_reactor();
    auto sz = reactor->fibers_.borrow()->size();  // std::set::size
    verify(sz > 0);
    auto task = std::bind(&Fiber::run_wrapper, const_cast<Fiber*>(this), std::placeholders::_1);
    *fiber_task_.borrow_mut() = rusty::Some(rusty::make_box<fiber_task_t>(std::move(task)));
#ifdef USE_FIBER_RUNTIME1
    (*(*fiber_task_.borrow()).as_ref().unwrap())();
#endif
  }
}

// @safe - Yields control back to the reactor
// SAFETY: Single-threaded fiber execution
void Fiber::yield_() const {
  // @unsafe
  {
    auto* yield_ptr = fiber_yield_.get();
    verify(yield_ptr != nullptr);
    auto s = status_.get();
    verify(s == STARTED || s == RESUMED || s == FINALIZING);
    status_.set(PAUSED);
    {
      auto reactor = Reactor::get_reactor();
      reactor->n_active_fibers_.set(reactor->n_active_fibers_.get() - 1);
    }
    fiber_yield_invoke(*yield_ptr);
  }
}

// @safe - Resumes a paused fiber
// SAFETY: Single-threaded fiber execution
void Fiber::continue_() const {
  // @unsafe
  {
    auto s = status_.get();
    verify(s == PAUSED || s == RECYCLED);
    verify((*fiber_task_.borrow()).is_some());
    status_.set(RESUMED);
    (*(*fiber_task_.borrow_mut()).as_mut().unwrap())();
  }
  // some events might have been triggered from last fiber,
  // but you have to manually call the scheduler to loop.
}

// @safe - Reads Cell<Status>::get() and returns a bool.
bool Fiber::finished() const {
  auto s = status_.get();
  return s == FINISHED || s == RECYCLED;
}

// @safe - One Cell<bool>::set call.
void Fiber::do_finalize() {
  // Handle finalization logic if needed
  needs_finalize_.set(false);
}


// --- from reactor.cc -----------------------------------------------------

// `REUSING_FIBER` is provided as a macro by reactor.h (line 203).
// The original module-attached `constexpr bool REUSING_FIBER`
// shadowed the macro inside the rrr module's purview; with
// de-modularization (header-textual inclusion) the macro now
// expands at parse time and the constexpr is redundant — the
// existing call sites in this TU (lines below) consume the macro
// directly.

namespace {

inline bool stackless_profile_enabled() {
  static bool enabled = []() {
    const char* env = std::getenv("MAKO_ASYNC_PROFILE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

// Type aliases — the DSL grammar can't parse `std::atomic<...>` itself,
// so we hide the template behind typedefs (same pattern as Server's
// `ServerPendingRequestsAtomic`). C++20 guarantees `std::atomic<T>{}`
// zero-initializes integer T, so the previous brace-init `{0}` is the
// same as default-construction; the DSL aggregate emit relies on that.
using StacklessProfileCountU64 = std::atomic<uint64_t>;
using StacklessProfileCountUsize = std::atomic<size_t>;

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Pure POD bag of profile counters. The per-field `.load()` /
// `.fetch_add()` / `.compare_exchange_weak()` calls live in the
// surrounding free functions (`stackless_profile_update_max_slots`,
// `stackless_profile_report_periodic`, and the dispatcher hot-path
// callers); the CAS loop on `max_slots` was the reason this struct
// was previously trivial-blocked. Moving the struct itself into a
// DSL block (with the helpers staying as plain C++ free functions
// against the global) clears that — the DSL emit keeps the same
// memory layout as the original brace-init form.
#if RUSTYCPP_RUST
struct StacklessProfileCounters {
    reg_calls: StacklessProfileCountU64,
    reg_scan_steps: StacklessProfileCountU64,
    reg_reuse: StacklessProfileCountU64,
    reg_new: StacklessProfileCountU64,
    poll_calls: StacklessProfileCountU64,
    poll_ready: StacklessProfileCountU64,
    enqueue_calls: StacklessProfileCountU64,
    max_slots: StacklessProfileCountUsize,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.stackless_profile version=1 rust_sha256=89b7d7978e54ae9761a6ce9cd806b5f954a010c626df7683c7b74daac49f9502*/
struct StacklessProfileCounters;

struct StacklessProfileCounters {
    StacklessProfileCountU64 reg_calls;
    StacklessProfileCountU64 reg_scan_steps;
    StacklessProfileCountU64 reg_reuse;
    StacklessProfileCountU64 reg_new;
    StacklessProfileCountU64 poll_calls;
    StacklessProfileCountU64 poll_ready;
    StacklessProfileCountU64 enqueue_calls;
    StacklessProfileCountUsize max_slots;
};
/*RUSTYCPP:GEN-END id=reactor.stackless_profile*/

StacklessProfileCounters g_stackless_profile;

// Shim for the DSL enqueue path (declared above the Reactor DSL block).
void stackless_profile_note_enqueue() {
  if (stackless_profile_enabled()) {
    g_stackless_profile.enqueue_calls.fetch_add(1, std::memory_order_relaxed);
  }
}

inline void stackless_profile_update_max_slots(size_t slots) {
  size_t old = g_stackless_profile.max_slots.load(std::memory_order_relaxed);
  while (slots > old &&
         !g_stackless_profile.max_slots.compare_exchange_weak(
             old, slots, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

// @unsafe - FUNCTION-LOCAL STATIC (`static thread_local uint64_t
// last_report_us`, §7.24b) plus raw std::atomic / std::memory_order on the
// profile counters. Both keep this out of the DSL.
inline void stackless_profile_report_periodic() {
  if (!stackless_profile_enabled()) {
    return;
  }
  static thread_local uint64_t last_report_us = 0;
  uint64_t now_us = Time::now(true);
  if (last_report_us == 0) {
    last_report_us = now_us;
    return;
  }
  if (now_us - last_report_us < 1000000) {
    return;
  }
  last_report_us = now_us;

  uint64_t reg_calls = g_stackless_profile.reg_calls.load(std::memory_order_relaxed);
  uint64_t reg_scans = g_stackless_profile.reg_scan_steps.load(std::memory_order_relaxed);
  uint64_t reg_reuse = g_stackless_profile.reg_reuse.load(std::memory_order_relaxed);
  uint64_t reg_new = g_stackless_profile.reg_new.load(std::memory_order_relaxed);
  uint64_t poll_calls = g_stackless_profile.poll_calls.load(std::memory_order_relaxed);
  uint64_t poll_ready = g_stackless_profile.poll_ready.load(std::memory_order_relaxed);
  uint64_t enqueue_calls = g_stackless_profile.enqueue_calls.load(std::memory_order_relaxed);
  size_t max_slots = g_stackless_profile.max_slots.load(std::memory_order_relaxed);

  double avg_scan = (reg_calls > 0) ? static_cast<double>(reg_scans) / static_cast<double>(reg_calls) : 0.0;
  Log_info("[async-prof] reg_calls={} avg_scan={:.2f} reuse={} new={} max_slots={} poll_calls={} poll_ready={} enqueue_calls={}",
           static_cast<unsigned long long>(reg_calls),
           avg_scan,
           static_cast<unsigned long long>(reg_reuse),
           static_cast<unsigned long long>(reg_new),
           max_slots,
           static_cast<unsigned long long>(poll_calls),
           static_cast<unsigned long long>(poll_ready),
           static_cast<unsigned long long>(enqueue_calls));
}

}  // namespace

// sp_reactor_th_ / sp_disk_reactor_th_ / sp_running_fiber_th_ are
// `static inline thread_local` in the class declaration above (vague linkage).
// Same for g_current_poll_worker, clients_, and dangling_ips_.

// @safe - Returns current fiber with single-threaded reference counting
// SAFETY: Returns copy of thread-local Rc - single-threaded, no synchronization needed
// Returns None if called outside of a fiber context
rusty::Option<rusty::Rc<Fiber>> Fiber::current_fiber() {
  // @unsafe - RefCell::borrow, Rc::clone
  {
    auto guard = sp_running_fiber_th_.borrow();
    if ((*guard).is_none()) {
      return rusty::None;
    }
    return rusty::Some((*guard).as_ref().unwrap().clone());
  }
}

// @unsafe - Creates and runs a new fiber with rusty::Rc ownership
rusty::Rc<Fiber>
Fiber::create_run_impl(rusty::Function<void()> func, const char* file, int64_t line) {
  auto reactor_rc = Reactor::get_reactor();
  // Rc gives const access, create_run_fiber is const (safe: thread-local, single owner)
  auto fiber = reactor_create_run_fiber_at_impl(*reactor_rc, std::move(func), file, line);
  // some events might be triggered in the last fiber.
  return fiber;
}

void Fiber::sleep(uint64_t microseconds) {
  if (microseconds == 0) {
    return;
  }
  auto x = reactor_create_sp_event<TimeoutEvent>(microseconds);
  x->wait();
}

/**
 * @safe - Returns thread-local reactor instance, creates if needed
 *
 * SAFETY CONTRACT:
 * 1. Thread-Local Storage: sp_reactor_th_ is thread_local, ensuring no data races.
 * 2. Lazy Initialization: Reactor created on first access per thread.
 * 3. Thread Pinning: thread_id_ set on creation, verified in Loop().
 * 4. Reference Counting: Rc<Reactor> returned, safe to clone within same thread.
 *
 * POSTCONDITIONS:
 * - Returns valid Rc<Reactor> pinned to current thread
 * - Reactor's thread_id_ matches rusty::thread::current_id()
 */
// @unsafe - Rc<Reactor> allocation + the create-time log lines (kept
// as kernels: Rc::<T>::make turbofish adjacent to a Log_* call
// mis-lowers the log as a member of the turbofish expression).
rusty::Rc<Reactor> reactor_make() { return rusty::Rc<Reactor>::make(); }
void reactor_log_create(bool disk) {
    if (disk) { Log_debug("create a disk fiber scheduler"); return; }
    Log_debug("create a fiber scheduler");
    if (!REUSING_FIBER) { Log_warn("reusing fiber not enabled!"); }
}

// Singleton fetch-or-init for the per-thread schedulers, authored as
// inline Rust DSL over the namespace-scope TLS slots; the members
// below are 1-line shims.
#if RUSTYCPP_RUST
fn reactor_tls_get() -> rusty::Rc<Reactor> {
    if sp_reactor_th_.is_none() {
        reactor_log_create(false);
        let mut r = reactor_make();
        (*r).thread_id_.set(rusty::thread::current_id());
        sp_reactor_th_ = rusty::Some(r);
    }
    sp_reactor_th_.as_ref().unwrap().clone()
}

fn reactor_tls_get_disk() -> rusty::Rc<Reactor> {
    if sp_disk_reactor_th_.is_none() {
        reactor_log_create(true);
        let mut r = reactor_make();
        (*r).thread_id_.set(rusty::thread::current_id());
        sp_disk_reactor_th_ = rusty::Some(r);
    }
    sp_disk_reactor_th_.as_ref().unwrap().clone()
}

fn reactor_tls_save_running() -> rusty::Option<rusty::Rc<Fiber>> {
    let guard = sp_running_fiber_th_.borrow();
    if (*guard).is_some() {
        return rusty::Some((*guard).as_ref().unwrap().clone());
    }
    None
}

fn reactor_tls_restore_running(old_fiber: rusty::Option<rusty::Rc<Fiber>>) {
    let mut guard = sp_running_fiber_th_.borrow_mut();
    *guard = old_fiber;
}

fn reactor_tls_set_running(fiber: &rusty::Rc<Fiber>) {
    let mut guard = sp_running_fiber_th_.borrow_mut();
    *guard = rusty::Some(fiber.clone());
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.tls_singletons version=1 rust_sha256=c58f69aa33122664c9f9897f7d877a2215ab95bd74fa27ba19b4590b753e702f*/
rusty::Rc<Reactor> reactor_tls_get() {
    if (sp_reactor_th_.is_none()) {
        reactor_log_create(false);
        auto r = reactor_make();
        (rusty::detail::deref_if_pointer_like(r)).thread_id_.set(rusty::thread::current_id());
        sp_reactor_th_ = rusty::Some(std::move(r));
    }
    return rusty::clone(sp_reactor_th_.as_ref().unwrap());
}

rusty::Rc<Reactor> reactor_tls_get_disk() {
    if (sp_disk_reactor_th_.is_none()) {
        reactor_log_create(true);
        auto r = reactor_make();
        (rusty::detail::deref_if_pointer_like(r)).thread_id_.set(rusty::thread::current_id());
        sp_disk_reactor_th_ = rusty::Some(std::move(r));
    }
    return rusty::clone(sp_disk_reactor_th_.as_ref().unwrap());
}

rusty::Option<rusty::Rc<Fiber>> reactor_tls_save_running() {
    auto&& guard = rusty::borrow(sp_running_fiber_th_);
    if (((rusty::detail::deref_if_pointer_like(guard))).is_some()) {
        return rusty::Option<rusty::Rc<Fiber>>(rusty::clone(((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap()));
    }
    return rusty::Option<rusty::Rc<Fiber>>{rusty::None};
}

void reactor_tls_restore_running(rusty::Option<rusty::Rc<Fiber>> old_fiber) {
    auto&& guard = sp_running_fiber_th_.borrow_mut();
    rusty::detail::deref_if_pointer_like(guard) = std::move(old_fiber);
}

void reactor_tls_set_running(const rusty::Rc<Fiber>& fiber) {
    auto&& guard = sp_running_fiber_th_.borrow_mut();
    rusty::detail::deref_if_pointer_like(guard) = rusty::Option<rusty::Rc<Fiber>>(rusty::clone(fiber));
}
/*RUSTYCPP:GEN-END id=reactor.tls_singletons*/

// =============================================================================
// Helper functions for create_run_fiber
// =============================================================================

// @safe -Gets a recycled fiber or creates a new one
rusty::Rc<Fiber> reactor_get_or_create_fiber_impl(const Reactor& self, rusty::Function<void()> func, const char* file, int64_t line) {
  // @unsafe
  {
    auto available_guard = self.available_fibers_.borrow_mut();
    if (REUSING_FIBER && available_guard->size() > 0) {
      self.n_idle_fibers_.set(self.n_idle_fibers_.get() - 1);
      auto fiber = available_guard->back().clone();
      available_guard->pop();
      // Use Cell/RefCell for interior mutability (safe: single-threaded)
      const auto& fiber_ref = *fiber;
      fiber_ref.id.set(Fiber::global_id++);
      *fiber_ref.func_.borrow_mut() = std::move(func);
      // Keep the existing task/stack so continue_() can resume from the fiber's yield point.
      verify((*fiber_ref.fiber_task_.borrow()).is_some());
      fiber_ref.status_.set(Fiber::RECYCLED);
      return fiber;
    } else {
      auto fiber = rusty::Rc<Fiber>::make(std::move(func));
      self.n_created_fibers_.set(self.n_created_fibers_.get() + 1);
      if (self.n_created_fibers_.get() % 1024 == 0) {
        Log_debug("created {}, busy {}, idle {} fibers on server {}, recent {}:{}",
                 (int)self.n_created_fibers_.get(),
                 (int)self.n_busy_fibers_.get(),
                 (int)self.n_idle_fibers_.get(),
                 self.server_id_.get(),
                 file,
                 (long long)line);
      }
      return fiber;
    }
  }
}

// @safe - 1-line shims into the DSL TLS helpers above.
// @safe -Registers a fiber in the active set

// @unsafe - Queue a stackless task slot for polling if not already queued.

// @unsafe - Register a stackless task poller and return slot index.
size_t reactor_register_stackless_poller_impl(const Reactor& self, rusty::Function<bool(rusty::Context&)> poller) {
  size_t scanned = 0;
  {
    auto free_guard = self.free_stackless_task_slots_.borrow_mut();
    if (!free_guard->is_empty()) {
      size_t idx = free_guard->back();
      free_guard->pop();
      auto tasks_guard = self.stackless_tasks_.borrow_mut();
      if (idx < tasks_guard->size()) {
        auto& entry = (*tasks_guard)[idx];
        entry.active = true;
        entry.queued = false;
        entry.poll_once = std::move(poller);
        if (stackless_profile_enabled()) {
          g_stackless_profile.reg_calls.fetch_add(1, std::memory_order_relaxed);
          g_stackless_profile.reg_scan_steps.fetch_add(scanned, std::memory_order_relaxed);
          g_stackless_profile.reg_reuse.fetch_add(1, std::memory_order_relaxed);
        }
        return idx;
      }
    }
  }

  auto tasks_guard = self.stackless_tasks_.borrow_mut();
  StacklessTaskEntry entry{true, false, std::move(poller)};
  tasks_guard->push(std::move(entry));
  if (stackless_profile_enabled()) {
    g_stackless_profile.reg_calls.fetch_add(1, std::memory_order_relaxed);
    g_stackless_profile.reg_scan_steps.fetch_add(scanned, std::memory_order_relaxed);
    g_stackless_profile.reg_new.fetch_add(1, std::memory_order_relaxed);
    stackless_profile_update_max_slots(tasks_guard->size());
  }
  return tasks_guard->size() - 1;
}

// @unsafe - Poll all queued stackless tasks once.
bool reactor_process_stackless_tasks_impl(const Reactor& self) {
  bool did_work = false;
  for (;;) {
    size_t idx = 0;
    {
      auto ready_guard = self.ready_stackless_tasks_.borrow_mut();
      if (ready_guard->is_empty()) {
        break;
      }
      idx = (*ready_guard)[0];
      ready_guard->pop_front();
    }

    // Move the poll function out of its slot before invoking it; rusty::Function
    // is move-only so we can't keep a copy in-place. Reactor is single-threaded,
    // so any synchronous waker callback during poll only mutates queued/active
    // flags (never poll_once), and we put the function back below if the poll
    // didn't complete the task.
    rusty::Function<bool(rusty::Context&)> poll_fn;
    {
      auto tasks_guard = self.stackless_tasks_.borrow_mut();
      if (idx >= tasks_guard->size()) {
        continue;
      }
      auto& entry = (*tasks_guard)[idx];
      entry.queued = false;
      if (!entry.active || !entry.poll_once) {
        continue;
      }
      poll_fn = std::move(entry.poll_once);
    }

    did_work = true;
    if (stackless_profile_enabled()) {
      g_stackless_profile.poll_calls.fetch_add(1, std::memory_order_relaxed);
    }
    rusty::Waker waker{[rp = &self, idx]() {
      rp->enqueue_stackless_task(idx);
    }};
    rusty::Context ctx{&waker};
    bool ready = poll_fn(ctx);

    {
      auto tasks_guard = self.stackless_tasks_.borrow_mut();
      if (idx < tasks_guard->size()) {
        auto& entry = (*tasks_guard)[idx];
        if (ready) {
          if (stackless_profile_enabled()) {
            g_stackless_profile.poll_ready.fetch_add(1, std::memory_order_relaxed);
          }
          entry.active = false;
          entry.queued = false;
          entry.poll_once = {};
          self.free_stackless_task_slots_.borrow_mut()->push(idx);
        } else {
          // Put the function back so the next poll iteration can fire it.
          entry.poll_once = std::move(poll_fn);
        }
      }
    }
  }
  stackless_profile_report_periodic();
  return did_work;
}

// =============================================================================
// Main create_run_fiber - orchestrates the helper functions
// =============================================================================

/**
 * @param func
 * @return
 */
// @safe - Creates and runs a fiber using safe helper functions
// KERNEL by verdict (reactor slice 2b): orchestration dominated by
// Rc<Fiber> arrow-method calls (run/continue_/finished/status) where
// the DSL's last-use move-insertion mis-handles the repeatedly-passed
// Rc, plus Reactor being a hand-written class (a DSL `self` param
// emits `this->` with no receiver). Converting would need per-call
// clone-guards + a member-shim dance for zero borrow-check gain.
rusty::Rc<Fiber> reactor_create_run_fiber_impl(const Reactor& self, rusty::Function<void()> func) {
  return reactor_create_run_fiber_at_impl(self, std::move(func), "", 0);
}

rusty::Rc<Fiber> reactor_create_run_fiber_at_impl(const Reactor& self, rusty::Function<void()> func, const char* file, int64_t line) {
  // Step 1: Get or create a fiber
  auto fiber = reactor_get_or_create_fiber_impl(self, std::move(func), file, line);

  // @unsafe
  {
    self.n_busy_fibers_.set(self.n_busy_fibers_.get() + 1);
  }

  // Step 2: Save current running fiber context (for nesting)
  auto old_fiber = self.save_running_fiber();

  // Step 3: Set this as the running fiber
  self.set_running_fiber(fiber);

  // Step 4: Register in the active fibers set
  self.register_fiber(fiber);

  // Step 5: Run the fiber
  // @unsafe
  {
    auto status = fiber->status_.get();
    if (status == Fiber::INIT) {
      fiber->run();
    } else {
      verify(status == Fiber::RECYCLED);
      fiber->continue_();
    }
    if (fiber->finished()) {
      self.recycle(fiber);
    }
  }

  // Step 6: Process events
  // @unsafe
  {
    reactor_run_loop_impl(self, false, true);
  }

  // Step 7: Restore previous running fiber
  self.restore_running_fiber(std::move(old_fiber));

  return fiber;
}

// @unsafe - Uses RefCell::borrow_mut (not borrow-checked)
// KERNEL by verdict: first pass derefs shared_ptr<EventPollable> to
// virtual-dispatch status()/wakeup_time()/is_ready() (arrow wall), and
// extract_if/retain take rusty::Function predicates that themselves
// cross the sp-> arrow — all-kernel body, no separable DSL policy.

// @unsafe - shared_ptr::use_count + rusty::Function in the retain predicate.
// Amortized cleanup of `all_events_`: drop events the list is the sole owner of
// (use_count()==1 → no fiber/waiter/other shared_ptr holds them, so they are
// finished) and that opted into pruning. Throttled by a moving high-water mark
// so the O(n) sweep runs ~O(1) amortized per create_sp_event. Runs on the
// reactor thread (single-threaded ownership), and cross-thread signalers reach
// events via the weak_ptr `self_`, so freeing a sole-owned event is safe.
// @unsafe - FUNCTION-LOCAL STATIC (`static thread_local size_t prune_hwm`).
// The DSL has no construct for a static declared inside a function body
// (§7.24b); hoisting it would change the per-thread high-water semantics.
void reactor_prune_finished_events_impl(const Reactor& self) {
  static thread_local std::size_t prune_hwm = 64;
  auto guard = self.all_events_.borrow_mut();
  if (guard->len() < prune_hwm) {
    return;
  }
  guard->retain(rusty::Function<bool(const rusty::Arc<EventPollable>&)>(
    [](const rusty::Arc<EventPollable>& e) {
      return e.strong_count() > 1 || !e->prunable();
    }));
  prune_hwm = guard->len() * 2 + 64;
}
void reactor_run_loop_impl(const Reactor& self, bool infinite, bool do_check_timeout) {
  verify(rusty::thread::current_id() == self.thread_id_.get());

  self.looping_.set(infinite);

  do {
    bool found_ready_events = true;
    while (found_ready_events) {
      found_ready_events = false;
      if (reactor_process_stackless_tasks_impl(self)) {
        found_ready_events = true;
      }
      rusty::VecDeque<rusty::Arc<EventPollable>> ready_events;

      // Process waiting events using RefCell
      {
        auto waiting_guard = self.waiting_events_.borrow_mut();
        // Test waiting events
        for (size_t i = 0; i < waiting_guard->len(); ++i) {
          (*waiting_guard)[i]->test();
        }
        // Extract READY events
        {
          auto ready_from_waiting = waiting_guard->extract_if(
            rusty::Function<bool(const rusty::Arc<EventPollable>&)>(
              [](const rusty::Arc<EventPollable>& ev) {
                return ev->status() == EventStatus::READY;
              }));
          if (!ready_from_waiting.is_empty()) {
            ready_events.append(std::move(ready_from_waiting));
            found_ready_events = true;
          }
        }
        // Remove DONE events
        {
          waiting_guard->retain(
            rusty::Function<bool(const rusty::Arc<EventPollable>&)>(
              [](const rusty::Arc<EventPollable>& ev) {
                return ev->status() != EventStatus::DONE;
              }));
        }
      }

      // Process composite events using RefCell
      {
        auto composite_guard = self.composite_events_.borrow_mut();
        for (size_t i = 0; i < composite_guard->len(); ++i) {
          (*composite_guard)[i]->test();
        }
        {
          auto ready_from_composite = composite_guard->extract_if(
            rusty::Function<bool(const rusty::Arc<EventPollable>&)>(
              [](const rusty::Arc<EventPollable>& ev) {
                return ev->status() == EventStatus::READY;
              }));
          if (!ready_from_composite.is_empty()) {
            ready_events.append(std::move(ready_from_composite));
            found_ready_events = true;
          }
        }
        {
          composite_guard->retain(
            rusty::Function<bool(const rusty::Arc<EventPollable>&)>(
              [](const rusty::Arc<EventPollable>& ev) {
                return ev->status() != EventStatus::DONE;
              }));
        }
      }

      // Check timeouts using RefCell-based check_timeout
      if (do_check_timeout) {
        size_t before = ready_events.len();
        // @unsafe { check_timeout is per-method @unsafe due to raw
        // std::shared_ptr<Event> handling + Status::TIMEOUT mutation. }
        { self.check_timeout(ready_events); }
        if (ready_events.len() > before) {
          found_ready_events = true;
        }
      }

      // Process ready events
      // @unsafe - Weak::upgrade, continue_fiber with potential use-after-move patterns
      {
        for (size_t i = 0; i < ready_events.len(); ++i) {
          auto& ev = ready_events[i];
          if (ev->status() == EventStatus::DONE) {
            continue;
          }
          auto option_fiber = ev->upgrade_fiber();
          if (option_fiber.is_none()) {
            continue;
          }
          auto fiber = option_fiber.unwrap();
          if (!self.fibers_.borrow()->contains(fiber)) {
            continue;
          }
          verify(fiber->status_.get() == Fiber::PAUSED);
          if (ev->status() == EventStatus::READY) {
            ev->set_status(EventStatus::DONE);
          } else {
            verify(ev->status() == EventStatus::TIMEOUT);
          }
          self.continue_fiber(fiber);
        }
      }

      if (!infinite && !found_ready_events) {
        break;
      }
    }

  } while (self.looping_.get());
}

// @unsafe - Continues execution of a paused fiber; RefCell ops and fiber calls.
// Takes the Rc by const reference: passing by value would invoke the port
// Rc's defaulted (shallow, non-incrementing) copy constructor, creating an
// uncounted alias that double-decrements the strong count on destruction and
// frees a still-referenced fiber. We clone() internally where ownership is
// actually needed.
// KERNEL by verdict: dense RefCell borrow guards (named-guard binding
// emits address-of-temporary) around Rc<Fiber> arrow calls; same walls
// as create_run_fiber.

// @unsafe - Uses RefCell interior mutability (rusty-cpp doesn't fully support RefCell semantics)

// @unsafe - Spawn a stackless task and schedule first poll on this reactor.
void reactor_spawn_stackless_task_impl(const Reactor& self, rusty::Task<void> task) {
  verify(rusty::thread::current_id() == self.thread_id_.get());
  constexpr size_t kUnregisteredSlot = std::numeric_limits<size_t>::max();
  // @unsafe - mutable atomic fields are storage for cross-thread
  // wake-state mutations from the early_waker lambda. The struct is
  // local to this method body and does not inherit Reactor's
  // class-level @safe in intent — the rusty-cpp mutable-field rule
  // fires here because libclang qualifies local types under the
  // enclosing class scope.
  struct EarlyWakeState {
    explicit EarlyWakeState(const Reactor* reactor_ptr) : reactor(reactor_ptr) {}
    const Reactor* reactor;
    mutable std::atomic<size_t> idx{kUnregisteredSlot};
    mutable std::atomic<bool> pending_wake{false};
  };

  auto early_wake = reactor_make_arc<EarlyWakeState>(&self);

  rusty::Waker early_waker{[early_wake, kUnregisteredSlot]() {
    size_t idx = early_wake->idx.load(std::memory_order_acquire);
    if (idx == kUnregisteredSlot) {
      early_wake->pending_wake.store(true, std::memory_order_release);
      return;
    }
    early_wake->reactor->enqueue_stackless_task(idx);
  }};
  rusty::Context early_ctx{&early_waker};
  auto early_poll = task.poll(early_ctx);
  if (early_poll.is_ready()) {
    return;
  }

  // @unsafe - mutable Task field is needed because the registered
  // poller closure must call `task.poll(ctx)` which mutates the Task,
  // and the closure receives `TaskState` by const Arc.
  struct TaskState {
    mutable rusty::Task<void> task;
    rusty::Arc<EarlyWakeState> early_wake;

    TaskState(rusty::Task<void> t, rusty::Arc<EarlyWakeState> ew)
        : task(std::move(t)), early_wake(std::move(ew)) {}
  };

  auto state = reactor_make_arc<TaskState>(std::move(task), std::move(early_wake));
  auto idx = reactor_register_stackless_poller_impl(self, [state](rusty::Context& ctx) mutable {
    auto poll_result = state->task.poll(ctx);
    if (poll_result.is_ready()) {
      state->early_wake->idx.store(kUnregisteredSlot, std::memory_order_release);
      return true;
    }
    return false;
  });
  state->early_wake->idx.store(idx, std::memory_order_release);
  if (state->early_wake->pending_wake.exchange(false, std::memory_order_acq_rel)) {
    self.enqueue_stackless_task(idx);
  }
}

// =============================================================================
// PollThreadWorker Implementation
// =============================================================================

// Kernel-internal forward decls (definition order below is legacy).
void pollworker_process_commands(PollThreadWorker& self);
void pollworker_trigger_job(PollThreadWorker& self);
void pollworker_process_pending_removals(PollThreadWorker& self);
void pollworker_do_add_pollable(PollThreadWorker& self, PollableProxy poll);
// 1-line arrow kernels for the DSL bodies (Box-trait dispatch wall).
int  pollable_proxy_fd(const PollableProxy& p);
int  pollable_proxy_mode(const PollableProxy& p);
rusty::Vec<int> pollworker_take_removals(PollThreadWorker& self);
void pollworker_close_proxy_of(PollThreadWorker& self, int fd);
void pollworker_do_remove_pollable(PollThreadWorker& self, int fd);
void pollworker_do_close_pollable(PollThreadWorker& self, int fd);
void pollworker_do_update_mode(PollThreadWorker& self, int fd, int new_mode);
void pollworker_do_add_job(PollThreadWorker& self, rusty::Arc<Job> job);
void pollworker_do_remove_job(PollThreadWorker& self, rusty::Arc<Job> job);

// (ctor folded into an aggregate factory; no eventfd needed — the
// channel is polled with try_recv() after each epoll_wait.)
static PollThreadWorker pollworker_make(PollCmdReceiver receiver) {
  return PollThreadWorker{std::move(receiver), Epoll(),        FdPollableMap(),
                          FdModeMap(),         FdSet(),        JobSet(),
                          false};
}

// @unsafe - factory function creates worker and wraps in Rc<RefCell> (rustycpp false positive on move)
rusty::Rc<rusty::RefCell<PollThreadWorker>> pollworker_create(PollCmdReceiver receiver) {
  // Create worker, then wrap in RefCell
  auto worker = pollworker_make(std::move(receiver));
  return rusty::Rc<rusty::RefCell<PollThreadWorker>>::make(std::move(worker));
}

void pollworker_poll_loop(PollThreadWorker& self) {
  Log_debug("[poll_loop] Starting poll loop");
  while (!self.stop_) {
    pollworker_trigger_job(self);

    // Wait for events (epoll_wait with short timeout)
    // Dispatch through proxy storage by fd; no Pollable* userdata assumptions.
    self.poll_.Wait([&self](int fd, int ready_events) {
      auto poll_opt = self.fd_to_pollable_.get(fd);
      if (poll_opt.is_none()) {
        return;
      }
      auto& poll = poll_opt.unwrap();

      if (ready_events & PollReady::READABLE) {
        poll->handle_read();
      }
      if (ready_events & PollReady::WRITABLE) {
        int new_mode = poll->handle_write();
        if (new_mode != PollMode::NO_CHANGE) {
          pollworker_do_update_mode(self, fd, new_mode);
        }
      }
      if (ready_events & PollReady::ERROR) {
        poll->handle_error();
      }
    });

    // Process commands from channel (non-blocking try_recv)
    pollworker_process_commands(self);

    pollworker_trigger_job(self);

    // Process deferred removals
    pollworker_process_pending_removals(self);

    pollworker_trigger_job(self);
    Reactor::get_reactor()->run_loop(false, true);

    // Check for pending write updates (set by end_reply() during fiber execution)
    // @unsafe - reads a pollable's interior-mutable pending_write_update_
    // flag through the shared Arc; no cast is involved.
    for (auto [fd, poll] : self.fd_to_pollable_) {
      if (poll->check_pending_write_update()) {
        pollworker_do_update_mode(self, fd, PollMode::READ | PollMode::WRITE);
      }
    }

    // Check for pollables closed by handle_error() and remove them
    // This prevents fd reuse issues when old connection is closed but not removed
    rusty::Vec<int> closed_fds;
    for (auto [fd, poll] : self.fd_to_pollable_) {
      if (poll->is_closed()) {
        closed_fds.push(fd);
      }
    }
    for (int fd : closed_fds) {
      auto proxy_opt = self.fd_to_pollable_.get(fd);
      if (proxy_opt.is_some()) {
        // Remove from epoll if still registered
        if (self.mode_.contains_key(fd)) {
          self.poll_.Remove(fd);
        }

        // Invoke close callback before erasing map entry so cleanup hooks run.
        // HashMap::get now returns Option<V&>; unwrap() is already the
        // PollableProxy reference, no extra deref.
        proxy_opt.unwrap()->close();

        self.fd_to_pollable_.remove(fd);
        self.mode_.remove(fd);
      }
    }
  }

  Log_debug("[poll_loop] Exited while loop (self.stop_=true), starting cleanup");
  // Shutdown cleanup - remove all registered pollables
  for (auto [fd, poll] : self.fd_to_pollable_) {
    if (self.mode_.contains_key(fd)) {
      self.poll_.Remove(fd);
    }
  }
  self.fd_to_pollable_.clear();
  self.mode_.clear();
  self.pending_remove_.clear();
  Log_debug("[poll_loop] Cleanup complete, poll_loop exiting");
}

// @unsafe - calls try_recv and std::visit
void pollworker_process_commands(PollThreadWorker& self) {
  // Non-blocking receive: process all pending commands
  int cmd_count = 0;
  while (true) {
    auto result = self.receiver_.try_recv();
    if (result.is_err()) {
      // Empty or disconnected - either way, stop processing
      break;
    }
    cmd_count++;
    auto cmd = result.unwrap();
    std::visit([&self](auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, CmdAddPollable>) {
        pollworker_do_add_pollable(self, std::move(arg.pollable));
      } else if constexpr (std::is_same_v<T, CmdRemovePollable>) {
        pollworker_do_remove_pollable(self, arg.fd);
      } else if constexpr (std::is_same_v<T, CmdClosePollable>) {
        pollworker_do_close_pollable(self, arg.fd);
      } else if constexpr (std::is_same_v<T, CmdUpdateMode>) {
        pollworker_do_update_mode(self, arg.fd, arg.new_mode);
      } else if constexpr (std::is_same_v<T, CmdAddJob>) {
        pollworker_do_add_job(self, std::move(arg.job));
      } else if constexpr (std::is_same_v<T, CmdRemoveJob>) {
        pollworker_do_remove_job(self, std::move(arg.job));
      } else if constexpr (std::is_same_v<T, CmdShutdown>) {
        self.stop_ = true;
      }
    }, cmd);
  }
}

// @safe - rusty::BTreeSet::clone/clear/insert and rusty::Arc are @safe;
// only the raw `Job*` extraction + virtual dispatch escapes into inner
// @unsafe blocks.
void pollworker_trigger_job(PollThreadWorker& self) {
  // Copy jobs to process (in case jobs modify the set).
  std::set<rusty::Arc<Job>> jobs_exec = self.jobs_;
  self.jobs_.clear();

  for (const auto& job : jobs_exec) {
    bool ready;
    // @unsafe { const_cast<Job*> + virtual Ready() dispatch }
    {
      Job* job_ptr = const_cast<Job*>(job.get());
      ready = job_ptr->Ready();
    }
    if (ready) {
      // Capture job by value to keep the Arc alive.
      Fiber::create_run([job]() {
        // @unsafe { const_cast<Job*> + virtual Work() dispatch }
        {
          Job* job_ptr = const_cast<Job*>(job.get());
          job_ptr->Work();
        }
      });
      // Don't re-add ready jobs that were executed.
    } else {
      // Re-add jobs that aren't ready yet - they should be checked again later.
      self.jobs_.insert(job);
    }
  }
}

// The poll-worker command handlers — registration policy, deferred
// removal, close, interest updates, job set — as inline Rust DSL. The
// Box-trait arrows (fd/poll_mode/close through PollableProxy) are
// 1-line kernels; Epoll::Add/Remove/Update are GEN methods.
#if RUSTYCPP_RUST
fn pollworker_do_add_pollable(w: &mut PollThreadWorker, poll: PollableProxy) {
    let fd = pollable_proxy_fd(poll);
    let poll_mode = pollable_proxy_mode(poll);

    // The pollable can close between CmdAddPollable being enqueued and
    // processed (teardown racing an accept/connect registration): fd is
    // then -1 and registering would abort inside Epoll::Add. A closed
    // pollable can never produce events — drop it.
    if fd < 0 {
        return;
    }
    if w.fd_to_pollable_.contains_key(fd) {
        return;
    }
    w.fd_to_pollable_.insert(fd, poll);
    w.mode_.insert(fd, poll_mode);
    // Add fails (-1) on the EBADF teardown race — drop the dead
    // pollable again.
    if w.poll_.Add(fd, poll_mode) != 0 {
        w.fd_to_pollable_.remove(fd);
        w.mode_.remove(fd);
    }
}

fn pollworker_do_remove_pollable(w: &mut PollThreadWorker, fd: i32) {
    if !w.fd_to_pollable_.contains_key(fd) {
        return;
    }
    // Deferred: actual removal happens after epoll_wait.
    w.pending_remove_.insert(fd);
}

fn pollworker_do_close_pollable(w: &mut PollThreadWorker, fd: i32) {
    w.pending_remove_.remove(fd);
    if !w.fd_to_pollable_.contains_key(fd) {
        return;
    }
    if w.mode_.contains_key(fd) {
        w.poll_.Remove(fd);
    }
    // Virtual close through the proxy (arrow kernel: unwrap would copy
    // the move-only Box).
    pollworker_close_proxy_of(w, fd);
    w.fd_to_pollable_.remove(fd);
    w.mode_.remove(fd);
}

fn pollworker_do_update_mode(w: &mut PollThreadWorker, fd: i32, new_mode: i32) {
    if !w.fd_to_pollable_.contains_key(fd) {
        return;
    }
    let mode_opt = w.mode_.get(fd);
    if mode_opt.is_none() {
        return;
    }
    let old_mode = mode_opt.unwrap();
    w.mode_.insert(fd, new_mode);
    if new_mode != old_mode {
        w.poll_.Update(fd, new_mode, old_mode);
    }
}

fn pollworker_do_add_job(w: &mut PollThreadWorker, job: rusty::Arc<Job>) {
    w.jobs_.insert(job);
}

fn pollworker_do_remove_job(w: &mut PollThreadWorker, job: rusty::Arc<Job>) {
    w.jobs_.erase(job);
}

fn pollworker_process_pending_removals(w: &mut PollThreadWorker) {
    // take-to-Vec kernel: the HashSet rejects the rusty::iter shim.
    let remove_fds = pollworker_take_removals(w);
    let mut i: usize = 0;
    while i < remove_fds.len() {
        let fd = remove_fds[i];
        if w.fd_to_pollable_.contains_key(fd) {
            // fd not reused (still in the mode map) => unregister.
            if w.mode_.contains_key(fd) {
                w.poll_.Remove(fd);
            }
            w.fd_to_pollable_.remove(fd);
            w.mode_.remove(fd);
        }
        i += 1;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.pollworker_cmds version=1 rust_sha256=195fbe036a1b4f3475eded34b645d6f80a489acd017d532eb8c0bc7faebb7c4f*/
void pollworker_do_add_pollable(PollThreadWorker& w, PollableProxy poll) {
    const auto fd = pollable_proxy_fd(std::move(poll));
    auto poll_mode = pollable_proxy_mode(std::move(poll));
    if (rusty::detail::deref_if_pointer_like(fd) < 0) {
        return;
    }
    if (w.fd_to_pollable_.contains_key(std::move(fd))) {
        return;
    }
    w.fd_to_pollable_.insert(std::move(fd), std::move(poll));
    w.mode_.insert(std::move(fd), std::move(poll_mode));
    if (w.poll_.Add(std::move(fd), std::move(poll_mode)) != 0) {
        w.fd_to_pollable_.remove(std::move(fd));
        w.mode_.remove(std::move(fd));
    }
}

void pollworker_do_remove_pollable(PollThreadWorker& w, int32_t fd) {
    if (rusty::detail::rust_not(w.fd_to_pollable_.contains_key(std::move(fd)))) {
        return;
    }
    w.pending_remove_.insert(std::move(fd));
}

void pollworker_do_close_pollable(PollThreadWorker& w, int32_t fd) {
    w.pending_remove_.remove(std::move(fd));
    if (rusty::detail::rust_not(w.fd_to_pollable_.contains_key(std::move(fd)))) {
        return;
    }
    if (w.mode_.contains_key(std::move(fd))) {
        w.poll_.Remove(std::move(fd));
    }
    pollworker_close_proxy_of(w, std::move(fd));
    w.fd_to_pollable_.remove(std::move(fd));
    w.mode_.remove(std::move(fd));
}

void pollworker_do_update_mode(PollThreadWorker& w, int32_t fd, int32_t new_mode) {
    if (rusty::detail::rust_not(w.fd_to_pollable_.contains_key(std::move(fd)))) {
        return;
    }
    auto mode_opt = w.mode_.get(std::move(fd));
    if (mode_opt.is_none()) {
        return;
    }
    const auto old_mode = mode_opt.unwrap();
    w.mode_.insert(std::move(fd), std::move(new_mode));
    if (rusty::detail::deref_if_pointer_like(new_mode) != rusty::detail::deref_if_pointer_like(old_mode)) {
        w.poll_.Update(std::move(fd), std::move(new_mode), std::move(old_mode));
    }
}

void pollworker_do_add_job(PollThreadWorker& w, rusty::Arc<Job> job) {
    w.jobs_.insert(std::move(job));
}

void pollworker_do_remove_job(PollThreadWorker& w, rusty::Arc<Job> job) {
    w.jobs_.erase(std::move(job));
}

void pollworker_process_pending_removals(PollThreadWorker& w) {
    const auto remove_fds = pollworker_take_removals(w);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(remove_fds)) {
        const auto fd = remove_fds[i];
        if (w.fd_to_pollable_.contains_key(std::move(fd))) {
            if (w.mode_.contains_key(std::move(fd))) {
                w.poll_.Remove(std::move(fd));
            }
            w.fd_to_pollable_.remove(std::move(fd));
            w.mode_.remove(std::move(fd));
        }
        i += 1;
    }
}
/*RUSTYCPP:GEN-END id=reactor.pollworker_cmds*/

// @unsafe - Box-trait arrow dispatch (the 1-line kernels the DSL calls).
int pollable_proxy_fd(const PollableProxy& p) { return p->fd(); }
// @unsafe - drains the pending-remove set into an indexable Vec for
// the DSL sweep (the HashSet's range-for has no rusty::iter shim).
rusty::Vec<int> pollworker_take_removals(PollThreadWorker& self) {
    rusty::Vec<int> v;
    for (int fd : self.pending_remove_) {
        v.push(fd);
    }
    self.pending_remove_.clear();
    return v;
}
int pollable_proxy_mode(const PollableProxy& p) { return p->poll_mode(); }
void pollworker_close_proxy_of(PollThreadWorker& self, int fd) {
    auto proxy_opt = self.fd_to_pollable_.get(fd);
    if (proxy_opt.is_some()) {
        proxy_opt.unwrap()->close();
    }
}

// @safe - Update poll mode directly (bypasses channel); only safe on
// the poll thread. Kernel by verdict: takes the abstract Pollable by
// reference (dyn-trait ref params have no verified DSL spelling) for
// one line of logic.
void pollworker_update_mode(PollThreadWorker& self, Pollable& poll, int new_mode) {
  { pollworker_do_update_mode(self, poll.fd(), new_mode); }
}

// =============================================================================
// PollThread Implementation
// =============================================================================


// @safe - ThreadId<->u64 bit_cast helpers. `platform::threading::thread_id`
// is `std::thread::id` (default backend) or `pthread_t` (POSIX backend).
// Both are 8-byte trivially copyable on the platforms we support; the
// static_assert below makes the bit_cast safe.
namespace {
inline std::uint64_t thread_id_to_u64(rusty::thread::ThreadId tid) noexcept {
    using NativeId = decltype(tid.as_native());
    static_assert(sizeof(NativeId) == sizeof(std::uint64_t),
                  "platform thread_id must be 8 bytes for bit_cast to u64");
    static_assert(std::is_trivially_copyable_v<NativeId>,
                  "platform thread_id must be trivially copyable");
    return std::bit_cast<std::uint64_t>(tid.as_native());
}

} // namespace

// @unsafe - takes address-of an atomic field (`&arc->poll_thread_id_bits_`)
// and passes the raw pointer into a spawned thread closure. The Arc
// keeps the PollThread (and thus the atomic) alive until the worker
// thread finishes; rusty-cpp can't express that lifetime relationship.
rusty::Arc<PollThread> pollthread_create() {
  // Create MPSC channel
  auto [sender, receiver] = rusty::sync::mpsc::channel<PollCommand>();

  // Movable-atomics aggregate route (no private ctor / friend Arc):
  auto arc = rusty::Arc<PollThread>::new_(PollThread{
      std::move(sender),
      PollJoinSlot(rusty::None),
      rusty::sync::atomic::AtomicU64(0),
      rusty::sync::atomic::AtomicBool(false)});

  // Pointer to atomic thread ID for safe cross-thread access (rusty
  // Atomic ops are const, so a const* suffices through the Arc).
  const rusty::sync::atomic::AtomicU64* thread_id_ptr = &arc->poll_thread_id_bits_;

  // Spawn thread - worker owns the receiver
  auto handle = rusty::thread::spawn(
    [thread_id_ptr](rusty::sync::mpsc::Receiver<PollCommand> rx) {
      auto tid = rusty::thread::current_id();
      thread_id_ptr->store(thread_id_to_u64(tid), rusty::sync::atomic::Ordering::Release);
      // Create worker wrapped in Rc<RefCell<>>
      auto worker = PollThreadWorker::create(std::move(rx));
      // Store raw pointer in TLS for direct access from same thread
      // The borrow_mut guard keeps RefCell borrowed during poll_loop()
      // Using raw pointer avoids RefCell re-borrow issues in fibers
      auto guard = worker->borrow_mut();
      g_current_poll_worker = &*guard;
      guard->poll_loop();
      g_current_poll_worker = nullptr;  // Clear on exit
    },
    std::move(receiver)
  );

  // Store handle
  {
    auto guard = arc->join_handle_.lock().unwrap();
    *guard = rusty::Some(std::move(handle));
  }

  return arc;
}

// The PollThread drop body, authored as inline Rust DSL: gettid via a
// route-2 unsafe{} syscall (SYS_gettid is a macro identifier that
// lowers as-is), int-arg Log_debug, and the shutdown() method call on
// the by-ref PollThread (non-`self` param name so it emits pt.method,
// not this->).
#if RUSTYCPP_RUST
fn pollthread_drop(pt: &PollThread) {
    let tid: i64 = unsafe { syscall(SYS_gettid) };
    Log_debug("[PollThread::~PollThread] Destructor called from TID={}", tid as i32);
    pt.shutdown();
    Log_debug("[PollThread::~PollThread] Destructor complete");
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.pollthread_drop version=1 rust_sha256=7e16d65337680fee3f4e12decbc58faaf2f1a2143c44a83a053a8644c330056c*/
void pollthread_drop(const PollThread& pt) {
    const int64_t tid = syscall(SYS_gettid);
    Log_debug("[PollThread::~PollThread] Destructor called from TID={}", static_cast<int32_t>(tid));
    pt.shutdown();
    Log_debug("[PollThread::~PollThread] Destructor complete");
}
/*RUSTYCPP:GEN-END id=reactor.pollthread_drop*/






// @safe - Sends update mode command via channel (send wrapped @unsafe)
// SAFETY: Channel send is thread-safe



// --- from fiber_context_runtime.cc --------------------------------------

// fiber_swap_context is implemented in arch-specific files:
//   fiber_context_x86_64.cc  (x86_64)
//   fiber_context_aarch64.cc (AArch64/ARM64)

thread_local fiber_task_t* fiber_task_t::tls_active_task_ = nullptr;

// Authored as inline Rust DSL. The raw `fiber_task_t*` deref lowers fine —
// the older comment claiming the transpiler could not translate it was
// stale (§7.30). The parameter is `y`, not `self`: a DSL param named
// `self` becomes a receiver and would emit a METHOD, which would not
// match `friend void fiber_yield_invoke(fiber_yield_t&)`. Parameter names
// are not part of a C++ signature, so friendship still applies.
//
// `!y.task_.is_null()` rather than `y.task_ != nullptr`: the latter emits
// a non-existent `nullptr_` (§7.31).
#if RUSTYCPP_RUST
fn fiber_yield_invoke(y: &mut fiber_yield_t) {
    verify(!y.task_.is_null());
    unsafe { (*y.task_).yield_to_caller(); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.23 version=1 rust_sha256=edfdd5edd7499fdf2a71159254c03890da9bb1e50bcedb96879b8ffbe6a05662*/
void fiber_yield_invoke(fiber_yield_t& y);

void fiber_yield_invoke(fiber_yield_t& y) {
    verify(rusty::detail::rust_not((y.task_ == nullptr)));
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(y.task_))).yield_to_caller();
    }
}
/*RUSTYCPP:GEN-END id=reactor.23*/

fiber_task_t::fiber_task_t(TaskFn fn)
    : fn_(std::move(fn)),
      yield_(fiber_yield_t::new_(*this)) {
  init_context();
  // Match Boost.Coroutine2 pull_type behavior: run immediately on construction.
  resume();
}

fiber_task_t::~fiber_task_t() {
  if (stack_mapping_ != nullptr) {
    int rc = munmap(stack_mapping_, stack_mapping_bytes_);
    verify(rc == 0);
    stack_mapping_ = nullptr;
    stack_mapping_bytes_ = 0;
  }
}

void fiber_task_t::operator()() {
  resume();
}

// @unsafe - mmap stack region, install guard page via mprotect,
// reinterpret_cast the trampoline address and stack-top into the
// ABI-specific FiberContext (rsp/rip on x86_64, sp/pc on aarch64).
// The whole body is raw-pointer arithmetic by design.
void fiber_task_t::init_context() {
  std::size_t page_sz =
      static_cast<std::size_t>(rusty::sys::process::sysconf(_SC_PAGESIZE));
  if (page_sz == 0) {
    page_sz = 4096;
  }

  stack_mapping_bytes_ = kDefaultStackBytes + page_sz;
  void* mapping = mmap(nullptr,
                       stack_mapping_bytes_,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);
  verify(mapping != MAP_FAILED);
  int protect_rc = mprotect(mapping, page_sz, PROT_NONE);
  verify(protect_rc == 0);
  stack_mapping_ = mapping;

  std::uintptr_t stack_top =
      reinterpret_cast<std::uintptr_t>(static_cast<char*>(stack_mapping_) + stack_mapping_bytes_);
  stack_top &= ~static_cast<std::uintptr_t>(0xF);

  fiber_ctx_ = FiberContext{};
  caller_ctx_ = FiberContext{};
  const auto trampoline_addr =
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(&fiber_task_t::entry_trampoline));
#if defined(__x86_64__)
  // SysV x86_64 ABI: %rsp % 16 == 8 on function entry (simulates a call pushing ret addr).
  stack_top -= sizeof(void*);
  *reinterpret_cast<void**>(stack_top) = nullptr;
  fiber_ctx_.rsp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.rip = trampoline_addr;
#elif defined(__aarch64__)
  // AAPCS64: sp must be 16-byte aligned on function entry. No fake return address needed;
  // the trampoline address is stored in pc (= lr) and entered via ret.
  fiber_ctx_.sp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.pc = trampoline_addr;
#endif
}

// @unsafe - fiber context switch via raw `fiber_task_t*` thread-local
// (`tls_active_task_`) save/restore + `&caller_ctx_`/`&fiber_ctx_`
// address-of into `fiber_swap_context`. The whole call is the fiber-
// switching primitive.
void fiber_task_t::resume() {
  if (state_ == State::FINISHED) {
    return;
  }
  auto* old = tls_active_task_;
  tls_active_task_ = this;
  fiber_swap_context(&caller_ctx_, &fiber_ctx_);
  tls_active_task_ = old;
}

// @unsafe - companion to resume() — `&fiber_ctx_`/`&caller_ctx_` into
// the fiber-switching primitive.
void fiber_task_t::yield_to_caller() {
  verify(state_ == State::RUNNING);
  state_ = State::SUSPENDED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  if (state_ != State::FINISHED) {
    state_ = State::RUNNING;
  }
}

// @unsafe - reads the raw `fiber_task_t*` thread-local set by resume()
// and dispatches into the fiber's entry routine.
void fiber_task_t::entry_trampoline() {
  auto* task = tls_active_task_;
  verify(task != nullptr);
  task->entry();
}

// @unsafe - uses raw `this` for the fiber-finished callback dispatch.
[[noreturn]] void fiber_task_t::entry() {
  state_ = State::RUNNING;
  verify(static_cast<bool>(fn_));
  fn_(yield_);
  state_ = State::FINISHED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  std::abort();
}

}  // namespace rrr (definitions)

// --- from quorum_event.cc ------------------------------------------------
// @safe - QuorumEvent impl. Methods carry per-method annotations.
namespace janus {

using rrr::IntEvent;
using rrr::Fiber;
using rrr::Time;
using rrr::verify;
// EventStatus used to be Event's nested enum (found via base-class scope in
// these member definitions); it now lives at rrr namespace scope (S4 hoist).
using rrr::EventStatus;

// Flattened (S4): the former QuorumEvent(int,int) ctor, as the aggregate
// factory rrr::event_make dispatches to. Build the struct with its field
// defaults, then seed the event-core state (was Event()'s job). finalize_event_
// is Registered via create_sp_event (not bare make) so the event has a live
// self-reference: the finalize fiber's wait() and the vote-side set() push
// get_self() into the reactor queues, which for an unregistered event is null
// (latent crash on the copilot finalize path). Nested create is safe: this runs
// inside the outer create_sp_event's make, BEFORE the outer all_events_ borrow.
rusty::Arc<QuorumEvent> quorum_event_make(int32_t n_total, int32_t quorum) {
  auto sp = rusty::Arc<QuorumEvent>::make(
      rusty::Cell<EventStatus>::new_(EventStatus::INIT),      // status_
      rusty::thread::current_id(),                            // owner_thread_
      EventState{},                                           // state_
      rusty::Cell<bool>::new_(true),                          // prunable_
      rusty::sync::Weak<EventPollable>(),                     // self_
      rusty::Cell<int32_t>::new_(0),                          // n_voted_yes_
      rusty::Cell<int32_t>::new_(0),                          // n_voted_no_
      rusty::RefCell<rusty::HashMap<uint16_t, rrr::i64>>(),   // xids_
      n_total,                                                // n_total_
      quorum,                                                 // quorum_
      rusty::Cell<QuorumPolicy>::new_(QuorumPolicy::DEFAULT), // policy_
      rusty::Cell<bool>::new_(false),                         // committed_seen_
      rusty::Cell<int32_t>::new_(0),                          // num_leader_
      rusty::Cell<int32_t>::new_(0),                          // n_leader_yes_
      rusty::Cell<int32_t>::new_(0),                          // n_leader_no_
      rusty::Cell<int64_t>::new_(0),                          // highest_term_
      rusty::Cell<bool>::new_(false),                         // timeouted_
      rusty::Cell<uint32_t>::new_(0),                         // leader_id_
      rusty::Cell<int64_t>::new_(-1),                         // par_id_
      rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(-1)), // id_
      rrr::reactor_create_sp_event<IntEvent>(n_total));      // finalize_event_
  event_state_seed(sp->state_);
  return sp;
}

// @unsafe - spawns a background fiber whose mutable closure captures the
// move-only finalize_func + a reference to `self`. Faithful port of the former
// QuorumEvent::finalize; only touches `self` (the final_ev clone + the
// dangling_rpc copy-out) BEFORE wait_timeout (see comment A), so the reference
// is safe.
void quorum_event_finalize(
    const QuorumEvent& self, uint64_t timeout,
    QuorumFinalizeFn finalize_func) {
  Fiber::create_run([timeout, finalize_func = std::move(finalize_func), &self]() mutable {
    bool ret = false;

    auto final_ev = self.finalize_event_.clone();  // copy the finalize event (comment A)
    rusty::Vec<std::pair<uint16_t, rrr::i64> > dangling_rpc;
    // borrow_mut (RefCell::borrow_mut is const) — HashMap iteration needs a
    // non-const map; this is a read-only copy-out, no aliasing.
    for (auto it : *self.xids_.borrow_mut())
      dangling_rpc.push(it);  // fetch dangling rpc info before it's freed (comment A)

    final_ev->wait_timeout(timeout);
    /* A: by the time this fires, the quorum event could have been freed. Thus,
     avoid accessing `self` or its members after this line */

    // didn't receive all RPC replies
    if (final_ev->status_.get() == EventStatus::TIMEOUT) {
      ret = finalize_func(dangling_rpc);
      // Drain guard: a TIMEOUT'd event is never evicted by the reactor loop
      // (extract takes READY, retain drops DONE), so a registered
      // finalize_event_ would otherwise linger in the queues forever at
      // broadcast rate. Mark it DONE here (we run on the owner thread) so
      // the next pass evicts and prune can free it.
      final_ev->status_.set(EventStatus::DONE);
    }
    (void)ret;
  }, __FILE__, __LINE__);
}

// @unsafe - reads/clears the reactor's shared slow_ flag (matches the former
// QuorumEvent::is_slow / Event::is_slow); slow_ is public and Reactor is
// complete here. `self` is unused (the flag is reactor-global).
bool quorum_event_is_slow(const QuorumEvent& self) {
  (void)self;
  bool result = Reactor::get_reactor()->slow_.get();
  Reactor::get_reactor()->slow_.set(false);
  return result;
}


}  // namespace janus (definitions)
