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
// The plain-C stackful-fiber engine (Goal-0 fiber-API C demotion).
#include "srpc_fiber.h"
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
#include <stdlib.h>
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
RUSTY_METHOD_DISPATCH(pop)
RUSTY_METHOD_DISPATCH(push_back)
RUSTY_METHOD_DISPATCH(retain)
RUSTY_METHOD_DISPATCH(set_self)
RUSTY_METHOD_DISPATCH(size)
RUSTY_METHOD_DISPATCH(unwrap)
RUSTY_METHOD_DISPATCH(upgrade)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

export namespace janus {
struct QuorumEvent;
rusty::Arc<QuorumEvent> quorum_event_make(int32_t n_total, int32_t quorum);
rusty::Arc<QuorumEvent> create_sp_quorum_event(int32_t n_total, int32_t quorum);
}

export namespace rrr {

// --- from event.h --------------------------------------------------------

class Reactor;
class Fiber;

// Per-thread scheduler singletons + the running-fiber slot. Namespace-
// scope (not class-static) so the DSL singleton/save/restore logic can
// name them; `inline` keeps vague linkage (same clang-21 dup-symbol
// rationale as the former class members).
#if RUSTYCPP_RUST
#[thread_local]
static mut sp_reactor_th_: rusty::Option<rusty::Rc<Reactor>> = rusty::Option::<rusty::Rc<Reactor>>::None;
#[thread_local]
static mut sp_disk_reactor_th_: rusty::Option<rusty::Rc<Reactor>> = rusty::Option::<rusty::Rc<Reactor>>::None;
#[thread_local]
static mut sp_running_fiber_th_: rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> = rusty::RefCell::new(rusty::Option::<rusty::Rc<Fiber>>::None);
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.2 version=1 rust_sha256=b64414e2cb3324a5b183b51a39d90fbba21ef13633609662fdcafff284f3b186*/
extern thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_;
extern thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_;
extern thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_fiber_th_;

inline thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_ = rusty::Option<rusty::Rc<Reactor>>{rusty::None};

inline thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_ = rusty::Option<rusty::Rc<Reactor>>{rusty::None};

inline thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_fiber_th_ = rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>>::new_(rusty::Option<rusty::Rc<Fiber>>{rusty::None});
/*RUSTYCPP:GEN-END id=reactor.2*/


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
// the flat DSL structs each carry a `status_: Cell<EventStatus>`.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The emitted `enum class EventStatus`
// carries no fixed underlying type, i.e. `int` -- the same layout the
// former `: int32_t` spelling gave on every target we build. External call
// sites keep writing `EventStatus::X`; DSL bodies in THIS file now lower to
// the generated `constexpr EventStatus_X()` accessors, which fold away.
#if RUSTYCPP_RUST
#[repr(i32)]
enum EventStatus {
    INIT = 0,
    WAIT = 1,
    READY = 2,
    DONE = 3,
    TIMEOUT = 4,
    DEBUG = 5,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.1 version=1 rust_sha256=76b532362758c9878092bb559507b13a4b0b423e66fa1a5eea58d6d19d3ee38b*/
enum class EventStatus;
constexpr EventStatus EventStatus_INIT();
constexpr EventStatus EventStatus_WAIT();
constexpr EventStatus EventStatus_READY();
constexpr EventStatus EventStatus_DONE();
constexpr EventStatus EventStatus_TIMEOUT();
constexpr EventStatus EventStatus_DEBUG();

enum class EventStatus {
    INIT = 0,
    WAIT = 1,
    READY = 2,
    DONE = 3,
    TIMEOUT = 4,
    DEBUG = 5
};
inline constexpr EventStatus EventStatus_INIT() { return EventStatus::INIT; }
inline constexpr EventStatus EventStatus_WAIT() { return EventStatus::WAIT; }
inline constexpr EventStatus EventStatus_READY() { return EventStatus::READY; }
inline constexpr EventStatus EventStatus_DONE() { return EventStatus::DONE; }
inline constexpr EventStatus EventStatus_TIMEOUT() { return EventStatus::TIMEOUT; }
inline constexpr EventStatus EventStatus_DEBUG() { return EventStatus::DEBUG; }
/*RUSTYCPP:GEN-END id=reactor.1*/

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
// (was a sprintf kernel; format! -> std::format emission retired that)
#if RUSTYCPP_RUST
fn event_core_record_place<W>(self_: &W, file: SrcFileCStr, line: i32) {
    let tag: std::string = format!("{}:{}", file, line);
    let mut g = self_.state_.wait_place_.borrow_mut();
    *g += tag;
    self_.state_.rcd_wait_.set(true);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.4 version=1 rust_sha256=5dbce4daef4c1afe2307d556583eb08549afe1c47478c851c9010d83ece415a6*/
template<typename W>
void event_core_record_place(const W& self_, SrcFileCStr file, int32_t line) {
    const std::string tag = std::format("{}:{}" , file , line);
    auto&& g = self_.state_.wait_place_.borrow_mut();
    rusty::detail::deref_if_pointer_like(g) += tag;
    self_.state_.rcd_wait_.set(true);
}
/*RUSTYCPP:GEN-END id=reactor.4*/
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
// (event_make + its is_box_event/box_event_payload dispatch traits are
//  gone: per-type factories + create_sp_box_event<T> cover every branch.)



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

// Factory + slot ops for BoxEvent<Type>, authored as DSL generic fns
// (fn f<Type> lowers to the same template; §7.58's lb precedent).
// Defined after the struct is complete and in this exported module
// region so deptran's BoxEvent<int>/<bool>/<std::string>
// instantiations resolve. Generic defaults: Default::default() is
// legal in TURBOFISH-call args (the make's RefCell slot) and the
// clear goes through core::mem::take (assignment-position Default
// does not lower).
#if RUSTYCPP_RUST
fn boxevent_make<Type>() -> Arc<BoxEvent<Type>> {
    let sp = rusty::Arc::<BoxEvent<Type>>::make(
        rusty::Cell::<EventStatus>::new(EventStatus::INIT),   // status_
        rusty::thread::current_id(),                          // owner_thread_
        EventState {},                                        // state_
        rusty::Cell::<bool>::new(true),                       // prunable_
        rusty::sync::Weak::<EventPollable>(),                 // self_
        rusty::RefCell::<Type>::new(Default::default()),      // content_
        rusty::Cell::<bool>::new(false),                      // is_set_
    );
    event_state_seed(sp.state_);
    return sp;
}

// Returns the slot payload by value (copy out of the RefCell).
fn boxevent_get<Type>(ev: &BoxEvent<Type>) -> Type {
    let g = ev.content_.borrow();
    (*g).clone()
}

fn boxevent_set<Type>(ev: &BoxEvent<Type>, c: &Type) {
    ev.is_set_.set(true);
    {
        let mut g = ev.content_.borrow_mut();
        *g = c.clone();
    }
    ev.test();
}

fn boxevent_clear<Type>(ev: &BoxEvent<Type>) {
    ev.is_set_.set(false);
    let mut g = ev.content_.borrow_mut();
    let _old = core::mem::take(&mut *g);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.5 version=1 rust_sha256=ff451405ec517e52a229208d518da0b88d55c87938395b0a477a061caf9aae1a*/
template<typename Type>
rusty::Arc<BoxEvent<Type>> boxevent_make() {
    auto sp = rusty::Arc<BoxEvent<Type>>::make(std::conditional_t<true, rusty::Cell<EventStatus>, Type>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, std::conditional_t<true, rusty::Cell<bool>, Type>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::RefCell<Type>::new_(rusty::default_like<Type>()), std::conditional_t<true, rusty::Cell<bool>, Type>::new_(false));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}

template<typename Type>
Type boxevent_get(const BoxEvent<Type>& ev) {
    auto&& g = rusty::borrow(ev.content_);
    return rusty::clone(((rusty::detail::deref_if_pointer_like(g))));
}

template<typename Type>
void boxevent_set(const BoxEvent<Type>& ev, const Type& c) {
    ev.is_set_.set(true);
    {
        auto&& g = ev.content_.borrow_mut();
        rusty::detail::deref_if_pointer_like(g) = rusty::clone(c);
    }
    ev.test();
}

template<typename Type>
void boxevent_clear(const BoxEvent<Type>& ev) {
    ev.is_set_.set(false);
    auto&& g = ev.content_.borrow_mut();
    const auto _old = rusty::mem::take(rusty::detail::deref_if_pointer_like(g));
}
/*RUSTYCPP:GEN-END id=reactor.5*/

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

// Sets value_ and runs the readiness test; returns the previous value
// (verbatim from the legacy IntEvent::set). The custom-predicate check
// is the tcp on_frame guard shape: borrow the Function slot, bool-test
// the guard, invoke through it.
#if RUSTYCPP_RUST
fn int_event_set(ev: &IntEvent, n: i32) -> i32 {
    let t: i32 = ev.value_.get();
    ev.value_.set(n);
    event_test_impl(ev);
    t
}

fn int_event_is_ready(ev: &IntEvent) -> bool {
    let guard = ev.state_.test_.borrow();
    if *guard {
        return (*guard)(ev.value_.get());
    }
    ev.value_.get() >= ev.target_.get()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.6 version=1 rust_sha256=00db42f1c1618921dc9babee7e1e77d45150cb46bf6132044cc5e778a6c5a2c5*/
int32_t int_event_set(const IntEvent& ev, int32_t n) {
    int32_t t = ev.value_.get();
    ev.value_.set(std::move(n));
    event_test_impl(ev);
    return std::move(t);
}

bool int_event_is_ready(const IntEvent& ev) {
    auto&& guard = rusty::borrow(ev.state_.test_);
    if (rusty::detail::deref_if_pointer_like(guard)) {
        return (rusty::detail::deref_if_pointer_like(guard))(ev.value_.get());
    }
    return ev.value_.get() >= ev.target_.get();
}
/*RUSTYCPP:GEN-END id=reactor.6*/

// `SharedIntEvent` — a shared counter that wakes IntEvent waiters when
// it crosses their thresholds. The `rusty::Arc<IntEvent>` element
// type stays std (Reactor::create_sp_event hands out shared_ptr — a
// declared boundary type).

struct SharedIntEvent;

// Backing free fns for the DSL methods below — ALL THREE are DSL now
// (the old "drives create_sp_event — not DSL-expressible" cause expired:
// an explicit template argument on the variadic factory lowers fine).
// Definitions near the bottom of this file; the DSL emits their
// definitions' decls, these forward decls just satisfy the delegating
// methods above them.
int32_t shared_int_event_set(SharedIntEvent& sie, int32_t v);
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

// @safe - Time::now read; strict `>` preserved from the original.
#if RUSTYCPP_RUST
fn timeout_event_is_ready(self_: &TimeoutEvent) -> bool {
    Time::now(true) > self_.wakeup_time_
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.16 version=1 rust_sha256=3d4fe2059a0fc7145fba5145e2ad97b1303ca121aea2851dd3bd6a3785ae3659*/
bool timeout_event_is_ready(const TimeoutEvent& self_) {
    return Time::now(true) > rusty::detail::deref_if_pointer_like(self_.wakeup_time_);
}
/*RUSTYCPP:GEN-END id=reactor.16*/

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
        if (!(((rusty::detail::deref_if_pointer_like(e))).is_ready() || (((rusty::detail::deref_if_pointer_like(e))).status() == rusty::clone(EventStatus_DONE())))) {
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
// (The fiber register bag + swap declaration live in srpc_fiber.h now —
//  the whole stackful engine is plain C in srpc_fiber.c; the .S files'
//  offset contract is against srpc_fiber_ctx.)

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

// Thin C++ holder over the plain-C srpc_fiber engine. The mmap stacks,
// ABI context seeding, TLS active slot, and resume/yield/finish state
// machine all live in srpc_fiber.c; the one irreducible C++ piece is
// fiber_task_entry_thunk, where C re-enters C++ to invoke the
// rusty::Function task body on the fiber stack.
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

  // Invoked (via the extern-C thunk) by the C trampoline on the fiber
  // stack: runs the rusty::Function task body with the yield handle.
  void run_body();

 private:
  friend class fiber_yield_t;
  friend void fiber_yield_invoke(fiber_yield_t& self);

  void yield_to_caller();

  TaskFn fn_;
  fiber_yield_t yield_;
  srpc_fiber fib_{};
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
// Fiber status machine, hoisted to namespace scope (same shape/reason as
// EventStatus). Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block
// below is the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. (The old note here claimed a
// DSL-defined enum "hits the variant-call trap" -- disproven: the emitted
// form is a plain `enum class` plus `constexpr FiberStatus_X()` accessors,
// and the default underlying type is `int`, matching the former
// `: int32_t`.) `using enum FiberStatus` inside the class keeps the
// historical `Fiber::INIT` spellings valid, and those paths are NOT
// rewritten by the transpiler because `Fiber` is not the enum's name.
#if RUSTYCPP_RUST
#[repr(i32)]
enum FiberStatus {
    INIT = 0,
    STARTED = 1,
    PAUSED = 2,
    RESUMED = 3,
    FINISHED = 4,
    FINALIZING = 5,
    RECYCLED = 6,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.18 version=1 rust_sha256=6007c4fe40b7ba6273f1da0e7fb8fb540b5af01d5dfbcfdb0ac391b7a02baf3c*/
enum class FiberStatus;
constexpr FiberStatus FiberStatus_INIT();
constexpr FiberStatus FiberStatus_STARTED();
constexpr FiberStatus FiberStatus_PAUSED();
constexpr FiberStatus FiberStatus_RESUMED();
constexpr FiberStatus FiberStatus_FINISHED();
constexpr FiberStatus FiberStatus_FINALIZING();
constexpr FiberStatus FiberStatus_RECYCLED();

enum class FiberStatus {
    INIT = 0,
    STARTED = 1,
    PAUSED = 2,
    RESUMED = 3,
    FINISHED = 4,
    FINALIZING = 5,
    RECYCLED = 6
};
inline constexpr FiberStatus FiberStatus_INIT() { return FiberStatus::INIT; }
inline constexpr FiberStatus FiberStatus_STARTED() { return FiberStatus::STARTED; }
inline constexpr FiberStatus FiberStatus_PAUSED() { return FiberStatus::PAUSED; }
inline constexpr FiberStatus FiberStatus_RESUMED() { return FiberStatus::RESUMED; }
inline constexpr FiberStatus FiberStatus_FINISHED() { return FiberStatus::FINISHED; }
inline constexpr FiberStatus FiberStatus_FINALIZING() { return FiberStatus::FINALIZING; }
inline constexpr FiberStatus FiberStatus_RECYCLED() { return FiberStatus::RECYCLED; }
/*RUSTYCPP:GEN-END id=reactor.18*/

// The per-thread fiber id counter (was Fiber::global_id, a static
// thread_local member; hoisted to namespace TLS — the
// g_current_poll_worker precedent) plus its post-increment kernel for
// the ctor's id stamp.
#if RUSTYCPP_RUST
#[thread_local]
static mut g_fiber_global_id: u64 = 0;
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.45 version=1 rust_sha256=578336901e3e008135ed0798dfa190e5fec457cbe86db0b99903b646757a7041*/
extern thread_local uint64_t g_fiber_global_id;

inline thread_local uint64_t g_fiber_global_id = static_cast<uint64_t>(0);
/*RUSTYCPP:GEN-END id=reactor.45*/
// @unsafe { post-increments the namespace-scope thread_local counter }
#if RUSTYCPP_RUST
fn fiber_next_global_id() -> u64 {
    let r = g_fiber_global_id;
    g_fiber_global_id = r + 1u64;
    r
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.41 version=1 rust_sha256=a6fa4669ca1a3b0235ef99b539927802208c96aaca605a9ac58cbe8495b221f1*/
uint64_t fiber_next_global_id();

uint64_t fiber_next_global_id() {
    auto r = std::move(g_fiber_global_id);
    g_fiber_global_id = rusty::detail::deref_if_pointer_like(r) + static_cast<uint64_t>(1);
    return std::move(r);
}
/*RUSTYCPP:GEN-END id=reactor.41*/

// DSL-support aliases: the grammar cannot spell fn-type template args.
using FiberFn = rusty::Function<void()>;
using FiberTaskFn = rusty::Function<void(fiber_yield_t&)>;

// Fiber -- converted to inline-Rust DSL (Goal 0). The whole class shell
// is GEN now: the fields, the `#[cpp_ctor]` member-initializer
// constructor (so `rusty::Rc<Fiber>::make(func)` keeps working
// unchanged), the `create_run<Func>` member template, and every method
// declaration. The method BODIES still live below Reactor as `fiber_*`
// DSL free fns, because Fiber and Reactor mutually recurse; the GEN
// methods delegate through the forward declarations directly below --
// the same cycle-breaking split Reactor's `reactor_*_impl` kernels use.
//
// Both historical blockers expired:
//   - a DSL generic method DOES lower to a real member template, so
//     `create_run<Func>` survives. Rust has no default arguments, so the
//     `file`/`line` defaults are gone: the call sites that DID pass
//     `__FILE__, __LINE__` now call `create_run_impl` directly, which
//     keeps their provenance exactly. (The tracker's claim that all 89
//     sites pass exactly one argument was WRONG -- four pass three.)
//   - `using Status = FiberStatus; using enum FiberStatus;` had no
//     external consumers -- the only four `Fiber::INIT`-style spellings
//     in the tree were DSL bodies in this file.
//
// One member could not follow: `operator<` over rusty::Rc<Fiber> is a
// free operator on a FOREIGN type; it is the 3-line kernel below the GEN.

// The DSL fiber free fns the GEN Fiber methods delegate to; bodies live
// below Reactor.
rusty::Option<rusty::Rc<Fiber>> fiber_current_fiber();
rusty::Rc<Fiber> fiber_create_run_impl(FiberFn func, SrcFileCStr file, int64_t line);
void fiber_sleep(uint64_t microseconds);
void fiber_run(const Fiber& fb);
void fiber_do_yield(const Fiber& fb);
void fiber_do_continue(const Fiber& fb);
bool fiber_is_finished(const Fiber& fb);

#if RUSTYCPP_RUST
struct Fiber {
    dep_id_: u64,
    need_finalize_: bool,
    id: rusty::Cell<u64>,
    status_: rusty::Cell<FiberStatus>,
    needs_finalize_: rusty::Cell<bool>,
    func_: rusty::RefCell<FiberFn>,
    fiber_task_: rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>,
    fiber_yield_: rusty::Cell<*mut fiber_yield_t>,
    // Fiber hands its own `this` to the C stack-switching engine, so a
    // move would leave the engine pointing at the old address. The old
    // hand-written class got that guarantee for free: its empty
    // `~Fiber()` was a user-declared destructor, which SUPPRESSED the
    // implicit move operations. A DSL struct has no destructor, so the
    // move ctor would come back -- inert today (nothing holds a Fiber by
    // value; Rc::make placement-news), but it would silently permit
    // `Fiber b = std::move(a);` and dangle the engine. `_pin` makes the
    // transpiler emit DELETED move operations instead, which is the same
    // guarantee the destructor used to provide, stated on purpose.
    // Same precedent as Reactor above.
    _pin: rusty::marker::PhantomPinned,
}

impl Fiber {
    #[cpp_ctor]
    fn new(func: FiberFn) -> Fiber {
        Fiber {
            dep_id_: 0u64,
            need_finalize_: false,
            id: rusty::Cell::<u64>::new_(fiber_next_global_id()),
            status_: rusty::Cell::<FiberStatus>::new_(FiberStatus::INIT),
            needs_finalize_: rusty::Cell::<bool>::new_(false),
            func_: rusty::RefCell::<FiberFn>::new_(func),
            fiber_task_: Default::default(),
            fiber_yield_: rusty::Cell::<*mut fiber_yield_t>::new_(core::ptr::null_mut()),
            _pin: rusty::marker::PhantomPinned {},
        }
    }

    fn current_fiber() -> rusty::Option<rusty::Rc<Fiber>> {
        fiber_current_fiber()
    }

    fn create_run<Func>(func: Func) -> rusty::Rc<Fiber> {
        Fiber::create_run_impl(func, "", 0i64)
    }

    fn create_run_impl(func: FiberFn, file: SrcFileCStr, line: i64) -> rusty::Rc<Fiber> {
        fiber_create_run_impl(func, file, line)
    }

    fn sleep(microseconds: u64) {
        fiber_sleep(microseconds);
    }

    fn run(&self) {
        fiber_run(self);
    }

    fn yield_(&self) {
        fiber_do_yield(self);
    }

    fn continue_(&self) {
        fiber_do_continue(self);
    }

    fn finished(&self) -> bool {
        fiber_is_finished(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.64 version=1 rust_sha256=56860ce89827ee50b45f160880f8109db49b5142ebd7b17eba35f80594f1e5a5*/
struct Fiber;

struct Fiber {
    uint64_t dep_id_;
    bool need_finalize_;
    rusty::Cell<uint64_t> id;
    rusty::Cell<FiberStatus> status_;
    rusty::Cell<bool> needs_finalize_;
    rusty::RefCell<FiberFn> func_;
    rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>> fiber_task_;
    rusty::Cell<fiber_yield_t*> fiber_yield_;
    rusty::marker::PhantomPinned _pin;

    Fiber(FiberFn func);
    static rusty::Option<rusty::Rc<Fiber>> current_fiber();
    template<typename Func>
    static rusty::Rc<Fiber> create_run(Func func);
    static rusty::Rc<Fiber> create_run_impl(FiberFn func, SrcFileCStr file, int64_t line);
    static void sleep(uint64_t microseconds);
    void run() const;
    void yield_() const;
    void continue_() const;
    bool finished() const;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;
};


Fiber::Fiber(FiberFn func)
    : dep_id_(static_cast<uint64_t>(0))
    , need_finalize_(false)
    , id(rusty::Cell<uint64_t>::new_(fiber_next_global_id()))
    , status_(rusty::Cell<FiberStatus>::new_(rusty::clone(rusty::clone(FiberStatus_INIT()))))
    , needs_finalize_(rusty::Cell<bool>::new_(false))
    , func_(rusty::RefCell<FiberFn>::new_(std::move(func)))
    , fiber_task_(rusty::default_like<rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>>())
    , fiber_yield_(rusty::Cell<fiber_yield_t*>::new_(rusty::ptr::null_mut()))
    , _pin(rusty::marker::PhantomPinned{})
{}

rusty::Option<rusty::Rc<Fiber>> Fiber::current_fiber() {
    return fiber_current_fiber();
}

template<typename Func>
rusty::Rc<Fiber> Fiber::create_run(Func func) {
    return Fiber::create_run_impl(std::move(func), "", static_cast<int64_t>(0));
}

rusty::Rc<Fiber> Fiber::create_run_impl(FiberFn func, SrcFileCStr file, int64_t line) {
    return fiber_create_run_impl(std::move(func), std::move(file), std::move(line));
}

void Fiber::sleep(uint64_t microseconds) {
    fiber_sleep(std::move(microseconds));
}

void Fiber::run() const {
    fiber_run((*this));
}

void Fiber::yield_() const {
    fiber_do_yield((*this));
}

void Fiber::continue_() const {
    fiber_do_continue((*this));
}

bool Fiber::finished() const {
    return fiber_is_finished((*this));
}
/*RUSTYCPP:GEN-END id=reactor.64*/

// KERNEL that must stay hand-written C++ (3 lines): a free `operator<`
// over a FOREIGN type (rusty::Rc<Fiber>) has no DSL trait-impl form -- a
// PartialOrd impl on Fiber would order Fibers, not Rc<Fiber> handles.
// Hoisted out of the class body (it was an in-class friend); at namespace
// scope ADL still finds it from `std::less<Rc<Fiber>>` because Fiber's
// namespace is an associated namespace of the template argument.
// Required by Reactor's `RefCell<std::set<Rc<Fiber>>> fibers_`.
inline bool operator<(const rusty::Rc<Fiber>& lhs, const rusty::Rc<Fiber>& rhs) {
  return lhs.get() < rhs.get();
}



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
#if RUSTYCPP_RUST
#[thread_local]
static mut reactor_clients_th_: rusty::HashMap<std::string, rusty::Vec<PollableProxy>> = rusty::HashMap::<std::string, rusty::Vec<PollableProxy>>::new();
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.50 version=1 rust_sha256=a0087a39b5a2b8beff6d62355134d719d0dbe2bca6a55741c1f36c4195648f24*/
extern thread_local rusty::HashMap<std::string, rusty::Vec<PollableProxy>> reactor_clients_th_;

inline thread_local rusty::HashMap<std::string, rusty::Vec<PollableProxy>> reactor_clients_th_ = rusty::HashMap<std::string, rusty::Vec<PollableProxy>>();
/*RUSTYCPP:GEN-END id=reactor.50*/

// Amortized-prune high-water mark, hoisted out of
// reactor_prune_finished_events_impl. (The old note here said a
// function-local static "is not DSL-expressible" -- that was never true;
// see the tracker's idioms. A namespace thread_local is kept because the
// value is shared across the whole file, not because a local was
// impossible.)
#if RUSTYCPP_RUST
#[thread_local]
static mut reactor_prune_hwm_th_: usize = 64usize;
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.58 version=1 rust_sha256=7b9485d87176dbd77774f7b7cafb0cc476c96d3259154aaf02b232ce3b25efae*/
extern thread_local size_t reactor_prune_hwm_th_;

inline thread_local size_t reactor_prune_hwm_th_ = static_cast<size_t>(64);
/*RUSTYCPP:GEN-END id=reactor.58*/

// Stackless-profile observability shim, defined next to g_stackless_profile
// further down (the DSL method cannot name the later-defined global).
void stackless_profile_note_enqueue();
void stackless_profile_note_register(size_t scanned, bool reuse, size_t slots_now);
// Spelling `rusty::Function<...>` inline as a DSL parameter type is a parse
// error, so reactor_poll_one's third parameter names this alias (same type
// as StacklessTaskEntry::poll_once).
using StacklessPollFn = rusty::Function<bool(rusty::Context&)>;
bool reactor_poll_one(const Reactor& self, size_t idx, StacklessPollFn* poll_fn);
void stackless_profile_note_poll_ready();
void stackless_profile_report_periodic_shim();

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
        verify(rusty::thread::current_id() == self.thread_id_.get());
        self.looping_.set(infinite);
        loop {
            let mut found_ready_events = true;
            while found_ready_events {
                found_ready_events = false;
                if self.process_stackless_tasks() {
                    found_ready_events = true;
                }
                let mut ready_events: rusty::VecDeque<rusty::Arc<EventPollable>> = Default::default();
                {
                    let mut waiting_guard = self.waiting_events_.borrow_mut();
                    let mut i: usize = 0usize;
                    while i < waiting_guard.len() {
                        let ev = (*waiting_guard)[i].clone();
                        (*ev).test();
                        i += 1usize;
                    }
                    let n_before = ready_events.len();
                    ready_events.append(waiting_guard.extract_if(move |ev: &rusty::Arc<EventPollable>| -> bool {
                        (*ev).status() == EventStatus::READY
                    }));
                    if ready_events.len() > n_before {
                        found_ready_events = true;
                    }
                    waiting_guard.retain(move |ev: &rusty::Arc<EventPollable>| -> bool {
                        (*ev).status() != EventStatus::DONE
                    });
                }
                {
                    let mut composite_guard = self.composite_events_.borrow_mut();
                    let mut i: usize = 0usize;
                    while i < composite_guard.len() {
                        let ev = (*composite_guard)[i].clone();
                        (*ev).test();
                        i += 1usize;
                    }
                    let n_before = ready_events.len();
                    ready_events.append(composite_guard.extract_if(move |ev: &rusty::Arc<EventPollable>| -> bool {
                        (*ev).status() == EventStatus::READY
                    }));
                    if ready_events.len() > n_before {
                        found_ready_events = true;
                    }
                    composite_guard.retain(move |ev: &rusty::Arc<EventPollable>| -> bool {
                        (*ev).status() != EventStatus::DONE
                    });
                }
                if do_check_timeout {
                    let before = ready_events.len();
                    self.check_timeout(&mut ready_events);
                    if ready_events.len() > before {
                        found_ready_events = true;
                    }
                }
                // Dispatch ready events. `continue` restructured as nested
                // ifs (the DSL has no continue); the Arc is cloned out of
                // the deque so no reference is held across continue_fiber.
                {
                    let mut i: usize = 0usize;
                    while i < ready_events.len() {
                        let ev = ready_events[i].clone();
                        i += 1usize;
                        if (*ev).status() != EventStatus::DONE {
                            let option_fiber = (*ev).upgrade_fiber();
                            if option_fiber.is_some() {
                                let fiber = option_fiber.unwrap();
                                let mut known = false;
                                {
                                    let fibers_guard = self.fibers_.borrow();
                                    known = (*fibers_guard).contains(fiber);
                                }
                                if known {
                                    verify(fiber.status_.get() == FiberStatus::PAUSED);
                                    if (*ev).status() == EventStatus::READY {
                                        (*ev).set_status(EventStatus::DONE);
                                    } else {
                                        verify((*ev).status() == EventStatus::TIMEOUT);
                                    }
                                    self.continue_fiber(&fiber);
                                }
                            }
                        }
                    }
                }
                if !infinite && !found_ready_events {
                    break;
                }
            }
            if !self.looping_.get() {
                break;
            }
        }
    }

    fn prune_finished_events(&self) {
        let mut guard = self.all_events_.borrow_mut();
        if guard.len() < reactor_prune_hwm_th_ {
            return;
        }
        guard.retain(move |e: &rusty::Arc<EventPollable>| -> bool {
            e.strong_count() > 1usize || !(*e).prunable()
        });
        reactor_prune_hwm_th_ = guard.len() * 2usize + 64usize;
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
        if fiber.status_.get() == FiberStatus::INIT {
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
        log_line(Log::INFO, 0i32, core::ptr::null(), std::format("waiting_events_: {}, composite_events_: {}",
                 self.waiting_events_.borrow().len(), self.composite_events_.borrow().len()));
    }

    fn register_fiber(&self, fiber: &rusty::Rc<Fiber>) {
        // std::set::insert returns pair<iterator, bool>; `.second` is true
        // when the value was newly inserted.
        let mut guard = self.fibers_.borrow_mut();
        let inserted = guard.insert(fiber.clone()).second;
        if !inserted {
            unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("[DEBUG] RegisterFiber: Failed to insert fiber into fibers_ set!")); }
            unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("[DEBUG] fibers_ size: {}, REUSING_FIBER: {}", guard.size(), REUSING_FIBER)); }
        }
        verify(inserted);
        verify(guard.size() > 0usize);
    }

    fn recycle(&self, fiber: &mut rusty::Rc<Fiber>) {
        // Fixes fibers not being recycled when they don't finish immediately.
        if REUSING_FIBER {
            fiber.status_.set(FiberStatus::RECYCLED);
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

    fn register_stackless_poller(&self, poller: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool>) -> usize {
        let scanned: usize = 0usize;
        {
            let mut free_guard = self.free_stackless_task_slots_.borrow_mut();
            if !free_guard.is_empty() {
                let idx: usize = free_guard.back();
                free_guard.pop();
                let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                if idx < tasks_guard.len() {
                    (*tasks_guard)[idx].active = true;
                    (*tasks_guard)[idx].queued = false;
                    (*tasks_guard)[idx].poll_once = poller;
                    stackless_profile_note_register(scanned, true, tasks_guard.len());
                    return idx;
                }
            }
        }
        let mut tasks_guard = self.stackless_tasks_.borrow_mut();
        tasks_guard.push(StacklessTaskEntry { active: true, queued: false, poll_once: poller });
        stackless_profile_note_register(scanned, false, tasks_guard.len());
        tasks_guard.len() - 1usize
    }

    fn process_stackless_tasks(&self) -> bool {
        let mut did_work = false;
        let mut keep_going = true;
        while keep_going {
            let mut idx: usize = 0usize;
            let mut have_task = false;
            {
                let mut ready_guard = self.ready_stackless_tasks_.borrow_mut();
                if ready_guard.is_empty() {
                    keep_going = false;
                } else {
                    idx = (*ready_guard)[0usize];
                    ready_guard.pop_front();
                    have_task = true;
                }
            }
            if have_task {
                // Move the poll function out of its slot before invoking it
                // (rusty::Function is move-only; take() leaves an empty one
                // behind). Reactor is single-threaded: a synchronous waker
                // during poll only mutates queued/active, never poll_once.
                let mut poll_fn: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool> = Default::default();
                let mut runnable = false;
                {
                    let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                    if idx < tasks_guard.len() {
                        (*tasks_guard)[idx].queued = false;
                        if (*tasks_guard)[idx].active && (*tasks_guard)[idx].poll_once {
                            poll_fn = core::mem::take(&mut (*tasks_guard)[idx].poll_once);
                            runnable = true;
                        }
                    }
                }
                if runnable {
                    did_work = true;
                    // reactor_poll_one is DSL now; it still takes the poll
                    // fn by raw pointer, which is exactly what a `&raw mut`
                    // argument lowers to.
                    let ready = reactor_poll_one(self, idx, &raw mut poll_fn);
                    {
                        let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                        if idx < tasks_guard.len() {
                            if ready {
                                stackless_profile_note_poll_ready();
                                (*tasks_guard)[idx].active = false;
                                (*tasks_guard)[idx].queued = false;
                                let empty_fn: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool> = Default::default();
                                (*tasks_guard)[idx].poll_once = empty_fn;
                                let mut free_guard = self.free_stackless_task_slots_.borrow_mut();
                                free_guard.push(idx as usize);
                            } else {
                                // Put the function back for the next poll.
                                (*tasks_guard)[idx].poll_once = poll_fn;
                            }
                        }
                    }
                }
            }
        }
        stackless_profile_report_periodic_shim();
        did_work
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
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[Reactor::~Reactor] Starting destruction, all_events_.len()={}, fibers_.size()={}",
                  self.all_events_.borrow().len(), self.fibers_.borrow().size()));
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[Reactor::~Reactor] Destructor body complete, about to destroy member variables"));
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.15 version=1 rust_sha256=3372368d12188ddcf7bd79f42a201fe665094ba6659527f8388420df09b55dbc*/
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
    void prune_finished_events() const;
    rusty::Rc<Fiber> create_run_fiber(rusty::Function<void()> func) const;
    void continue_fiber(const rusty::Rc<Fiber>& fiber) const;
    void display_waiting_ev() const;
    void register_fiber(const rusty::Rc<Fiber>& fiber) const;
    void recycle(rusty::Rc<Fiber>& fiber) const;
    void enqueue_stackless_task(size_t idx) const;
    size_t register_stackless_poller(rusty::Function<bool(rusty::Context&)> poller) const;
    bool process_stackless_tasks() const;
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
    verify(rusty::thread::current_id() == this->thread_id_.get());
    this->looping_.set(std::move(infinite));
    while (true) {
        auto found_ready_events = true;
        while (found_ready_events) {
            found_ready_events = false;
            if (this->process_stackless_tasks()) {
                found_ready_events = true;
            }
            rusty::VecDeque<rusty::Arc<EventPollable>> ready_events = rusty::default_like<rusty::VecDeque<rusty::Arc<EventPollable>>>();
            {
                auto waiting_guard = this->waiting_events_.borrow_mut();
                size_t i = static_cast<size_t>(0);
                while (rusty::detail::deref_if_pointer_like(i) < rusty::len(waiting_guard)) {
                    const auto ev = rusty::clone((*waiting_guard)[i]);
                    ((rusty::detail::deref_if_pointer_like(ev))).test();
                    i += static_cast<size_t>(1);
                }
                const auto n_before = rusty::len(ready_events);
                ready_events.append(waiting_guard->extract_if([=](const rusty::Arc<EventPollable>& ev) -> bool {
return ((rusty::detail::deref_if_pointer_like(ev))).status() == rusty::clone(EventStatus_READY());
}));
                if (rusty::len(ready_events) > rusty::detail::deref_if_pointer_like(n_before)) {
                    found_ready_events = true;
                }
                waiting_guard->retain([=](const rusty::Arc<EventPollable>& ev) -> bool {
return ((rusty::detail::deref_if_pointer_like(ev))).status() != rusty::clone(EventStatus_DONE());
});
            }
            {
                auto composite_guard = this->composite_events_.borrow_mut();
                size_t i = static_cast<size_t>(0);
                while (rusty::detail::deref_if_pointer_like(i) < rusty::len(composite_guard)) {
                    const auto ev = rusty::clone((*composite_guard)[i]);
                    ((rusty::detail::deref_if_pointer_like(ev))).test();
                    i += static_cast<size_t>(1);
                }
                const auto n_before = rusty::len(ready_events);
                ready_events.append(composite_guard->extract_if([=](const rusty::Arc<EventPollable>& ev) -> bool {
return ((rusty::detail::deref_if_pointer_like(ev))).status() == rusty::clone(EventStatus_READY());
}));
                if (rusty::len(ready_events) > rusty::detail::deref_if_pointer_like(n_before)) {
                    found_ready_events = true;
                }
                composite_guard->retain([=](const rusty::Arc<EventPollable>& ev) -> bool {
return ((rusty::detail::deref_if_pointer_like(ev))).status() != rusty::clone(EventStatus_DONE());
});
            }
            if (do_check_timeout) {
                const auto before = rusty::len(ready_events);
                this->check_timeout(ready_events);
                if (rusty::len(ready_events) > rusty::detail::deref_if_pointer_like(before)) {
                    found_ready_events = true;
                }
            }
            {
                size_t i = static_cast<size_t>(0);
                while (rusty::detail::deref_if_pointer_like(i) < rusty::len(ready_events)) {
                    const auto ev = rusty::clone(ready_events[i]);
                    i += static_cast<size_t>(1);
                    if (((rusty::detail::deref_if_pointer_like(ev))).status() != rusty::clone(EventStatus_DONE())) {
                        auto option_fiber = ((rusty::detail::deref_if_pointer_like(ev))).upgrade_fiber();
                        if (option_fiber.is_some()) {
                            const auto fiber = option_fiber.unwrap();
                            auto known = false;
                            {
                                const auto fibers_guard = this->fibers_.borrow();
                                known = rusty::contains((*fibers_guard), std::move(fiber));
                            }
                            if (known) {
                                verify([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.status_); }) { return (__r.status_); } else if constexpr (requires { (__r.status__field); }) { return (__r.status__field); } else if constexpr (requires { ((*__r).status_); }) { return ((*__r).status_); } else { return ((*__r).status__field); } }(fiber).get() == rusty::clone(FiberStatus_PAUSED()));
                                if (((rusty::detail::deref_if_pointer_like(ev))).status() == rusty::clone(EventStatus_READY())) {
                                    ((rusty::detail::deref_if_pointer_like(ev))).set_status(rusty::clone(rusty::clone(EventStatus_DONE())));
                                } else {
                                    verify(((rusty::detail::deref_if_pointer_like(ev))).status() == rusty::clone(EventStatus_TIMEOUT()));
                                }
                                this->continue_fiber(fiber);
                            }
                        }
                    }
                }
            }
            if (!infinite && rusty::detail::rust_not(found_ready_events)) {
                break;
            }
        }
        if (rusty::detail::rust_not(this->looping_.get())) {
            break;
        }
    }
}

void Reactor::prune_finished_events() const {
    auto guard = this->all_events_.borrow_mut();
    if (rusty::len(guard) < rusty::detail::deref_if_pointer_like(reactor_prune_hwm_th_)) {
        return;
    }
    guard->retain([=](const rusty::Arc<EventPollable>& e) -> bool {
return (e.strong_count() > static_cast<size_t>(1)) || rusty::detail::rust_not(((rusty::detail::deref_if_pointer_like(e))).prunable());
});
    reactor_prune_hwm_th_ = (rusty::len(guard) * static_cast<size_t>(2)) + static_cast<size_t>(64);
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
    if ((*fiber).status_.get() == rusty::clone(FiberStatus_INIT())) {
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
    log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("waiting_events_: {}, composite_events_: {}", rusty::len(this->waiting_events_.borrow()), rusty::len(this->composite_events_.borrow())));
}

void Reactor::register_fiber(const rusty::Rc<Fiber>& fiber) const {
    auto guard = this->fibers_.borrow_mut();
    const auto inserted = rusty::deref_call(guard, rusty::detail::__mdisp_insert{}, rusty::clone(fiber)).second;
    if (rusty::detail::rust_not(inserted)) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[DEBUG] RegisterFiber: Failed to insert fiber into fibers_ set!"));
        }
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[DEBUG] fibers_ size: {}, REUSING_FIBER: {}", rusty::deref_call(guard, rusty::detail::__mdisp_size{}), REUSING_FIBER));
        }
    }
    verify(std::move(inserted));
    verify(rusty::deref_call(guard, rusty::detail::__mdisp_size{}) > static_cast<size_t>(0));
}

void Reactor::recycle(rusty::Rc<Fiber>& fiber) const {
    if (REUSING_FIBER) {
        (*fiber).status_.set(rusty::clone(rusty::clone(FiberStatus_RECYCLED())));
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

size_t Reactor::register_stackless_poller(rusty::Function<bool(rusty::Context&)> poller) const {
    const size_t scanned = static_cast<size_t>(0);
    {
        auto free_guard = this->free_stackless_task_slots_.borrow_mut();
        if (rusty::detail::rust_not(rusty::is_empty(free_guard))) {
            size_t idx = free_guard->back();
            free_guard->pop();
            auto tasks_guard = this->stackless_tasks_.borrow_mut();
            if (rusty::detail::deref_if_pointer_like(idx) < rusty::len(tasks_guard)) {
                [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.active); }) { return (__r.active); } else if constexpr (requires { (__r.active_field); }) { return (__r.active_field); } else if constexpr (requires { ((*__r).active); }) { return ((*__r).active); } else { return ((*__r).active_field); } }((*tasks_guard)[idx]) = true;
                [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.queued); }) { return (__r.queued); } else if constexpr (requires { (__r.queued_field); }) { return (__r.queued_field); } else if constexpr (requires { ((*__r).queued); }) { return ((*__r).queued); } else { return ((*__r).queued_field); } }((*tasks_guard)[idx]) = false;
                [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.poll_once); }) { return (__r.poll_once); } else if constexpr (requires { (__r.poll_once_field); }) { return (__r.poll_once_field); } else if constexpr (requires { ((*__r).poll_once); }) { return ((*__r).poll_once); } else { return ((*__r).poll_once_field); } }((*tasks_guard)[idx]) = std::move(poller);
                stackless_profile_note_register(std::move(scanned), true, rusty::len(tasks_guard));
                return std::move(idx);
            }
        }
    }
    auto tasks_guard = this->stackless_tasks_.borrow_mut();
    tasks_guard->push(StacklessTaskEntry{.active = true, .queued = false, .poll_once = std::move(poller)});
    stackless_profile_note_register(std::move(scanned), false, rusty::len(tasks_guard));
    return rusty::len(tasks_guard) - static_cast<size_t>(1);
}

bool Reactor::process_stackless_tasks() const {
    auto did_work = false;
    auto keep_going = true;
    while (keep_going) {
        size_t idx = static_cast<size_t>(0);
        auto have_task = false;
        {
            auto ready_guard = this->ready_stackless_tasks_.borrow_mut();
            if (rusty::is_empty(ready_guard)) {
                keep_going = false;
            } else {
                idx = (*ready_guard)[static_cast<size_t>(0)];
                ready_guard->pop_front();
                have_task = true;
            }
        }
        if (have_task) {
            rusty::Function<bool(rusty::Context&)> poll_fn = rusty::default_like<rusty::Function<bool(rusty::Context&)>>();
            auto runnable = false;
            {
                auto tasks_guard = this->stackless_tasks_.borrow_mut();
                if (rusty::detail::deref_if_pointer_like(idx) < rusty::len(tasks_guard)) {
                    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.queued); }) { return (__r.queued); } else if constexpr (requires { (__r.queued_field); }) { return (__r.queued_field); } else if constexpr (requires { ((*__r).queued); }) { return ((*__r).queued); } else { return ((*__r).queued_field); } }((*tasks_guard)[idx]) = false;
                    if (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.active); }) { return (__r.active); } else if constexpr (requires { (__r.active_field); }) { return (__r.active_field); } else if constexpr (requires { ((*__r).active); }) { return ((*__r).active); } else { return ((*__r).active_field); } }((*tasks_guard)[idx])) && rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.poll_once); }) { return (__r.poll_once); } else if constexpr (requires { (__r.poll_once_field); }) { return (__r.poll_once_field); } else if constexpr (requires { ((*__r).poll_once); }) { return ((*__r).poll_once); } else { return ((*__r).poll_once_field); } }((*tasks_guard)[idx]))) {
                        poll_fn = rusty::mem::take([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.poll_once); }) { return (__r.poll_once); } else if constexpr (requires { (__r.poll_once_field); }) { return (__r.poll_once_field); } else if constexpr (requires { ((*__r).poll_once); }) { return ((*__r).poll_once); } else { return ((*__r).poll_once_field); } }((*tasks_guard)[idx]));
                        runnable = true;
                    }
                }
            }
            if (runnable) {
                did_work = true;
                const auto ready = reactor_poll_one((*this), std::move(idx), &poll_fn);
                {
                    auto tasks_guard = this->stackless_tasks_.borrow_mut();
                    if (rusty::detail::deref_if_pointer_like(idx) < rusty::len(tasks_guard)) {
                        if (ready) {
                            stackless_profile_note_poll_ready();
                            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.active); }) { return (__r.active); } else if constexpr (requires { (__r.active_field); }) { return (__r.active_field); } else if constexpr (requires { ((*__r).active); }) { return ((*__r).active); } else { return ((*__r).active_field); } }((*tasks_guard)[idx]) = false;
                            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.queued); }) { return (__r.queued); } else if constexpr (requires { (__r.queued_field); }) { return (__r.queued_field); } else if constexpr (requires { ((*__r).queued); }) { return ((*__r).queued); } else { return ((*__r).queued_field); } }((*tasks_guard)[idx]) = false;
                            rusty::Function<bool(rusty::Context&)> empty_fn = rusty::default_like<rusty::Function<bool(rusty::Context&)>>();
                            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.poll_once); }) { return (__r.poll_once); } else if constexpr (requires { (__r.poll_once_field); }) { return (__r.poll_once_field); } else if constexpr (requires { ((*__r).poll_once); }) { return ((*__r).poll_once); } else { return ((*__r).poll_once_field); } }((*tasks_guard)[idx]) = std::move(empty_fn);
                            auto free_guard = this->free_stackless_task_slots_.borrow_mut();
                            free_guard->push(static_cast<size_t>(idx));
                        } else {
                            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.poll_once); }) { return (__r.poll_once); } else if constexpr (requires { (__r.poll_once_field); }) { return (__r.poll_once_field); } else if constexpr (requires { ((*__r).poll_once); }) { return ((*__r).poll_once); } else { return ((*__r).poll_once_field); } }((*tasks_guard)[idx]) = std::move(poll_fn);
                        }
                    }
                }
            }
        }
    }
    stackless_profile_report_periodic_shim();
    return std::move(did_work);
}

void Reactor::check_timeout(rusty::VecDeque<rusty::Arc<EventPollable>>& ready_events) const {
    const int64_t time_now = Time::now(true);
    auto guard = this->timeout_events_.borrow_mut();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(guard)) {
        const auto event = rusty::clone((rusty::detail::deref_if_pointer_like(guard))[i]);
        if (((rusty::detail::deref_if_pointer_like(event))).status() == rusty::clone(EventStatus_WAIT())) {
            const auto wakeup_time = ((rusty::detail::deref_if_pointer_like(event))).wakeup_time();
            verify(rusty::detail::deref_if_pointer_like(wakeup_time) > static_cast<uint64_t>(0));
            if (rusty::detail::deref_if_pointer_like(time_now) >= (static_cast<int64_t>(wakeup_time))) {
                if (((rusty::detail::deref_if_pointer_like(event))).is_ready()) {
                    ((rusty::detail::deref_if_pointer_like(event))).set_status(rusty::clone(rusty::clone(EventStatus_READY())));
                } else {
                    ((rusty::detail::deref_if_pointer_like(event))).set_status(rusty::clone(rusty::clone(EventStatus_TIMEOUT())));
                }
            }
        }
        i += static_cast<size_t>(1);
    }
    ready_events.append(rusty::deref_call(guard, rusty::detail::__mdisp_extract_if{}, [=](const rusty::Arc<EventPollable>& sp) -> bool {
const auto status = ((rusty::detail::deref_if_pointer_like(sp))).status();
return (rusty::detail::deref_if_pointer_like(status) == rusty::clone(EventStatus_READY())) || (rusty::detail::deref_if_pointer_like(status) == rusty::clone(EventStatus_TIMEOUT()));
}));
    rusty::deref_call(guard, rusty::detail::__mdisp_retain{}, [=](const rusty::Arc<EventPollable>& sp) -> bool {
return ((rusty::detail::deref_if_pointer_like(sp))).status() != rusty::clone(EventStatus_DONE());
});
}

Reactor::~Reactor() noexcept(false) {
    if (_rusty_forgotten) { return; }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[Reactor::~Reactor] Starting destruction, all_events_.len()={}, fibers_.size()={}", rusty::len(this->all_events_.borrow()), this->fibers_.borrow()->size()));
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[Reactor::~Reactor] Destructor body complete, about to destroy member variables"));
}
/*RUSTYCPP:GEN-END id=reactor.15*/


// ==== Member templates hoisted out of `class Reactor` (Goal 0 Stage A:
// a DSL struct's GEN cannot mix in hand-written members, so the class's
// template members become free function templates). ====



// @safe - Spawn a stackless task with a completion callback when ready.
// Hoisted out of `class Reactor` (member template; a DSL struct's GEN is
// fully generated so hand-written members cannot remain). Behaviour is
// identical to the former member; `self_` replaces the implicit `this`.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block. A
// two-parameter DSL generic fn lowers to the same `template <typename T,
// typename OnReady>`, and the two fn-body-local structs lower verbatim
// (hoisted to the top of the emitted body, where they can still name the
// enclosing template's T / OnReady — which is exactly why neither carries
// generic parameters of its own: a local struct with its own <T, OnReady>
// emits a non-template declaration against a templated use site).
//
// Four things read differently from the hand-written original; each is
// forced by a lowering rule, not by choice:
//   * the parameter is `self_` — a DSL parameter literally named `self` is
//     swallowed into a receiver and the body emits `this->`.
//   * `std::atomic` + `std::memory_order` become
//     `rusty::sync::atomic::{AtomicUsize,AtomicBool}` + `Ordering::*`
//     (`exchange` is spelled `swap`). That also DELETES the `mutable` on
//     both wake-state fields: rusty's atomic load/store/swap are already
//     const, so they reach through `Arc::operator->`'s `const T*`.
//   * the two genuinely mutable fields (the Task and the callback) become
//     `RefCell` — the DSL's only interior mutability for move-only types.
//     The emitted poller closure captures `state` by value and is NOT
//     `mutable`, so every access through it is const. The task borrow is
//     scoped so that no borrow is held while `on_ready` runs, matching the
//     original (which held none).
//   * `early_wake` is `.clone()`d into each closure: a DSL `move ||`
//     closure MOVES its captures, and this body reads `early_wake` again
//     after registering the poller.
// `usize::MAX` lowers to `std::numeric_limits<size_t>::max()`, so
// kUnregisteredSlot stays a plain function-local (no namespace-scope hoist
// is needed, and the Task<void> sibling's own copy is untouched).
#if RUSTYCPP_RUST
fn reactor_spawn_stackless_task_with_result<T, OnReady>(self_: &Reactor, task: rusty::Task<T>, on_ready: OnReady) {
    // SAFETY: shared state is heap-owned; the reactor outlives callback
    // execution. Both counters are atomic because the waker may fire from
    // another thread.
    struct EarlyWakeState {
        reactor: *const Reactor,
        idx: rusty::sync::atomic::AtomicUsize,
        pending_wake: rusty::sync::atomic::AtomicBool
    }
    // SAFETY: TaskState is only reached through the Arc captured by the
    // poller closure.
    struct TaskState {
        task: rusty::RefCell<rusty::Task<T>>,
        on_ready: rusty::RefCell<rusty::Option<OnReady>>
    }

    let kUnregisteredSlot: usize = usize::MAX;
    let rp: *const Reactor = &raw const self_;
    let seed = EarlyWakeState {
        reactor: rp,
        idx: rusty::sync::atomic::AtomicUsize::new(kUnregisteredSlot),
        pending_wake: rusty::sync::atomic::AtomicBool::new(false)
    };
    let early_wake: rusty::Arc<EarlyWakeState> = rusty::Arc::<EarlyWakeState>::make(seed);

    // Each `move ||` closure gets its own clone; early_wake is read again
    // after the poller is registered.
    let ew_waker = early_wake.clone();
    let mut early_waker = rusty::Waker {
        wake_fn: move || {
            let slot = ew_waker.idx.load(rusty::sync::atomic::Ordering::Acquire);
            if slot == kUnregisteredSlot {
                ew_waker.pending_wake.store(true, rusty::sync::atomic::Ordering::Release);
            } else {
                (*ew_waker.reactor).enqueue_stackless_task(slot);
            }
        }
    };
    let wp: *mut rusty::Waker = &raw mut early_waker;
    let mut early_ctx = rusty::Context { waker: wp };
    // Named binding: a bare `&mut local` argument lowers to a pointer, and a
    // last-use local argument is std::move()d — neither binds
    // `Task::poll(rusty::Context&)`. An annotated `&mut` let emits a real
    // C++ reference.
    let ectx: &mut rusty::Context = &mut early_ctx;
    let mut early_poll = task.poll(ectx);
    if early_poll.is_ready() {
        on_ready(early_poll.value);
        return;
    }

    let ts = TaskState {
        task: rusty::RefCell::<rusty::Task<T>>::new(task),
        on_ready: rusty::RefCell::<rusty::Option<OnReady>>::new(rusty::Some(on_ready))
    };
    let state: rusty::Arc<TaskState> = rusty::Arc::<TaskState>::make(ts);
    let ew_poll = early_wake.clone();
    let idx = self_.register_stackless_poller(move |ctx: &mut rusty::Context| -> bool {
        // Scoped so the task borrow is released before on_ready runs.
        let mut poll_result: rusty::Poll<T> = rusty::Poll::<T>::pending();
        {
            let tguard = state.task.borrow_mut();
            poll_result = (*tguard).poll(ctx);
        }
        if !poll_result.is_ready() {
            return false;
        }
        ew_poll.idx.store(kUnregisteredSlot, rusty::sync::atomic::Ordering::Release);
        // take() moves the callback out and leaves None, so it fires once.
        let mut cb: rusty::Option<OnReady> = None;
        {
            let cbguard = state.on_ready.borrow_mut();
            cb = (*cbguard).take();
        }
        if cb.is_some() {
            let mut f = cb.unwrap();
            f(poll_result.value);
        }
        true
    });
    early_wake.idx.store(idx, rusty::sync::atomic::Ordering::Release);
    if early_wake.pending_wake.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
        self_.enqueue_stackless_task(idx);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.27 version=1 rust_sha256=d4ccf3448df318d9e5f90dd4096bf126a37ee2c7d83e5e8511903ffa1d3cd837*/
template<typename T, typename OnReady>
void reactor_spawn_stackless_task_with_result(const Reactor& self_, rusty::Task<T> task, OnReady on_ready) {
    struct EarlyWakeState {
        const Reactor* reactor;
        rusty::sync::atomic::AtomicUsize idx;
        rusty::sync::atomic::AtomicBool pending_wake;
    };
    struct TaskState {
        rusty::RefCell<rusty::Task<T>> task;
        rusty::RefCell<rusty::Option<OnReady>> on_ready;
    };
    size_t kUnregisteredSlot = std::numeric_limits<size_t>::max();
    const Reactor* rp = &self_;
    auto seed = EarlyWakeState{.reactor = rp, .idx = rusty::sync::atomic::AtomicUsize::new_(std::move(kUnregisteredSlot)), .pending_wake = rusty::sync::atomic::AtomicBool::new_(false)};
    const rusty::Arc<EarlyWakeState> early_wake = rusty::Arc<EarlyWakeState>::make(std::move(seed));
    auto ew_waker = rusty::clone(early_wake);
    auto early_waker = rusty::Waker{.wake_fn = [=, ew_waker = std::move(ew_waker), kUnregisteredSlot = std::move(kUnregisteredSlot)]() {
const auto slot = (*ew_waker).idx.load(rusty::sync::atomic::Ordering::Acquire);
if (rusty::detail::deref_if_pointer_like(slot) == rusty::detail::deref_if_pointer_like(kUnregisteredSlot)) {
    (*ew_waker).pending_wake.store(true, rusty::sync::atomic::Ordering::Release);
} else {
    ((*(*ew_waker).reactor)).enqueue_stackless_task(std::move(slot));
}
}};
    rusty::Waker* wp = &early_waker;
    auto early_ctx = rusty::Context{.waker = wp};
    rusty::Context& ectx = early_ctx;
    auto early_poll = task.poll(ectx);
    if (early_poll.is_ready()) {
        on_ready(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.value); }) { return (__r.value); } else if constexpr (requires { (__r.value_field); }) { return (__r.value_field); } else if constexpr (requires { ((*__r).value); }) { return ((*__r).value); } else { return ((*__r).value_field); } }(early_poll)));
        return;
    }
    auto ts = TaskState{.task = rusty::RefCell<rusty::Task<T>>::new_(std::move(task)), .on_ready = rusty::RefCell<rusty::Option<OnReady>>::new_(rusty::Option<OnReady>(std::move(on_ready)))};
    rusty::Arc<TaskState> state = rusty::Arc<TaskState>::make(std::move(ts));
    auto ew_poll = rusty::clone(early_wake);
    const auto idx = self_.register_stackless_poller([=, ew_poll = std::move(ew_poll), kUnregisteredSlot = std::move(kUnregisteredSlot), state = std::move(state)](rusty::Context& ctx) -> bool {
rusty::Poll<T> poll_result = rusty::Poll<T>::pending();
{
    auto tguard = (*state).task.borrow_mut();
    poll_result = ((*tguard)).poll(ctx);
}
if (rusty::detail::rust_not(poll_result.is_ready())) {
    return false;
}
(*ew_poll).idx.store(std::move(kUnregisteredSlot), rusty::sync::atomic::Ordering::Release);
rusty::Option<OnReady> cb = rusty::Option<OnReady>{rusty::None};
{
    auto cbguard = (*state).on_ready.borrow_mut();
    cb = ((*cbguard)).take();
}
if (cb.is_some()) {
    OnReady f = cb.unwrap();
    f(std::move(poll_result.value));
}
return true;
});
    (*early_wake).idx.store(std::move(idx), rusty::sync::atomic::Ordering::Release);
    if ((*early_wake).pending_wake.swap(false, rusty::sync::atomic::Ordering::AcqRel)) {
        self_.enqueue_stackless_task(std::move(idx));
    }
}
/*RUSTYCPP:GEN-END id=reactor.27*/

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
// Shared post-mint registration, as inline Rust DSL. Unique-owner init
// window: ev is freshly minted (strong_count 1), so get_mut() gives the
// one mutable access needed to stamp __debug_creator and install the
// self weak-ref before ev is ever shared. The upcast to
// Arc<EventPollable> is a plain typed let — rusty::Arc's converting
// ctor does the derived->base hop (sync::Weak has none of its own).
// The canonical strong ref lands in all_events_; finished sole-owned
// events are pruned amortized (bounded growth).
#if RUSTYCPP_RUST
fn reactor_setup_sp_event<Ev>(ev0: Arc<Ev>) -> Arc<Ev> {
    let mut ev = ev0;
    {
        let opt = ev.get_mut();
        verify(opt.is_some());
        let m: &mut Ev = opt.unwrap();
        m.state_.__debug_creator = 1;
        let base: rusty::Arc<EventPollable> = ev.clone();
        m.set_self(rusty::sync::downgrade(base));
    }
    let reactor = Reactor::get_reactor();
    {
        let stored: rusty::Arc<EventPollable> = ev.clone();
        let mut guard = (*reactor).all_events_.borrow_mut();
        (*guard).push_back(stored);
    }
    (*reactor).prune_finished_events();
    ev
}

// Per-type creation entry points (the event_make dispatcher's named
// branches, one honest factory each — the callsite-rewrite campaign
// migrates reactor_create_sp_event<Ev> sites onto these).
fn create_sp_int_event(target: i32) -> Arc<IntEvent> {
    reactor_setup_sp_event::<IntEvent>(int_event_make(target))
}

fn create_sp_timeout_event(wait_us: u64) -> Arc<TimeoutEvent> {
    reactor_setup_sp_event::<TimeoutEvent>(timeout_event_make(wait_us))
}

fn create_sp_never_event() -> Arc<NeverEvent> {
    reactor_setup_sp_event::<NeverEvent>(never_event_make())
}

fn create_sp_waitany(a: rusty::Arc<EventPollable>, b: rusty::Arc<EventPollable>) -> Arc<WaitAny> {
    reactor_setup_sp_event::<WaitAny>(waitany_make(a, b))
}

fn create_sp_waitall() -> Arc<WaitAll> {
    reactor_setup_sp_event::<WaitAll>(waitall_make())
}

fn create_sp_waitall_from(evs: &rusty::Vec<rusty::Arc<EventPollable>>) -> Arc<WaitAll> {
    reactor_setup_sp_event::<WaitAll>(waitall_make_from(evs))
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.26 version=1 rust_sha256=451336cab00fbd4d05d24967761fe083770c12357017c2edd356fd2f3edb09f4*/
template<typename Ev>
rusty::Arc<Ev> reactor_setup_sp_event(rusty::Arc<Ev> ev0);

template<typename Ev>
rusty::Arc<Ev> reactor_setup_sp_event(rusty::Arc<Ev> ev0) {
    auto ev = std::move(ev0);
    {
        auto opt = ev.get_mut();
        verify(opt.is_some());
        Ev& m = opt.unwrap();
        m.state_.__debug_creator = 1;
        const rusty::Arc<EventPollable> base = rusty::clone(ev);
        rusty::deref_call(m, rusty::detail::__mdisp_set_self{}, rusty::sync::downgrade(std::move(base)));
    }
    const auto reactor = Reactor::get_reactor();
    {
        rusty::Arc<EventPollable> stored = rusty::clone(ev);
        auto&& guard = (rusty::detail::deref_if_pointer_like(reactor)).all_events_.borrow_mut();
        ((rusty::detail::deref_if_pointer_like(guard))).push_back(std::move(stored));
    }
    ((rusty::detail::deref_if_pointer_like(reactor))).prune_finished_events();
    return std::move(ev);
}

rusty::Arc<IntEvent> create_sp_int_event(int32_t target) {
    return reactor_setup_sp_event<IntEvent>(int_event_make(std::move(target)));
}

rusty::Arc<TimeoutEvent> create_sp_timeout_event(uint64_t wait_us) {
    return reactor_setup_sp_event<TimeoutEvent>(timeout_event_make(std::move(wait_us)));
}

rusty::Arc<NeverEvent> create_sp_never_event() {
    return reactor_setup_sp_event<NeverEvent>(never_event_make());
}

rusty::Arc<WaitAny> create_sp_waitany(rusty::Arc<EventPollable> a, rusty::Arc<EventPollable> b) {
    return reactor_setup_sp_event<WaitAny>(waitany_make(std::move(a), std::move(b)));
}

rusty::Arc<WaitAll> create_sp_waitall() {
    return reactor_setup_sp_event<WaitAll>(waitall_make());
}

rusty::Arc<WaitAll> create_sp_waitall_from(const rusty::Vec<rusty::Arc<EventPollable>>& evs) {
    return reactor_setup_sp_event<WaitAll>(waitall_make_from(evs));
}
/*RUSTYCPP:GEN-END id=reactor.26*/


// BoxEvent<T> creation (the old dispatcher's is_box_event branch, now an
// honest 1-line generic -- BoxEvent's aggregate seeding lives in
// boxevent_make<T>). A DSL `fn f<T>` emits a real `template<typename T>`,
// so the ~12 deptran call sites are untouched.
// @unsafe { reactor_setup_sp_event stamps the freshly-minted Arc through
//           get_mut() and installs the self weak-ref }
#if RUSTYCPP_RUST
fn create_sp_box_event<T>() -> Arc<BoxEvent<T>> {
    reactor_setup_sp_event::<BoxEvent<T>>(boxevent_make::<T>())
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.42 version=1 rust_sha256=185041ca732aa131ec2c5045aecef199f8ff06e2e11eb22bc1e97823137fe571*/
template<typename T>
rusty::Arc<BoxEvent<T>> create_sp_box_event() {
    return reactor_setup_sp_event<BoxEvent<T>>(boxevent_make<T>());
}
/*RUSTYCPP:GEN-END id=reactor.42*/

// (reactor_create_event<Ev>& is gone — its only call site now pins a
//  typed factory's Arc with set_prunable(false) directly.)


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
pub enum PollCommand {
    AddPollable { pollable: Box<PollableBase> },
    RemovePollable { fd: i32 },
    ClosePollable { fd: i32 },
    UpdateMode { fd: i32, new_mode: i32 },
    AddJob { job: Arc<Job> },
    RemoveJob { job: Arc<Job> },
    Shutdown,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.poll_cmds version=1 rust_sha256=1f731da7278a0257428df0b7442676810ae656ca09e9a861499fc7a802bd8e49*/
struct PollCommand_AddPollable;
struct PollCommand_RemovePollable;
struct PollCommand_ClosePollable;
struct PollCommand_UpdateMode;
struct PollCommand_AddJob;
struct PollCommand_RemoveJob;
struct PollCommand_Shutdown;
using PollCommand = std::variant<PollCommand_AddPollable, PollCommand_RemovePollable, PollCommand_ClosePollable, PollCommand_UpdateMode, PollCommand_AddJob, PollCommand_RemoveJob, PollCommand_Shutdown>;

// Algebraic data type
struct PollCommand_AddPollable {
    rusty::Box<PollableBase> pollable;
};
struct PollCommand_RemovePollable {
    int32_t fd;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
struct PollCommand_ClosePollable {
    int32_t fd;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
struct PollCommand_UpdateMode {
    int32_t fd;
    int32_t new_mode;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
struct PollCommand_AddJob {
    rusty::Arc<Job> job;
};
struct PollCommand_RemoveJob {
    rusty::Arc<Job> job;
};
struct PollCommand_Shutdown { static constexpr bool is_send = true; static constexpr bool is_sync = true; };
PollCommand_AddPollable AddPollable(rusty::Box<PollableBase> pollable);
PollCommand_RemovePollable RemovePollable(int32_t fd);
PollCommand_ClosePollable ClosePollable(int32_t fd);
PollCommand_UpdateMode UpdateMode(int32_t fd, int32_t new_mode);
PollCommand_AddJob AddJob(rusty::Arc<Job> job);
PollCommand_RemoveJob RemoveJob(rusty::Arc<Job> job);
PollCommand_Shutdown Shutdown();
using PollCommand = std::variant<PollCommand_AddPollable, PollCommand_RemovePollable, PollCommand_ClosePollable, PollCommand_UpdateMode, PollCommand_AddJob, PollCommand_RemoveJob, PollCommand_Shutdown>;
PollCommand_AddPollable AddPollable(rusty::Box<PollableBase> pollable) { return PollCommand_AddPollable{.pollable = std::forward<rusty::Box<PollableBase>>(pollable)};  }
PollCommand_RemovePollable RemovePollable(int32_t fd) { return PollCommand_RemovePollable{.fd = std::forward<int32_t>(fd)};  }
PollCommand_ClosePollable ClosePollable(int32_t fd) { return PollCommand_ClosePollable{.fd = std::forward<int32_t>(fd)};  }
PollCommand_UpdateMode UpdateMode(int32_t fd, int32_t new_mode) { return PollCommand_UpdateMode{.fd = std::forward<int32_t>(fd), .new_mode = std::forward<int32_t>(new_mode)};  }
PollCommand_AddJob AddJob(rusty::Arc<Job> job) { return PollCommand_AddJob{.job = std::forward<rusty::Arc<Job>>(job)};  }
PollCommand_RemoveJob RemoveJob(rusty::Arc<Job> job) { return PollCommand_RemoveJob{.job = std::forward<rusty::Arc<Job>>(job)};  }
PollCommand_Shutdown Shutdown() { return PollCommand_Shutdown{};  }
/*RUSTYCPP:GEN-END id=reactor.poll_cmds*/


}  // export namespace rrr

// --- from reactor.h (trait spec for PollCommand) -------------------------
#if RUSTYCPP_RUST
unsafe impl Send for rrr::PollCommand {}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.71 version=1 rust_sha256=e4cd59fa5ed7d0f2ccc7c55f9efeb7eda1ee3061306dc2cf4ab9522536e9df1f*/
template<> struct rusty::is_send<rrr::PollCommand> : std::true_type {};
/*RUSTYCPP:GEN-END id=reactor.71*/

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
#if RUSTYCPP_RUST
#[thread_local]
static mut g_current_poll_worker: *mut PollThreadWorker = core::ptr::null_mut();
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.60 version=1 rust_sha256=cf21adaa5dfba5be843570257794b9ec5fc01b65352deba6c9b89f7e99db3e57*/
extern thread_local PollThreadWorker* g_current_poll_worker;

inline thread_local PollThreadWorker* g_current_poll_worker = rusty::ptr::null_mut();
/*RUSTYCPP:GEN-END id=reactor.60*/

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

// @safe - Check if the current thread is a poll thread. Both blockers the
// old comment cited have expired: a DSL body CAN read a namespace-scope
// `thread_local` raw pointer, and `.is_null()` lowers to a plain
// `== nullptr` test (no `nullptr_`).
#if RUSTYCPP_RUST
fn pollworker_is_on_poll_thread() -> bool {
    !g_current_poll_worker.is_null()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.43 version=1 rust_sha256=1509dddc81622acb38bc65958150ba8cf8e9706289579ee0422d9f72043bc4e6*/
bool pollworker_is_on_poll_thread();

bool pollworker_is_on_poll_thread() {
    return rusty::detail::rust_not((g_current_poll_worker == nullptr));
}
/*RUSTYCPP:GEN-END id=reactor.43*/

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
// Moved ABOVE its first use: the DSL shutdown() below calls it. The old
// "an `inline` definition emits no external symbol, so a forward
// declaration links only if the definition is non-inline" caveat went
// away with the hand-written body — the GEN definition is non-inline.
// @safe - `platform::threading::thread_id` is `std::thread::id` (default
// backend) or `pthread_t` (POSIX backend); both are 8-byte and trivially
// copyable on every platform we support. The old "C++ template
// metaprogramming, not DSL-expressible" verdict was stale twice over: a
// foreign type PATH lowers inside a turbofish (so the
// `decltype(std::declval<...>())` dance is unnecessary), and
// `std::bit_cast` itself enforces equal size plus trivial copyability.
#if RUSTYCPP_RUST
fn u64_to_thread_id(bits: u64) -> rusty::thread::ThreadId {
    let native = std::bit_cast::<rusty::platform::threading::thread_id>(bits);
    rusty::thread::ThreadId(native)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.62 version=1 rust_sha256=4f5b9dcf581a693756510a542a29adfcfc326a869fd3ba068a785a70705ce431*/
rusty::thread::ThreadId u64_to_thread_id(uint64_t bits);

rusty::thread::ThreadId u64_to_thread_id(uint64_t bits) {
    auto native = std::bit_cast<rusty::platform::threading::thread_id>(std::move(bits));
    return rusty::thread::ThreadId(std::move(native));
}
/*RUSTYCPP:GEN-END id=reactor.62*/

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
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Called from TID={}", main_tid as i32));
        if self.shutdown_called_.swap(true) {
            log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Already called, returning"));
            return;
        }
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Sending CmdShutdown"));
        self.sender_.send(PollCommand::Shutdown);
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] CmdShutdown sent"));
        // Thread-safe read of the poll thread's id.
        let current_tid = rusty::thread::current_id();
        let poll_tid = u64_to_thread_id(
            self.poll_thread_id_bits_.load(rusty::sync::atomic::Ordering::Acquire));
        if current_tid == poll_tid {
            log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Called from poll thread, skipping join"));
            return;
        }
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Acquiring join_handle lock..."));
        // Scoped so the guard drops BEFORE the "Released" log below, as the
        // C++ block did.
        {
            let mut guard = self.join_handle_.lock().unwrap();
            log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] join_handle lock acquired"));
            if (*guard).is_some() {
                log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Calling thread.join()..."));
                (*guard).take().unwrap().join();
                log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] thread.join() completed!"));
            } else {
                log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] join_handle is None, thread already joined"));
            }
        }
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Released join_handle lock"));
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::shutdown] Complete"));
    }

    fn add_proxy(&self, poll: PollableProxy) {
        self.sender_.send(PollCommand::AddPollable { pollable: poll });
    }

    fn remove(&self, poll: &mut Pollable) {
        self.sender_.send(PollCommand::RemovePollable { fd: poll.fd() });
    }

    // fd-keyed variant (remove only reads .fd() anyway); lets
    // shim-only callers avoid the Pollable base entirely.
    fn remove_fd(&self, fd: i32) {
        self.sender_.send(PollCommand::RemovePollable { fd: fd });
    }

    // Thread-safe close: removes from epoll, closes socket, drops
    // proxy ownership.
    fn request_close(&self, fd: i32) {
        self.sender_.send(PollCommand::ClosePollable { fd: fd });
    }

    fn update_mode(&self, fd: i32, new_mode: i32) {
        let result = self.sender_.send(PollCommand::UpdateMode { fd: fd, new_mode: new_mode });
        if result.is_err() {
            unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("PollThread::update_mode: send failed! Channel disconnected?")); }
        }
    }

    fn add(&self, job: Arc<Job>) {
        self.sender_.send(PollCommand::AddJob { job: job });
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
/*RUSTYCPP:GEN-BEGIN id=reactor.poll_thread version=1 rust_sha256=e9b09a245d98f364afc4319ef61d7b219f544d9f253c2ebf476fe5d8a08cfc38*/
struct PollThread;

struct PollThread {
    rusty::sync::mpsc::Sender<PollCommand> sender_;
    PollJoinSlot join_handle_;
    rusty::sync::atomic::AtomicU64 poll_thread_id_bits_;
    rusty::sync::atomic::AtomicBool shutdown_called_;
    mutable bool _rusty_forgotten = false;
    PollThread(rusty::sync::mpsc::Sender<PollCommand> sender__init, PollJoinSlot join_handle__init, rusty::sync::atomic::AtomicU64 poll_thread_id_bits__init, rusty::sync::atomic::AtomicBool shutdown_called__init) : sender_(std::move(sender__init)), join_handle_(std::move(join_handle__init)), poll_thread_id_bits_(std::move(poll_thread_id_bits__init)), shutdown_called_(std::move(shutdown_called__init)) {}
    PollThread(const PollThread&) = delete;
    PollThread(PollThread&& other) noexcept : sender_(std::move(other.sender_)), join_handle_(std::move(other.join_handle_)), poll_thread_id_bits_(std::move(other.poll_thread_id_bits_)), shutdown_called_(std::move(other.shutdown_called_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    PollThread& operator=(const PollThread&) = delete;
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
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Called from TID={}", static_cast<int32_t>(main_tid)));
    if (this->shutdown_called_.swap(true)) {
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Already called, returning"));
        return;
    }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Sending CmdShutdown"));
    this->sender_.send(PollCommand_Shutdown{});
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] CmdShutdown sent"));
    const auto current_tid = rusty::thread::current_id();
    const auto poll_tid = u64_to_thread_id(this->poll_thread_id_bits_.load(rusty::sync::atomic::Ordering::Acquire));
    if (rusty::detail::deref_if_pointer_like(current_tid) == rusty::detail::deref_if_pointer_like(poll_tid)) {
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Called from poll thread, skipping join"));
        return;
    }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Acquiring join_handle lock..."));
    {
        auto guard = this->join_handle_.lock().unwrap();
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] join_handle lock acquired"));
        if (((rusty::detail::deref_if_pointer_like(guard))).is_some()) {
            log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Calling thread.join()..."));
            ((rusty::detail::deref_if_pointer_like(guard))).take().unwrap().join();
            log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] thread.join() completed!"));
        } else {
            log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] join_handle is None, thread already joined"));
        }
    }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Released join_handle lock"));
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::shutdown] Complete"));
}

void PollThread::add_proxy(PollableProxy poll) const {
    this->sender_.send(PollCommand_AddPollable{.pollable = std::move(poll)});
}

void PollThread::remove(Pollable& poll) const {
    this->sender_.send(PollCommand_RemovePollable{.fd = poll.fd()});
}

void PollThread::remove_fd(int32_t fd) const {
    this->sender_.send(PollCommand_RemovePollable{.fd = std::move(fd)});
}

void PollThread::request_close(int32_t fd) const {
    this->sender_.send(PollCommand_ClosePollable{.fd = std::move(fd)});
}

void PollThread::update_mode(int32_t fd, int32_t new_mode) const {
    const auto result = this->sender_.send(PollCommand_UpdateMode{.fd = std::move(fd), .new_mode = std::move(new_mode)});
    if (result.is_err()) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("PollThread::update_mode: send failed! Channel disconnected?"));
        }
    }
}

void PollThread::add(rusty::Arc<Job> job) const {
    this->sender_.send(PollCommand_AddJob{.job = std::move(job)});
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
#if RUSTYCPP_RUST
unsafe impl Send for rrr::PollThread {}
unsafe impl Sync for rrr::PollThread {}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.72 version=1 rust_sha256=aa9c886955b892c3bcbdf54d85993794d1d7a99a4721abed7c8642ef51d221d3*/
template<> struct rusty::is_send<rrr::PollThread> : std::true_type {};
template<> struct rusty::is_sync<rrr::PollThread> : std::true_type {};
/*RUSTYCPP:GEN-END id=reactor.72*/

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
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The emitted `enum class QuorumPolicy`
// takes the default `int` underlying type -- exactly what the former
// `: int` spelling gave -- so the deptran call sites
// (`q().policy_.set(QuorumPolicy::ALL_NO)` and friends) are untouched.
#if RUSTYCPP_RUST
#[repr(i32)]
enum QuorumPolicy {
    DEFAULT = 0,
    ALL_NO = 1,
    LEADER_AND = 2,
    COMMITTED_SHORT = 3,
    ALWAYS_READY = 4,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.44 version=1 rust_sha256=128d4ad4314a9e9dda82fe04b9b69a948d07e3d404940eb6c2ffa3163ec8938d*/
enum class QuorumPolicy;
constexpr QuorumPolicy QuorumPolicy_DEFAULT();
constexpr QuorumPolicy QuorumPolicy_ALL_NO();
constexpr QuorumPolicy QuorumPolicy_LEADER_AND();
constexpr QuorumPolicy QuorumPolicy_COMMITTED_SHORT();
constexpr QuorumPolicy QuorumPolicy_ALWAYS_READY();

enum class QuorumPolicy {
    DEFAULT = 0,
    ALL_NO = 1,
    LEADER_AND = 2,
    COMMITTED_SHORT = 3,
    ALWAYS_READY = 4
};
inline constexpr QuorumPolicy QuorumPolicy_DEFAULT() { return QuorumPolicy::DEFAULT; }
inline constexpr QuorumPolicy QuorumPolicy_ALL_NO() { return QuorumPolicy::ALL_NO; }
inline constexpr QuorumPolicy QuorumPolicy_LEADER_AND() { return QuorumPolicy::LEADER_AND; }
inline constexpr QuorumPolicy QuorumPolicy_COMMITTED_SHORT() { return QuorumPolicy::COMMITTED_SHORT; }
inline constexpr QuorumPolicy QuorumPolicy_ALWAYS_READY() { return QuorumPolicy::ALWAYS_READY; }
/*RUSTYCPP:GEN-END id=reactor.44*/

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
// (wired into rrr::event_make). The owning QuorumEventWrapper is now DSL too
// (below), with a QuorumEventBase construction shim in deptran/communicator.h.

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
        if (*fe).status_.get() != rrr::EventStatus::TIMEOUT && (*fe).status_.get() != rrr::EventStatus::DONE {
            (*fe).set(self.n_voted_yes_.get() + self.n_voted_no_.get());
        }
    }
    fn vote_no(&self) {
        self.n_voted_no_.set(self.n_voted_no_.get() + 1);
        event_test_impl(self);
        let fe = self.finalize_event_.clone();
        if (*fe).status_.get() != rrr::EventStatus::TIMEOUT && (*fe).status_.get() != rrr::EventStatus::DONE {
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
/*RUSTYCPP:GEN-BEGIN id=reactor.quorum_event version=1 rust_sha256=55c3c980234798f8f2eb82d32dd527e591afcda35c1aee548f28dc853d4a02bd*/
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
    if (this->policy_.get() == rusty::clone(QuorumPolicy_LEADER_AND())) {
        return rusty::detail::deref_if_pointer_like(base) && (this->n_leader_yes_.get() >= this->num_leader_.get());
    }
    return std::move(base);
}

bool QuorumEvent::no() const {
    if (this->policy_.get() == rusty::clone(QuorumPolicy_ALL_NO())) {
        return this->n_voted_no_.get() == rusty::detail::deref_if_pointer_like(this->n_total_);
    }
    verify(rusty::detail::deref_if_pointer_like(this->n_total_) >= rusty::detail::deref_if_pointer_like(this->quorum_));
    auto base = this->n_voted_no_.get() > ((rusty::detail::deref_if_pointer_like(this->n_total_) - rusty::detail::deref_if_pointer_like(this->quorum_)));
    if (this->policy_.get() == rusty::clone(QuorumPolicy_LEADER_AND())) {
        return rusty::detail::deref_if_pointer_like(base) || (this->n_leader_no_.get() > 0);
    }
    return std::move(base);
}

void QuorumEvent::vote_yes() const {
    this->n_voted_yes_.set(this->n_voted_yes_.get() + static_cast<int32_t>(1));
    event_test_impl((*this));
    const auto fe = rusty::clone(this->finalize_event_);
    if (((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(rrr::EventStatus_TIMEOUT())) && ((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(rrr::EventStatus_DONE()))) {
        ((rusty::detail::deref_if_pointer_like(fe))).set(this->n_voted_yes_.get() + this->n_voted_no_.get());
    }
}

void QuorumEvent::vote_no() const {
    this->n_voted_no_.set(this->n_voted_no_.get() + static_cast<int32_t>(1));
    event_test_impl((*this));
    const auto fe = rusty::clone(this->finalize_event_);
    if (((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(rrr::EventStatus_TIMEOUT())) && ((rusty::detail::deref_if_pointer_like(fe)).status_.get() != rusty::clone(rrr::EventStatus_DONE()))) {
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
    if (rusty::detail::deref_if_pointer_like(p) == rusty::clone(QuorumPolicy_ALWAYS_READY())) {
        return true;
    }
    if (rusty::detail::deref_if_pointer_like(p) == rusty::clone(QuorumPolicy_ALL_NO())) {
        return this->yes() || this->no();
    }
    if (rusty::detail::deref_if_pointer_like(p) == rusty::clone(QuorumPolicy_COMMITTED_SHORT())) {
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
// `class XQuorumEvent : public QuorumEventBase` — the 4-line deptran-local
// shim in src/deptran/communicator.h that adapts the DSL factory below to a
// real 2-arg base constructor. They OWN the reactor-registered QuorumEvent
// instead of BEING it, so nothing outside rrr inherits the event type (a hard
// requirement for flattening QuorumEvent to an inline-Rust DSL struct, which
// cannot be a base class). The wrapper itself is not an Event and is never
// registered; waiting/voting forward to the owned, registered `q_`.
//
// Field access through a wrapper goes via `q()`:  e->timeouted_  becomes
// e->q().timeouted_. The common verb surface is forwarded so method call
// sites compile unchanged. `q_` is set once at construction and never
// reseated.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block. Only
// inherent impls, so the emitted struct stays a copyable aggregate that
// hand-written C++ still derives from. Two deliberate, benign widenings vs.
// the former hand-written class:
//   * every method is emitted `const` (the Arc is a const view and every
//     QuorumEvent mutation goes through Cell::set / RefCell, both const), so
//     the old const/non-const `q()` pair collapses into the single
//     `const QuorumEvent& q() const` that serves both call shapes;
//   * `test()` now returns QuorumEvent::test()'s bool instead of void —
//     every call site discards it.
#if RUSTYCPP_RUST
struct QuorumEventWrapper {
    q_: rusty::Arc<QuorumEvent>,
}

impl QuorumEventWrapper {
    fn new(n_total: i32, quorum: i32) -> QuorumEventWrapper {
        QuorumEventWrapper { q_: create_sp_quorum_event(n_total, quorum) }
    }
    fn q(&self) -> &QuorumEvent {
        &(*self.q_)
    }
    fn wait(&self) {
        (*self.q_).wait()
    }
    fn wait_timeout(&self, timeout: u64) {
        (*self.q_).wait_timeout(timeout)
    }
    fn log(&self) {
        (*self.q_).log()
    }
    fn get_fiber_id(&self) -> u64 {
        (*self.q_).get_fiber_id()
    }
    fn vote_yes(&self) {
        (*self.q_).vote_yes()
    }
    fn vote_no(&self) {
        (*self.q_).vote_no()
    }
    fn yes(&self) -> bool {
        (*self.q_).yes()
    }
    fn no(&self) -> bool {
        (*self.q_).no()
    }
    fn is_ready(&self) -> bool {
        (*self.q_).is_ready()
    }
    fn is_slow(&self) -> bool {
        (*self.q_).is_slow()
    }
    fn test(&self) -> bool {
        (*self.q_).test()
    }
    fn add_xid(&self, site: u16, xid: rrr::i64) {
        (*self.q_).add_xid(site, xid)
    }
    fn remove_xid(&self, site: u16) {
        (*self.q_).remove_xid(site)
    }
    fn finalize(&self, timeout: u64, f: QuorumFinalizeFn) {
        (*self.q_).finalize(timeout, f)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.36 version=1 rust_sha256=07fabcd3811b39c7816349d8e616b394da47b85192c0829831b78890f8108e3a*/
struct QuorumEventWrapper;

struct QuorumEventWrapper {
    rusty::Arc<QuorumEvent> q_;

    static QuorumEventWrapper new_(int32_t n_total, int32_t quorum);
    const QuorumEvent& q() const;
    void wait() const;
    void wait_timeout(uint64_t timeout) const;
    void log() const;
    uint64_t get_fiber_id() const;
    void vote_yes() const;
    void vote_no() const;
    bool yes() const;
    bool no() const;
    bool is_ready() const;
    bool is_slow() const;
    bool test() const;
    void add_xid(uint16_t site, rrr::i64 xid) const;
    void remove_xid(uint16_t site) const;
    void finalize(uint64_t timeout, QuorumFinalizeFn f) const;
};


QuorumEventWrapper QuorumEventWrapper::new_(int32_t n_total, int32_t quorum) {
    return QuorumEventWrapper{.q_ = create_sp_quorum_event(std::move(n_total), std::move(quorum))};
}

const QuorumEvent& QuorumEventWrapper::q() const {
    return (rusty::detail::deref_if_pointer_like(this->q_));
}

void QuorumEventWrapper::wait() const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).wait();
}

void QuorumEventWrapper::wait_timeout(uint64_t timeout) const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).wait_timeout(std::move(timeout));
}

void QuorumEventWrapper::log() const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).log();
}

uint64_t QuorumEventWrapper::get_fiber_id() const {
    return ((rusty::detail::deref_if_pointer_like(this->q_))).get_fiber_id();
}

void QuorumEventWrapper::vote_yes() const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).vote_yes();
}

void QuorumEventWrapper::vote_no() const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).vote_no();
}

bool QuorumEventWrapper::yes() const {
    return ((rusty::detail::deref_if_pointer_like(this->q_))).yes();
}

bool QuorumEventWrapper::no() const {
    return ((rusty::detail::deref_if_pointer_like(this->q_))).no();
}

bool QuorumEventWrapper::is_ready() const {
    return ((rusty::detail::deref_if_pointer_like(this->q_))).is_ready();
}

bool QuorumEventWrapper::is_slow() const {
    return ((rusty::detail::deref_if_pointer_like(this->q_))).is_slow();
}

bool QuorumEventWrapper::test() const {
    return ((rusty::detail::deref_if_pointer_like(this->q_))).test();
}

void QuorumEventWrapper::add_xid(uint16_t site, rrr::i64 xid) const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).add_xid(std::move(site), std::move(xid));
}

void QuorumEventWrapper::remove_xid(uint16_t site) const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).remove_xid(std::move(site));
}

void QuorumEventWrapper::finalize(uint64_t timeout, QuorumFinalizeFn f) const {
    ((rusty::detail::deref_if_pointer_like(this->q_))).finalize(std::move(timeout), std::move(f));
}
/*RUSTYCPP:GEN-END id=reactor.36*/

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
        verify(fiber_status != FiberStatus::FINISHED && fiber_status != FiberStatus::RECYCLED);
        (*fiber).yield_();
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.13 version=1 rust_sha256=a41f3e2ea0cb9654b784b3f6ce13bc3481ccb87368385658fa6476f1388b4b5f*/
template<typename W>
void event_wait_impl(const W& ev, uint64_t timeout);

template<typename W>
void event_wait_impl(const W& ev, uint64_t timeout) {
    verify(sp_reactor_th_.is_some());
    const auto reactor_th = rusty::clone(sp_reactor_th_.as_ref().unwrap());
    verify((rusty::detail::deref_if_pointer_like(reactor_th)).thread_id_.get() == rusty::thread::current_id());
    if (ev.status_.get() == rusty::clone(EventStatus_DONE())) {
        return;
    }
    if (rusty::deref_call(ev, rusty::detail::__mdisp_is_ready{})) {
        ev.status_.set(rusty::clone(rusty::clone(EventStatus_DONE())));
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
        ev.status_.set(rusty::clone(rusty::clone(EventStatus_WAIT())));
        const auto fiber_status = (rusty::detail::deref_if_pointer_like(fiber)).status_.get();
        verify((rusty::detail::deref_if_pointer_like(fiber_status) != rusty::clone(FiberStatus_FINISHED())) && (rusty::detail::deref_if_pointer_like(fiber_status) != rusty::clone(FiberStatus_RECYCLED())));
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
            log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("event status ready, triggered?"));
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
/*RUSTYCPP:GEN-BEGIN id=reactor.12 version=1 rust_sha256=d895a772427dbef023fede7843e9b6b2e09b6dfba0e592ec3936dea9528d820b*/
template<typename W>
bool event_test_impl(const W& ev);

template<typename W>
bool event_test_impl(const W& ev) {
    verify(ev.state_.__debug_creator);
    if (rusty::deref_call(ev, rusty::detail::__mdisp_is_ready{})) {
        if (ev.status_.get() == rusty::clone(EventStatus_INIT())) {
            ev.status_.set(rusty::clone(rusty::clone(EventStatus_DONE())));
        } else if (ev.status_.get() == rusty::clone(EventStatus_WAIT())) {
            if (rusty::thread::current_id() == rusty::detail::deref_if_pointer_like(ev.owner_thread_)) {
                const auto option_fiber = rusty::deref_call(rusty::borrow(ev.state_.wp_fiber_), rusty::detail::__mdisp_upgrade{});
                verify(option_fiber.is_some());
                verify(ev.status_.get() != rusty::clone(EventStatus_DEBUG()));
            }
            ev.status_.set(rusty::clone(rusty::clone(EventStatus_READY())));
        } else if (ev.status_.get() == rusty::clone(EventStatus_READY())) {
            log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("event status ready, triggered?"));
        } else if (ev.status_.get() == rusty::clone(EventStatus_DONE())) {
        } else if (ev.status_.get() == rusty::clone(EventStatus_TIMEOUT())) {
        } else {
            verify(0);
        }
        return true;
    } else {
        if (ev.status_.get() == rusty::clone(EventStatus_DONE())) {
            ev.status_.set(rusty::clone(rusty::clone(EventStatus_INIT())));
        }
    }
    return false;
}
/*RUSTYCPP:GEN-END id=reactor.12*/



// Flattened-struct factories (declared next to event_make): each
// replicates the legacy Event constructor's seeding — wait_place_ tag and
// the creating-fiber capture — on top of the aggregate's zero state, plus
// the type's own defaults. Field order matches the DSL struct exactly.
#if RUSTYCPP_RUST
fn event_core_get_fiber_id() -> u64 {
    let fiber_opt = Fiber::current_fiber();
    verify(fiber_opt.is_some());
    (*fiber_opt.unwrap()).id.get()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.25 version=1 rust_sha256=beec5a65763d42205ddcfde216c55be9da3ff7812ef948ff24dc88bcd6221196*/
uint64_t event_core_get_fiber_id();

uint64_t event_core_get_fiber_id() {
    auto fiber_opt = Fiber::current_fiber();
    verify(fiber_opt.is_some());
    return (rusty::detail::deref_if_pointer_like(fiber_opt.unwrap())).id.get();
}
/*RUSTYCPP:GEN-END id=reactor.25*/

// Seeds an event's EventState (wait_place_ tag + creating-fiber weak
// capture), matching the legacy Event constructor. The tag goes through
// format! (a bare &str literal would lower to string_view, which
// std::string does not assign from).
#if RUSTYCPP_RUST
fn event_state_seed(st: &EventState) {
    {
        let mut g = st.wait_place_.borrow_mut();
        *g = format!("not recorded");
    }
    let fiber_opt = Fiber::current_fiber();
    if fiber_opt.is_some() {
        let rc_fiber = fiber_opt.unwrap();
        let mut g2 = st.wp_fiber_.borrow_mut();
        *g2 = rusty::port::rc::Rc::<Fiber>::downgrade(rc_fiber);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.24 version=1 rust_sha256=eb6007bf8e08f78bbb50caa981b4f406659649504695dce0c3454539b9066f51*/
void event_state_seed(const EventState& st) {
    {
        auto&& g = st.wait_place_.borrow_mut();
        rusty::detail::deref_if_pointer_like(g) = std::format("not recorded");
    }
    auto fiber_opt = Fiber::current_fiber();
    if (fiber_opt.is_some()) {
        auto rc_fiber = fiber_opt.unwrap();
        auto&& g2 = st.wp_fiber_.borrow_mut();
        rusty::detail::deref_if_pointer_like(g2) = rusty::port::rc::Rc<Fiber>::downgrade(std::move(rc_fiber));
    }
}
/*RUSTYCPP:GEN-END id=reactor.24*/

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
    auto sp = rusty::Arc<NeverEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>());
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
    auto sp = rusty::Arc<TimeoutEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), Time::now(true) + rusty::detail::deref_if_pointer_like(wait_us), std::move(wait_us));
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
    auto sp = rusty::Arc<IntEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<int32_t>::new_(std::move(target)));
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
    auto sp = rusty::Arc<WaitAny>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), std::move(events));
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
    auto sp = rusty::Arc<WaitAll>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::RefCell<rusty::Vec<rusty::Arc<EventPollable>>>());
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
    auto sp = rusty::Arc<WaitAll>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::RefCell<rusty::Vec<rusty::Arc<EventPollable>>>(std::move(events)));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}
/*RUSTYCPP:GEN-END id=reactor.waitall_make_from*/



// Publish the new value and wake every registered waiter whose target
// is now satisfied. Indexed sweep (the DSL guard-indexing idiom); the
// Arc element needs the explicit deref for method dispatch.
#if RUSTYCPP_RUST
fn shared_int_event_set(sie: &mut SharedIntEvent, v: i32) -> i32 {
    let ret: i32 = sie.value_;
    sie.value_ = v;
    let mut i: usize = 0usize;
    while i < sie.events_.len() {
        let ev: &rusty::Arc<IntEvent> = &sie.events_[i];
        if (*ev).status_.get() <= EventStatus::WAIT {
            if (*ev).target_.get() <= v {
                (*ev).set(v);
            }
        }
        i += 1usize;
    }
    ret
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.28 version=1 rust_sha256=965396c9d89418791931baf8043398d878a1754e69b139ca9236e131073780c6*/
int32_t shared_int_event_set(SharedIntEvent& sie, int32_t v) {
    SharedIntEvent* sie_shadow1 = &sie;
    int32_t ret = (*sie_shadow1).value_;
    (*sie_shadow1).value_ = std::move(v);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len((*sie_shadow1).events_)) {
        const rusty::Arc<IntEvent>& ev = (*sie_shadow1).events_[i];
        if ((rusty::detail::deref_if_pointer_like(ev)).status_.get() <= rusty::clone(EventStatus_WAIT())) {
            if ((rusty::detail::deref_if_pointer_like(ev)).target_.get() <= rusty::detail::deref_if_pointer_like(v)) {
                ((rusty::detail::deref_if_pointer_like(ev))).set(std::move(v));
            }
        }
        i += static_cast<size_t>(1);
    }
    return std::move(ret);
}
/*RUSTYCPP:GEN-END id=reactor.28*/

// @unsafe - Arc handle-method raw extraction for the retain identity
// compare below (`.get()` on the handle is misrouted to the pointee by
// the DSL autoderef — same reason as sconn_proxy_ptr). `&raw const *ev`
// reaches the same address without naming `get()`: it lowers to
// `&deref_if_pointer_like(ev)`. The only difference from `.get()` is the
// debug-build assert Arc::operator* carries for a null handle, and every
// Arc reaching here is a live element of `SharedIntEvent::events_`.
#if RUSTYCPP_RUST
fn int_event_raw_ptr(ev: &rusty::Arc<IntEvent>) -> *const IntEvent {
    let p: *const IntEvent = &raw const *ev;
    p
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.63 version=1 rust_sha256=f31e9221e5401669b8df00dff69caa47af913b99b4cd9a78f9a3b01f56824269*/
const IntEvent* int_event_raw_ptr(const rusty::Arc<IntEvent>& ev) {
    const IntEvent* p = &rusty::detail::deref_if_pointer_like(ev);
    return p;
}
/*RUSTYCPP:GEN-END id=reactor.63*/

// Threshold wait: register a fresh IntEvent, park with a timeout, then
// drop it from the waiter list by pointer identity (the Arc keeps the
// target alive across the retain). Returns whether the wait timed out.
#if RUSTYCPP_RUST
fn shared_int_event_wait_until_gte(sie: &mut SharedIntEvent, x: i32, timeout: i32) -> bool {
    if sie.value_ >= x {
        return false;
    }
    let ev: rusty::Arc<IntEvent> = create_sp_int_event(1);
    (*ev).value_.set(sie.value_);
    (*ev).target_.set(x);
    sie.events_.push(ev.clone());
    (*ev).wait_timeout(timeout as u64);
    // Remove the event from the waiter list once it reaches a terminal
    // state (READY or TIMEOUT).
    let if_timeout: bool = (*ev).status_.get() == EventStatus::TIMEOUT;
    let ev_ptr: *const IntEvent = int_event_raw_ptr(ev);
    sie.events_.retain(move |item: &rusty::Arc<IntEvent>| {
        int_event_raw_ptr(item) != ev_ptr
    });
    if_timeout
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.30 version=1 rust_sha256=7a3e478f8b9a7ff8bb7bc3386753f95ef210c8498d5080d3b3e5e8a1e79f19bc*/
bool shared_int_event_wait_until_gte(SharedIntEvent& sie, int32_t x, int32_t timeout) {
    if (rusty::detail::deref_if_pointer_like(sie.value_) >= rusty::detail::deref_if_pointer_like(x)) {
        return false;
    }
    const rusty::Arc<IntEvent> ev = create_sp_int_event(1);
    (rusty::detail::deref_if_pointer_like(ev)).value_.set(sie.value_);
    (rusty::detail::deref_if_pointer_like(ev)).target_.set(std::move(x));
    sie.events_.push(rusty::clone(ev));
    ((rusty::detail::deref_if_pointer_like(ev))).wait_timeout(static_cast<uint64_t>(timeout));
    bool if_timeout = (rusty::detail::deref_if_pointer_like(ev)).status_.get() == rusty::clone(EventStatus_TIMEOUT());
    const IntEvent* ev_ptr = int_event_raw_ptr(std::move(ev));
    sie.events_.retain([=, ev_ptr = std::move(ev_ptr)](const rusty::Arc<IntEvent>& item) {
return int_event_raw_ptr(item) != ev_ptr;
});
    return std::move(if_timeout);
}
/*RUSTYCPP:GEN-END id=reactor.30*/

// Custom-predicate wait: register a fresh IntEvent carrying the test
// Function and park on it. Probe result recorded here: an explicit
// template argument on a variadic factory (`reactor_create_sp_event::
// <IntEvent>()`) DOES lower from the DSL.
#if RUSTYCPP_RUST
fn shared_int_event_wait(sie: &mut SharedIntEvent, f: EventTestFn) {
    if f(sie.value_) {
        return;
    }
    let ev: rusty::Arc<IntEvent> = create_sp_int_event(1);
    (*ev).value_.set(sie.value_);
    {
        let mut guard = (*ev).state_.test_.borrow_mut();
        *guard = f;
    }
    sie.events_.push(ev.clone());
    (*ev).wait();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.29 version=1 rust_sha256=2996698930e173fb8f9fc7f062891fd02a200463cbbb6bfc2ab0d2fde7323b89*/
void shared_int_event_wait(SharedIntEvent& sie, EventTestFn f) {
    if (f(sie.value_)) {
        return;
    }
    const rusty::Arc<IntEvent> ev = create_sp_int_event(1);
    (rusty::detail::deref_if_pointer_like(ev)).value_.set(sie.value_);
    {
        auto&& guard = (rusty::detail::deref_if_pointer_like(ev)).state_.test_.borrow_mut();
        rusty::detail::deref_if_pointer_like(guard) = std::move(f);
    }
    sie.events_.push(rusty::clone(ev));
    ((rusty::detail::deref_if_pointer_like(ev))).wait();
}
/*RUSTYCPP:GEN-END id=reactor.29*/


// --- from fiber_impl.cc --------------------------------------------------


// @unsafe { each body dereferences a raw pointer the caller owns }
// The rest of the guard surgery is DSL now. Raw-pointer params because a
// DSL `&fb.field` argument lowers to a pointer at the call site, so the
// call sites in fiber_run_wrapper / fiber_run / fiber_do_* are unchanged.
#if RUSTYCPP_RUST
fn fiber_fn_present(f: *const rusty::RefCell<FiberFn>) -> bool {
    let g = (*f).borrow();
    !(*g).is_empty()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.65 version=1 rust_sha256=c0efda8bea4b18acb6ef2f3deaee3794042665a2bcc0fa4aee078426c9afa88e*/
bool fiber_fn_present(const rusty::RefCell<FiberFn>* f) {
    const auto g = ((*f)).borrow();
    return rusty::detail::rust_not(rusty::is_empty(((*g))));
}
/*RUSTYCPP:GEN-END id=reactor.65*/

#if RUSTYCPP_RUST
fn fiber_fn_invoke(f: *const rusty::RefCell<FiberFn>) {
    // borrow_mut: rusty::Function::operator() is non-const.
    let mut g = (*f).borrow_mut();
    (*g)();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.66 version=1 rust_sha256=ff1a5dbdd95a3e94dd059a5cfdcc97eb15652bc3928f501e7e82b8d005402230*/
void fiber_fn_invoke(const rusty::RefCell<FiberFn>* f) {
    auto g = ((*f)).borrow_mut();
    (*g)();
}
/*RUSTYCPP:GEN-END id=reactor.66*/

#if RUSTYCPP_RUST
fn fiber_fn_clear(f: *const rusty::RefCell<FiberFn>) {
    let mut g = (*f).borrow_mut();
    let mut empty: FiberFn = Default::default();
    *g = empty;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.67 version=1 rust_sha256=e4e297199652b3cd0968ce2eb3e1d2bb6426be03117bb5302e36615a86179c87*/
void fiber_fn_clear(const rusty::RefCell<FiberFn>* f) {
    auto g = ((*f)).borrow_mut();
    FiberFn empty = rusty::default_like<FiberFn>();
    *g = std::move(empty);
}
/*RUSTYCPP:GEN-END id=reactor.67*/

#if RUSTYCPP_RUST
fn fiber_install_task(t: *const rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>,
                      task: FiberTaskFn) {
    // fiber_task_t's ctor RUNS the body up to its first yield, so the box
    // must be built BEFORE the borrow is taken (C++17 sequences the RHS of
    // `*t->borrow_mut() = ...` before the LHS; binding the guard first
    // would newly hold a borrow across fiber execution).
    let mut boxed = rusty::Some(rusty::make_box::<fiber_task_t>(task));
    let mut g = (*t).borrow_mut();
    *g = boxed;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.68 version=1 rust_sha256=b82e41644f3b754691aad6f542675950097eb921d586b196e1fcdfa10cd30fbe*/
void fiber_install_task(const rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>* t, FiberTaskFn task) {
    auto boxed = rusty::Some(rusty::make_box<fiber_task_t>(std::move(task)));
    auto g = ((*t)).borrow_mut();
    *g = std::move(boxed);
}
/*RUSTYCPP:GEN-END id=reactor.68*/

#if RUSTYCPP_RUST
fn fiber_task_invoke(t: *const rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>) {
    let mut g = (*t).borrow_mut();
    let bx: &mut rusty::Box<fiber_task_t> = (*g).as_mut().unwrap();
    (*bx)();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.69 version=1 rust_sha256=4318c29076a0199bf53b61afdcffc7fe1c79c95fb1fab3d2d459726ad2ab4b11*/
void fiber_task_invoke(const rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>* t);

void fiber_task_invoke(const rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>>* t) {
    auto g = ((*t)).borrow_mut();
    rusty::Box<fiber_task_t>& bx = ((*g)).as_mut().unwrap();
    (rusty::detail::deref_if_pointer_like(bx))();
}
/*RUSTYCPP:GEN-END id=reactor.69*/

#if RUSTYCPP_RUST
fn fiber_yield_invoke_ptr(y: *mut fiber_yield_t) {
    fiber_yield_invoke(*y);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.70 version=1 rust_sha256=322012d3d1ecaddd184094e9b1aacc4ed9e1cca9a866d40048a13ad72b7253d0*/
void fiber_yield_invoke_ptr(fiber_yield_t* y);

void fiber_yield_invoke_ptr(fiber_yield_t* y) {
    fiber_yield_invoke(*y);
}
/*RUSTYCPP:GEN-END id=reactor.70*/

// Reactor-touching helpers as DSL free fns (Reactor is complete here;
// fresh get_reactor() per call also dodges the Rc-in-loop last-use-move
// trap a bound handle would hit inside fiber_run_wrapper's loop).
#if RUSTYCPP_RUST
fn reactor_live_fiber_count() -> usize {
    let reactor = Reactor::get_reactor();
    let guard = (*reactor).fibers_.borrow();
    (*guard).size()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.reactor_live_fiber_count version=1 rust_sha256=620e2f989d54a2b33a85849d92e02ef3b47c3b9dd08f258a76e826516513f5ad*/
size_t reactor_live_fiber_count();

size_t reactor_live_fiber_count() {
    const auto reactor = Reactor::get_reactor();
    auto&& guard = rusty::borrow((rusty::detail::deref_if_pointer_like(reactor)).fibers_);
    return ((rusty::detail::deref_if_pointer_like(guard))).size();
}
/*RUSTYCPP:GEN-END id=reactor.reactor_live_fiber_count*/

#if RUSTYCPP_RUST
fn reactor_dec_active_fibers() {
    let reactor = Reactor::get_reactor();
    (*reactor).n_active_fibers_.set((*reactor).n_active_fibers_.get() - 1i64);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.reactor_dec_active_fibers version=1 rust_sha256=ec0388986b5f23fe11ea5174d9e807175554690e7cb8e61151291a9a6ec758d5*/
void reactor_dec_active_fibers();

void reactor_dec_active_fibers() {
    const auto reactor = Reactor::get_reactor();
    (rusty::detail::deref_if_pointer_like(reactor)).n_active_fibers_.set((rusty::detail::deref_if_pointer_like(reactor)).n_active_fibers_.get() - static_cast<int64_t>(1));
}
/*RUSTYCPP:GEN-END id=reactor.reactor_dec_active_fibers*/

// The task body driven by the C engine: stash the yield handle, then
// invoke the user closure; on completion mark FINISHED, decrement the
// active count, and yield back for possible recycling (the loop re-runs
// a recycled fiber's new closure on the next continue_).
#if RUSTYCPP_RUST
fn fiber_run_wrapper(fb: &Fiber, y: *mut fiber_yield_t) {
    fb.fiber_yield_.set(y);
    verify(fiber_fn_present(&fb.func_));
    loop {
        let sz = reactor_live_fiber_count();
        verify(sz > 0usize);
        verify(fiber_fn_present(&fb.func_));
        fiber_fn_invoke(&fb.func_);
        fiber_fn_clear(&fb.func_);
        fb.status_.set(FiberStatus::FINISHED);
        if fb.needs_finalize_.get() {
            log_line(Log::INFO, 0i32, core::ptr::null(), std::format("Warning: We did not deal with backlog issues"));
            fb.needs_finalize_.set(false);
        }
        reactor_dec_active_fibers();
        fiber_yield_invoke_ptr(y);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_run_wrapper version=1 rust_sha256=54973f94e8e72901f128839ad3bb6ba50c6e80cac4e111700df5632f82746c5e*/
void fiber_run_wrapper(const Fiber& fb, fiber_yield_t* y) {
    fb.fiber_yield_.set(std::move(y));
    verify(fiber_fn_present(&fb.func_));
    while (true) {
        const auto sz = reactor_live_fiber_count();
        verify(rusty::detail::deref_if_pointer_like(sz) > static_cast<size_t>(0));
        verify(fiber_fn_present(&fb.func_));
        fiber_fn_invoke(&fb.func_);
        fiber_fn_clear(&fb.func_);
        fb.status_.set(rusty::clone(rusty::clone(FiberStatus_FINISHED())));
        if (fb.needs_finalize_.get()) {
            log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Warning: We did not deal with backlog issues"));
            fb.needs_finalize_.set(false);
        }
        reactor_dec_active_fibers();
        fiber_yield_invoke_ptr(y);
    }
}
/*RUSTYCPP:GEN-END id=reactor.fiber_run_wrapper*/

// Start the fiber: install the task closure (fiber_task_t runs the body
// immediately on construction — the C engine's first resume mirrors the
// old Boost pull_type behavior).
#if RUSTYCPP_RUST
fn fiber_run(fb: &Fiber) {
    {
        let tguard = fb.fiber_task_.borrow();
        verify((*tguard).is_none());
    }
    verify(fb.status_.get() == FiberStatus::INIT);
    fb.status_.set(FiberStatus::STARTED);
    let sz = reactor_live_fiber_count();
    verify(sz > 0usize);
    // The closure only reads through this pointer; keep the constness instead
    // of manufacturing a mutable pointer with a const-removal kernel.
    let self_ptr: *const Fiber = &raw const *fb;
    let mut task: FiberTaskFn = move |yy: &mut fiber_yield_t| {
        unsafe { fiber_run_wrapper(&*self_ptr, &raw mut *yy); }
    };
    fiber_install_task(&fb.fiber_task_, task);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_run version=1 rust_sha256=e5e72f67d7b00599414d86d10a5577fe1d8dfb486f4489d929c1a732ae97c45b*/
void fiber_run(const Fiber& fb) {
    {
        auto&& tguard = rusty::borrow(fb.fiber_task_);
        verify(((rusty::detail::deref_if_pointer_like(tguard))).is_none());
    }
    verify(fb.status_.get() == rusty::clone(FiberStatus_INIT()));
    fb.status_.set(rusty::clone(rusty::clone(FiberStatus_STARTED())));
    const auto sz = reactor_live_fiber_count();
    verify(rusty::detail::deref_if_pointer_like(sz) > static_cast<size_t>(0));
    const Fiber* self_ptr = &fb;
    FiberTaskFn task = [=, self_ptr = std::move(self_ptr)](fiber_yield_t& yy) {
// @unsafe
{
    fiber_run_wrapper(*self_ptr, &yy);
}
};
    fiber_install_task(&fb.fiber_task_, std::move(task));
}
/*RUSTYCPP:GEN-END id=reactor.fiber_run*/

#if RUSTYCPP_RUST
fn fiber_do_yield(fb: &Fiber) {
    let y: *mut fiber_yield_t = fb.fiber_yield_.get();
    verify(!y.is_null());
    let s = fb.status_.get();
    verify(s == FiberStatus::STARTED || s == FiberStatus::RESUMED
        || s == FiberStatus::FINALIZING);
    fb.status_.set(FiberStatus::PAUSED);
    reactor_dec_active_fibers();
    fiber_yield_invoke_ptr(y);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_do_yield version=1 rust_sha256=96e91d076d9786f6f12d4fba819ea3ad582b1549bd2c4f8aa93cbaaff5d8a1ca*/
void fiber_do_yield(const Fiber& fb) {
    fiber_yield_t* const y = fb.fiber_yield_.get();
    verify(rusty::detail::rust_not((y == nullptr)));
    const auto s = fb.status_.get();
    verify(((rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_STARTED())) || (rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_RESUMED()))) || (rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_FINALIZING())));
    fb.status_.set(rusty::clone(rusty::clone(FiberStatus_PAUSED())));
    reactor_dec_active_fibers();
    fiber_yield_invoke_ptr(y);
}
/*RUSTYCPP:GEN-END id=reactor.fiber_do_yield*/

#if RUSTYCPP_RUST
fn fiber_do_continue(fb: &Fiber) {
    let s = fb.status_.get();
    verify(s == FiberStatus::PAUSED || s == FiberStatus::RECYCLED);
    {
        let tguard = fb.fiber_task_.borrow();
        verify((*tguard).is_some());
    }
    fb.status_.set(FiberStatus::RESUMED);
    fiber_task_invoke(&fb.fiber_task_);
    // some events might have been triggered from last fiber,
    // but you have to manually call the scheduler to loop.
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_do_continue version=1 rust_sha256=56342c24888c2de5ee84c086ca431c815bc725377b5ab265d1a597e70b5d5590*/
void fiber_do_continue(const Fiber& fb) {
    const auto s = fb.status_.get();
    verify((rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_PAUSED())) || (rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_RECYCLED())));
    {
        auto&& tguard = rusty::borrow(fb.fiber_task_);
        verify(((rusty::detail::deref_if_pointer_like(tguard))).is_some());
    }
    fb.status_.set(rusty::clone(rusty::clone(FiberStatus_RESUMED())));
    fiber_task_invoke(&fb.fiber_task_);
}
/*RUSTYCPP:GEN-END id=reactor.fiber_do_continue*/

#if RUSTYCPP_RUST
fn fiber_is_finished(fb: &Fiber) -> bool {
    let s = fb.status_.get();
    s == FiberStatus::FINISHED || s == FiberStatus::RECYCLED
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_is_finished version=1 rust_sha256=3e09f5a973271dd7f2b023950bc6a4842dec3b515088729332087c2cdf6da192*/
bool fiber_is_finished(const Fiber& fb) {
    const auto s = fb.status_.get();
    return (rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_FINISHED())) || (rusty::detail::deref_if_pointer_like(s) == rusty::clone(FiberStatus_RECYCLED()));
}
/*RUSTYCPP:GEN-END id=reactor.fiber_is_finished*/

#if RUSTYCPP_RUST
fn fiber_do_finalize(fb: &Fiber) {
    fb.needs_finalize_.set(false);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.fiber_do_finalize version=1 rust_sha256=0621e502b36d605b91d9b94bfeb2f47f986986080c841830d51cebcbe9f74cc6*/
void fiber_do_finalize(const Fiber& fb) {
    fb.needs_finalize_.set(false);
}
/*RUSTYCPP:GEN-END id=reactor.fiber_do_finalize*/




// --- from reactor.cc -----------------------------------------------------

// `REUSING_FIBER` is provided as a macro by reactor.h (line 203).
// The original module-attached `constexpr bool REUSING_FIBER`
// shadowed the macro inside the rrr module's purview; with
// de-modularization (header-textual inclusion) the macro now
// expands at parse time and the constexpr is redundant — the
// existing call sites in this TU (lines below) consume the macro
// directly.

namespace {

// One-line bridge for libc's actual C `char`; Rust `char` is a four-byte
// Unicode scalar and therefore cannot name getenv's pointer type.
using c_char = char;

// @unsafe { getenv returns a borrowed raw process-environment pointer }
// Preserve the old first-byte policy exactly: absent, empty, or leading '0'
// disables profiling; every other non-empty value enables it.
#if RUSTYCPP_RUST
fn stackless_profile_env() -> bool {
    let env: *const c_char = unsafe { getenv("MAKO_ASYNC_PROFILE") };
    if env.is_null() {
        return false;
    }
    unsafe { *env != 0 as c_char && *env != 48 as c_char }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.73 version=1 rust_sha256=e74d57eb6b466eee0dafc670684476fc3696dc02d169028193fe565e822c08d5*/
bool stackless_profile_env();

bool stackless_profile_env() {
    const c_char* env = getenv("MAKO_ASYNC_PROFILE");
    if ((env == nullptr)) {
        return false;
    }
    // @unsafe
    {
        return (*env != (static_cast<c_char>(0))) && (*env != (static_cast<c_char>(48)));
    }
}
/*RUSTYCPP:GEN-END id=reactor.73*/

// The "function-local static" blocker on this function has expired: a
// fn-body `static` lowers to a real C++ magic static
// (`static bool ENABLED = stackless_profile_env();`), so the lazy,
// thread-safe, init-once semantics every profile shim depends on are
// preserved exactly -- the only change is that the initializer is now a
// named kernel instead of an inline lambda.
//
// LOAD-BEARING: the body must end with the bare name as a TAIL
// EXPRESSION. Spelling `return ENABLED;` emits
// `return std::move(ENABLED);`, which moves out of a process-lifetime
// object and guts it after the first call.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn stackless_profile_enabled() -> bool {
    static ENABLED: bool = stackless_profile_env();
    ENABLED
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.53 version=1 rust_sha256=c5435df018cfed2e1aa41c03d34c24c2e948cadb18f1a2df6b6628266b3f3255*/
bool stackless_profile_enabled();

bool stackless_profile_enabled() {
    static bool ENABLED = stackless_profile_env();
    return ENABLED;
}
/*RUSTYCPP:GEN-END id=reactor.53*/

// Type aliases for the profile counters (the DSL grammar can't parse a
// `std::atomic<...>` template-id in field position, so the struct names
// these -- same pattern as Server's `ServerPendingRequestsAtomic`).
//
// These are rusty atomics rather than `std::atomic` so that the max-slots
// update is a single `fetch_max` call: `rusty::sync::atomic::Atomic<T>`
// already implements fetch_max AS the compare-exchange loop, and the DSL
// cannot emit a hand-written one (the in/out `expected` argument gets
// std::move()d). Consequently every read/write below passes
// `rusty::sync::atomic::Ordering::*`, not `std::memory_order_*`. Each
// counter default-constructs to 0, and `g_stackless_profile` has static
// storage duration, so it is zero-initialized before any dynamic
// initializer can observe it.
using StacklessProfileCountU64 = rusty::sync::atomic::AtomicU64;
using StacklessProfileCountUsize = rusty::sync::atomic::AtomicUsize;

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

// Atomics provide interior mutability, so the Rust binding itself need not be
// `mut`. Explicit zero initializers preserve the former static-storage state.
static g_stackless_profile: StacklessProfileCounters = StacklessProfileCounters {
    reg_calls: rusty::sync::atomic::AtomicU64::new(0u64),
    reg_scan_steps: rusty::sync::atomic::AtomicU64::new(0u64),
    reg_reuse: rusty::sync::atomic::AtomicU64::new(0u64),
    reg_new: rusty::sync::atomic::AtomicU64::new(0u64),
    poll_calls: rusty::sync::atomic::AtomicU64::new(0u64),
    poll_ready: rusty::sync::atomic::AtomicU64::new(0u64),
    enqueue_calls: rusty::sync::atomic::AtomicU64::new(0u64),
    max_slots: rusty::sync::atomic::AtomicUsize::new(0usize),
};
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.stackless_profile version=1 rust_sha256=46e4c9a9ec86f93311e77b7ca40217b25ba1e962497d9350fb5bf36b52f86841*/
struct StacklessProfileCounters;
extern StacklessProfileCounters g_stackless_profile;

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

inline StacklessProfileCounters g_stackless_profile = StacklessProfileCounters{.reg_calls = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .reg_scan_steps = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .reg_reuse = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .reg_new = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .poll_calls = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .poll_ready = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .enqueue_calls = rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), .max_slots = rusty::sync::atomic::AtomicUsize::new_(static_cast<size_t>(0))};
/*RUSTYCPP:GEN-END id=reactor.stackless_profile*/

// The 7-line compare_exchange_weak loop collapsed into one call:
// `rusty::sync::atomic::Atomic<T>::fetch_max` IS that CAS loop.
#if RUSTYCPP_RUST
fn stackless_profile_update_max_slots(slots: usize) {
    g_stackless_profile.max_slots.fetch_max(slots, rusty::sync::atomic::Ordering::Relaxed);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.54 version=1 rust_sha256=fe02a3d961397f029359c037da46b3e9465c25970a79dc891adf2c15af0742d4*/
void stackless_profile_update_max_slots(size_t slots);

void stackless_profile_update_max_slots(size_t slots) {
    g_stackless_profile.max_slots.fetch_max(std::move(slots), rusty::sync::atomic::Ordering::Relaxed);
}
/*RUSTYCPP:GEN-END id=reactor.54*/

// The "function-local static" blocker on this function has expired: a
// fn-body `#[thread_local] static mut` lowers to a real
// `static thread_local uint64_t last_report_us = 0;`.
//
// LOAD-BEARING: the transpiler HOISTS that declaration to the top of the
// emitted body, ABOVE the `stackless_profile_enabled()` guard. That is
// harmless here only because the initializer is the constant `0` --
// constant-initialized TLS, so nothing dynamic runs on the disabled
// path. Do not give this static a non-constant initializer.
//
// The counters are rusty atomics, so the loads below are ordinary
// `Ordering::Relaxed` calls (spelled with the full path, as everywhere
// else in this file). The `static_cast<unsigned long long>` wrappers on
// the log arguments are gone: they existed only to feed a printf-style
// `%llu`, and `std::format` renders `uint64_t` / `size_t` identically
// without them.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn stackless_profile_report_periodic() {
    if !stackless_profile_enabled() {
        return;
    }
    #[thread_local] static mut last_report_us: u64 = 0;
    let now_us: u64 = Time::now(true);
    if last_report_us == 0u64 {
        last_report_us = now_us;
        return;
    }
    if now_us - last_report_us < 1000000u64 {
        return;
    }
    last_report_us = now_us;

    let reg_calls: u64 = g_stackless_profile.reg_calls.load(rusty::sync::atomic::Ordering::Relaxed);
    let reg_scans: u64 = g_stackless_profile.reg_scan_steps.load(rusty::sync::atomic::Ordering::Relaxed);
    let reg_reuse: u64 = g_stackless_profile.reg_reuse.load(rusty::sync::atomic::Ordering::Relaxed);
    let reg_new: u64 = g_stackless_profile.reg_new.load(rusty::sync::atomic::Ordering::Relaxed);
    let poll_calls: u64 = g_stackless_profile.poll_calls.load(rusty::sync::atomic::Ordering::Relaxed);
    let poll_ready: u64 = g_stackless_profile.poll_ready.load(rusty::sync::atomic::Ordering::Relaxed);
    let enqueue_calls: u64 = g_stackless_profile.enqueue_calls.load(rusty::sync::atomic::Ordering::Relaxed);
    let max_slots: usize = g_stackless_profile.max_slots.load(rusty::sync::atomic::Ordering::Relaxed);

    let mut avg_scan: f64 = 0.0f64;
    if reg_calls > 0u64 {
        avg_scan = (reg_scans as f64) / (reg_calls as f64);
    }
    log_line(Log::INFO, 0i32, core::ptr::null(), std::format("[async-prof] reg_calls={} avg_scan={:.2f} reuse={} new={} max_slots={} poll_calls={} poll_ready={} enqueue_calls={}",
        reg_calls, avg_scan, reg_reuse, reg_new, max_slots, poll_calls, poll_ready, enqueue_calls));
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.57 version=1 rust_sha256=4b7ceb069de858bd99d79a7d0d65fbe5c8c6181c6218199eb95000ecf9ddf533*/
void stackless_profile_report_periodic();

void stackless_profile_report_periodic() {
    static thread_local uint64_t last_report_us = static_cast<uint64_t>(0);
    if (rusty::detail::rust_not(stackless_profile_enabled())) {
        return;
    }
    uint64_t now_us = Time::now(true);
    if (rusty::detail::deref_if_pointer_like(last_report_us) == static_cast<uint64_t>(0)) {
        last_report_us = std::move(now_us);
        return;
    }
    if ((rusty::detail::deref_if_pointer_like(now_us) - rusty::detail::deref_if_pointer_like(last_report_us)) < static_cast<uint64_t>(1000000)) {
        return;
    }
    last_report_us = std::move(now_us);
    const uint64_t reg_calls = g_stackless_profile.reg_calls.load(rusty::sync::atomic::Ordering::Relaxed);
    const uint64_t reg_scans = g_stackless_profile.reg_scan_steps.load(rusty::sync::atomic::Ordering::Relaxed);
    const uint64_t reg_reuse = g_stackless_profile.reg_reuse.load(rusty::sync::atomic::Ordering::Relaxed);
    const uint64_t reg_new = g_stackless_profile.reg_new.load(rusty::sync::atomic::Ordering::Relaxed);
    const uint64_t poll_calls = g_stackless_profile.poll_calls.load(rusty::sync::atomic::Ordering::Relaxed);
    const uint64_t poll_ready = g_stackless_profile.poll_ready.load(rusty::sync::atomic::Ordering::Relaxed);
    const uint64_t enqueue_calls = g_stackless_profile.enqueue_calls.load(rusty::sync::atomic::Ordering::Relaxed);
    const size_t max_slots = g_stackless_profile.max_slots.load(rusty::sync::atomic::Ordering::Relaxed);
    double avg_scan = 0.0;
    if (rusty::detail::deref_if_pointer_like(reg_calls) > static_cast<uint64_t>(0)) {
        avg_scan = ((static_cast<double>(reg_scans))) / ((static_cast<double>(reg_calls)));
    }
    log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[async-prof] reg_calls={} avg_scan={:.2f} reuse={} new={} max_slots={} poll_calls={} poll_ready={} enqueue_calls={}", std::move(reg_calls), std::move(avg_scan), std::move(reg_reuse), std::move(reg_new), std::move(max_slots), std::move(poll_calls), std::move(poll_ready), std::move(enqueue_calls)));
}
/*RUSTYCPP:GEN-END id=reactor.57*/

}  // namespace

// Stackless-profile observability shims for the DSL enqueue / poll /
// register paths (declared, exported, above the Reactor DSL block).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` blocks below are
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` blocks.
//
// LINKAGE (load-bearing): these must stay OUTSIDE the anonymous
// namespace -- inside it the definition has internal linkage and never
// matches the exported declaration (gate32's 108 undefined-reference
// link failures). The transpiler emits each GEN block in place, so
// authoring the DSL here -- after `}  // namespace` -- keeps the
// generated definitions at `rrr` scope with external linkage. The
// anon-namespace `stackless_profile_enabled()`, `g_stackless_profile`,
// `stackless_profile_update_max_slots()` and
// `stackless_profile_report_periodic()` that these bodies read are
// still visible here in-TU.
//
// The counters are rusty atomics now (see the typedefs above), so these
// bodies pass `rusty::sync::atomic::Ordering::Relaxed`; that path
// expression lowers verbatim, exactly as `std::memory_order_relaxed` did.
#if RUSTYCPP_RUST
fn stackless_profile_note_enqueue() {
    if stackless_profile_enabled() {
        g_stackless_profile.enqueue_calls.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.46 version=1 rust_sha256=06a55092e9f41728dad2dc3a288ea59117b71f2fdbb37b1237102433841b4622*/
void stackless_profile_note_enqueue();

void stackless_profile_note_enqueue() {
    if (stackless_profile_enabled()) {
        g_stackless_profile.enqueue_calls.fetch_add(static_cast<uint64_t>(1), rusty::sync::atomic::Ordering::Relaxed);
    }
}
/*RUSTYCPP:GEN-END id=reactor.46*/

// The "Waker/Context wiring the DSL cannot spell" objection expired:
// rusty::Waker and rusty::Context are plain aggregates, so struct literals
// over them lower fine. Two shapes are load-bearing here:
//   * the waker pointer must go through `let wp: *mut rusty::Waker` --
//     writing `rusty::Context { waker: &waker }` silently drops the `&`;
//   * the poll call must go through the named `let ctx_ref: &mut ...`
//     binding -- a bare `&mut ctx` argument lowers to a POINTER, which
//     will not bind the `rusty::Function<bool(rusty::Context&)>` operand.
// The loop and slot bookkeeping around this live in the DSL
// Reactor::process_stackless_tasks.
#if RUSTYCPP_RUST
fn reactor_poll_one(r: &Reactor, idx: usize, poll_fn: *mut StacklessPollFn) -> bool {
    if stackless_profile_enabled() {
        g_stackless_profile.poll_calls.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    }
    // Raw back-pointer, not the reference itself: a `move ||` closure
    // captures BY VALUE and Reactor is non-copyable. The waker never
    // outlives this call.
    let rp: *const Reactor = &raw const r;
    let mut waker = rusty::Waker {
        wake_fn: move || {
            (*rp).enqueue_stackless_task(idx);
        }
    };
    let wp: *mut rusty::Waker = &raw mut waker;
    let mut ctx = rusty::Context { waker: wp };
    let ctx_ref: &mut rusty::Context = &mut ctx;
    (*poll_fn)(ctx_ref)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.56 version=1 rust_sha256=fed3cb716f10732f71866e0f615048ecb66962c1720f5fedc64ab69cfd1336d7*/
bool reactor_poll_one(const Reactor& r, size_t idx, StacklessPollFn* poll_fn) {
    if (stackless_profile_enabled()) {
        g_stackless_profile.poll_calls.fetch_add(static_cast<uint64_t>(1), rusty::sync::atomic::Ordering::Relaxed);
    }
    const Reactor* rp = &r;
    auto waker = rusty::Waker{.wake_fn = [=, idx = std::move(idx), rp = std::move(rp)]() {
((*rp)).enqueue_stackless_task(std::move(idx));
}};
    rusty::Waker* wp = &waker;
    auto ctx = rusty::Context{.waker = wp};
    rusty::Context& ctx_ref = ctx;
    return (*poll_fn)(ctx_ref);
}
/*RUSTYCPP:GEN-END id=reactor.56*/

// Ready-count + periodic-report shims for the DSL poll loop (same
// linkage note as above).
#if RUSTYCPP_RUST
fn stackless_profile_note_poll_ready() {
    if stackless_profile_enabled() {
        g_stackless_profile.poll_ready.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.47 version=1 rust_sha256=f4cdd57b76e6146f1c08a12071df68bc99d29cb07aee642a202ba792a75726e0*/
void stackless_profile_note_poll_ready();

void stackless_profile_note_poll_ready() {
    if (stackless_profile_enabled()) {
        g_stackless_profile.poll_ready.fetch_add(static_cast<uint64_t>(1), rusty::sync::atomic::Ordering::Relaxed);
    }
}
/*RUSTYCPP:GEN-END id=reactor.47*/

#if RUSTYCPP_RUST
fn stackless_profile_report_periodic_shim() {
    stackless_profile_report_periodic();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.48 version=1 rust_sha256=6fbe17a6ea4929baafeb0b27bfa835405ce2ea9ba42814a89f65fc9cc9083645*/
void stackless_profile_report_periodic_shim();

void stackless_profile_report_periodic_shim() {
    stackless_profile_report_periodic();
}
/*RUSTYCPP:GEN-END id=reactor.48*/

// Register-path profile shim (same linkage note as above).
#if RUSTYCPP_RUST
fn stackless_profile_note_register(scanned: usize, reuse: bool, slots_now: usize) {
    if !stackless_profile_enabled() {
        return;
    }
    g_stackless_profile.reg_calls.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    g_stackless_profile.reg_scan_steps.fetch_add(scanned, rusty::sync::atomic::Ordering::Relaxed);
    if reuse {
        g_stackless_profile.reg_reuse.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    } else {
        g_stackless_profile.reg_new.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
        stackless_profile_update_max_slots(slots_now);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.49 version=1 rust_sha256=def337a95497345eac0c06f9ea7a86e9618e4b281386284067f44b8e49167659*/
void stackless_profile_note_register(size_t scanned, bool reuse, size_t slots_now);

void stackless_profile_note_register(size_t scanned, bool reuse, size_t slots_now) {
    if (rusty::detail::rust_not(stackless_profile_enabled())) {
        return;
    }
    g_stackless_profile.reg_calls.fetch_add(static_cast<uint64_t>(1), rusty::sync::atomic::Ordering::Relaxed);
    g_stackless_profile.reg_scan_steps.fetch_add(std::move(scanned), rusty::sync::atomic::Ordering::Relaxed);
    if (reuse) {
        g_stackless_profile.reg_reuse.fetch_add(static_cast<uint64_t>(1), rusty::sync::atomic::Ordering::Relaxed);
    } else {
        g_stackless_profile.reg_new.fetch_add(static_cast<uint64_t>(1), rusty::sync::atomic::Ordering::Relaxed);
        stackless_profile_update_max_slots(std::move(slots_now));
    }
}
/*RUSTYCPP:GEN-END id=reactor.49*/


// sp_reactor_th_ / sp_disk_reactor_th_ / sp_running_fiber_th_ are
// `static inline thread_local` in the class declaration above (vague linkage).
// Same for g_current_poll_worker, clients_, and dangling_ips_.

// @safe - Returns current fiber with single-threaded reference counting
// SAFETY: Returns copy of thread-local Rc - single-threaded, no synchronization needed
// Returns None if called outside of a fiber context
// The static member delegates to the DSL free fn over the hoisted
// namespace thread_local.
#if RUSTYCPP_RUST
fn fiber_current_fiber() -> Option<rusty::Rc<Fiber>> {
    let guard = sp_running_fiber_th_.borrow();
    if (*guard).is_none() {
        return None;
    }
    Some((*guard).as_ref().unwrap().clone())
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.33 version=1 rust_sha256=71161dba394e986d41a6b15746a37443dd636d36f2ffcf039917a38afced2cfa*/
rusty::Option<rusty::Rc<Fiber>> fiber_current_fiber() {
    auto&& guard = rusty::borrow(sp_running_fiber_th_);
    if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
        return rusty::Option<rusty::Rc<Fiber>>{rusty::None};
    }
    return rusty::Option<rusty::Rc<Fiber>>(rusty::clone(((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap()));
}
/*RUSTYCPP:GEN-END id=reactor.33*/

// @unsafe - Creates and runs a new fiber with rusty::Rc ownership.
// The static member delegates to a DSL free fn (same split as
// current_fiber/sleep above). `const char*` is spelled with the file's
// SrcFileCStr alias -- `*const i8` would emit `const int8_t*`, a distinct
// type that will not bind a `const char*` argument.
#if RUSTYCPP_RUST
fn fiber_create_run_impl(func: FiberFn, file: SrcFileCStr, line: i64) -> rusty::Rc<Fiber> {
    let reactor_rc = Reactor::get_reactor();
    reactor_create_run_fiber_at_impl(&*reactor_rc, func, file, line)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.61 version=1 rust_sha256=69c5f6d097c4e68bc8138513fc434512f2e5c73c1d8fc1dd1341245de9bbd346*/
rusty::Rc<Fiber> fiber_create_run_impl(FiberFn func, SrcFileCStr file, int64_t line) {
    const auto reactor_rc = Reactor::get_reactor();
    return reactor_create_run_fiber_at_impl(rusty::detail::deref_if_pointer_like(reactor_rc), std::move(func), std::move(file), std::move(line));
}
/*RUSTYCPP:GEN-END id=reactor.61*/

// The static member delegates to the DSL free fn (§7.59 turbofish
// factory call with an argument).
#if RUSTYCPP_RUST
fn fiber_sleep(microseconds: u64) {
    if microseconds == 0u64 {
        return;
    }
    let x = create_sp_timeout_event(microseconds);
    (*x).wait();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.34 version=1 rust_sha256=d375463f4fbe82a86a5f667b77ed3439090590808a12004e9d95ab81723c08e2*/
void fiber_sleep(uint64_t microseconds);

void fiber_sleep(uint64_t microseconds) {
    if (rusty::detail::deref_if_pointer_like(microseconds) == static_cast<uint64_t>(0)) {
        return;
    }
    const auto x = create_sp_timeout_event(std::move(microseconds));
    ((rusty::detail::deref_if_pointer_like(x))).wait();
}
/*RUSTYCPP:GEN-END id=reactor.34*/

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
// @safe - Rc<Reactor> allocation (the old turbofish+Log mis-lowering
// note no longer applies: the body has no log call).
#if RUSTYCPP_RUST
fn reactor_make() -> Rc<Reactor> {
    Rc::<Reactor>::make()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.40 version=1 rust_sha256=722bb5c897eb18725642565af8d7a584487465db7e74da98ff0a904c488ca70b*/
rusty::Rc<Reactor> reactor_make() {
    return rusty::Rc<Reactor>::make();
}
/*RUSTYCPP:GEN-END id=reactor.40*/
// REUSING_FIBER is a project macro; it survives the lowering as an
// identifier (same as the SHUT_RDWR/EAGAIN idiom in tcp_channel).
#if RUSTYCPP_RUST
fn reactor_log_create(disk: bool) {
    if disk {
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("create a disk fiber scheduler"));
        return;
    }
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("create a fiber scheduler"));
    if !REUSING_FIBER {
        log_line(Log::WARN, 0i32, core::ptr::null(), std::format("reusing fiber not enabled!"));
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.35 version=1 rust_sha256=effe655df364599d8131ba7a66560a7a92a3d2cf5520105fa9a55f960c96d80e*/
void reactor_log_create(bool disk);

void reactor_log_create(bool disk) {
    if (disk) {
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("create a disk fiber scheduler"));
        return;
    }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("create a fiber scheduler"));
    if (rusty::detail::rust_not(REUSING_FIBER)) {
        log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("reusing fiber not enabled!"));
    }
}
/*RUSTYCPP:GEN-END id=reactor.35*/

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

// Gets a recycled fiber or creates a new one. Authored as inline Rust DSL;
// no kernel is needed — the whole body lowers.
//
// Two probe-established points worth keeping:
//  * `available_guard.pop().unwrap()` replaces the old `back().clone()` +
//    `pop()` pair. The ported rustc `Vec::pop()` has Rust semantics (drops
//    the length, `ptr::read`s the last slot out, hands back `Option<T>`), so
//    the element taken and the resulting vector are identical to before —
//    only the transient refcount bump from the clone disappears.
//  * This fn returns a CLASS type, so an unqualified `log_line(Log::DEBUG, 0, nullptr, std::format(...))` is
//    mis-qualified by the lowering as `rusty::Rc<Fiber>::Log_debug`. It is
//    spelled `rrr::Log_debug` for that reason (the counters are logged as
//    i64/i32 now instead of via the old `(int)` / `(long long)` casts —
//    `{}` formats them the same).
// The `self` param is named `self_` so it stays a real parameter instead of
// becoming a receiver, matching the sibling helpers below.
#if RUSTYCPP_RUST
fn reactor_get_or_create_fiber_impl(self_: &Reactor, func: FiberFn, file: SrcFileCStr, line: i64) -> rusty::Rc<Fiber> {
    let mut available_guard = self_.available_fibers_.borrow_mut();
    if REUSING_FIBER && available_guard.len() > 0usize {
        self_.n_idle_fibers_.set(self_.n_idle_fibers_.get() - 1i64);
        let fiber: rusty::Rc<Fiber> = available_guard.pop().unwrap();
        // Cell/RefCell interior mutability re-stamps the recycled fiber
        // through the shared handle (safe: single-threaded).
        (*fiber).id.set(fiber_next_global_id());
        *(*fiber).func_.borrow_mut() = func;
        // Keep the existing task/stack so continue_() can resume from the
        // fiber's yield point.
        verify((*(*fiber).fiber_task_.borrow()).is_some());
        (*fiber).status_.set(FiberStatus::RECYCLED);
        return fiber;
    } else {
        let fiber: rusty::Rc<Fiber> = rusty::Rc::<Fiber>::make(func);
        self_.n_created_fibers_.set(self_.n_created_fibers_.get() + 1i64);
        if self_.n_created_fibers_.get() % 1024i64 == 0i64 {
            unsafe {
                rrr::log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("created {}, busy {}, idle {} fibers on server {}, recent {}:{}",
                               self_.n_created_fibers_.get(),
                               self_.n_busy_fibers_.get(),
                               self_.n_idle_fibers_.get(),
                               self_.server_id_.get(),
                               file,
                               line));
            }
        }
        return fiber;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.55 version=1 rust_sha256=510bcef67003e249de1a971430cbf39fd9576cd79ae693aefde3b5c5b3989cb9*/
rusty::Rc<Fiber> reactor_get_or_create_fiber_impl(const Reactor& self_, FiberFn func, SrcFileCStr file, int64_t line) {
    auto&& available_guard = self_.available_fibers_.borrow_mut();
    if (rusty::detail::deref_if_pointer_like(REUSING_FIBER) && (rusty::len(available_guard) > static_cast<size_t>(0))) {
        self_.n_idle_fibers_.set(self_.n_idle_fibers_.get() - static_cast<int64_t>(1));
        rusty::Rc<Fiber> fiber = rusty::deref_call(available_guard, rusty::detail::__mdisp_pop{}).unwrap();
        (rusty::detail::deref_if_pointer_like(fiber)).id.set(fiber_next_global_id());
        rusty::detail::deref_if_pointer_like((rusty::detail::deref_if_pointer_like(fiber)).func_.borrow_mut()) = std::move(func);
        verify(((rusty::detail::deref_if_pointer_like(rusty::borrow((rusty::detail::deref_if_pointer_like(fiber)).fiber_task_)))).is_some());
        (rusty::detail::deref_if_pointer_like(fiber)).status_.set(rusty::clone(rusty::clone(FiberStatus_RECYCLED())));
        return std::move(fiber);
    } else {
        rusty::Rc<Fiber> fiber = rusty::Rc<Fiber>::make(std::move(func));
        self_.n_created_fibers_.set(self_.n_created_fibers_.get() + static_cast<int64_t>(1));
        if ((self_.n_created_fibers_.get() % static_cast<int64_t>(1024)) == static_cast<int64_t>(0)) {
            // @unsafe
            {
                rrr::log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("created {}, busy {}, idle {} fibers on server {}, recent {}:{}", self_.n_created_fibers_.get(), self_.n_busy_fibers_.get(), self_.n_idle_fibers_.get(), self_.server_id_.get(), std::move(file), std::move(line)));
            }
        }
        return std::move(fiber);
    }
}
/*RUSTYCPP:GEN-END id=reactor.55*/

// @safe - 1-line shims into the DSL TLS helpers above.
// @safe -Registers a fiber in the active set

// @unsafe - Queue a stackless task slot for polling if not already queued.

// @unsafe - Register a stackless task poller and return slot index.

// @unsafe - Poll all queued stackless tasks once.

// =============================================================================
// Main create_run_fiber - orchestrates the helper functions
// =============================================================================

/**
 * @param func
 * @return
 */
// Creates and runs a fiber. Authored as inline Rust DSL.
//
// The old "KERNEL by verdict (reactor slice 2b)" note claimed the DSL's
// last-use move-insertion mis-handled the repeatedly-passed Rc. Probing
// showed that only happens on input real Rust would reject: the three
// members here already take `&Rc<Fiber>` / `&mut Rc<Fiber>`, so passing
// borrows emits exactly ONE std::move (the return) with no clone-guards.
// The `self` param is named `self_` so it stays a real parameter instead
// of becoming a receiver.
#if RUSTYCPP_RUST
fn reactor_create_run_fiber_impl(self_: &Reactor, func: FiberFn) -> rusty::Rc<Fiber> {
    reactor_create_run_fiber_at_impl(self_, func, "", 0i64)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.51 version=1 rust_sha256=e687f569e16abbab9288bce73508a127526902db0d93314ad27ce20edc69b219*/
rusty::Rc<Fiber> reactor_create_run_fiber_impl(const Reactor& self_, FiberFn func) {
    return reactor_create_run_fiber_at_impl(self_, std::move(func), "", static_cast<int64_t>(0));
}
/*RUSTYCPP:GEN-END id=reactor.51*/

#if RUSTYCPP_RUST
fn reactor_create_run_fiber_at_impl(self_: &Reactor, func: FiberFn, file: SrcFileCStr, line: i64) -> rusty::Rc<Fiber> {
    // Step 1: Get or create a fiber
    let mut fiber = reactor_get_or_create_fiber_impl(self_, func, file, line);

    self_.n_busy_fibers_.set(self_.n_busy_fibers_.get() + 1i64);

    // Step 2: Save current running fiber context (for nesting)
    let old_fiber = self_.save_running_fiber();

    // Step 3: Set this as the running fiber
    self_.set_running_fiber(&fiber);

    // Step 4: Register in the active fibers set
    self_.register_fiber(&fiber);

    // Step 5: Run the fiber
    let status = (*fiber).status_.get();
    if status == FiberStatus::INIT {
        (*fiber).run();
    } else {
        verify(status == FiberStatus::RECYCLED);
        (*fiber).continue_();
    }
    if (*fiber).finished() {
        // Named binding: `&mut local` lowers to a POINTER, which will not
        // bind to recycle's `Rc<Fiber>&`; a typed `&mut` binding lowers
        // to a reference.
        let fiber_ref: &mut rusty::Rc<Fiber> = &mut fiber;
        self_.recycle(fiber_ref);
    }

    // Step 6: Process events
    self_.run_loop(false, true);

    // Step 7: Restore previous running fiber
    self_.restore_running_fiber(old_fiber);

    fiber
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.52 version=1 rust_sha256=2048534e4ce67222ba4694da7eec436b82f5c7944568646debaec68e393adbe6*/
rusty::Rc<Fiber> reactor_create_run_fiber_at_impl(const Reactor& self_, FiberFn func, SrcFileCStr file, int64_t line) {
    auto fiber = reactor_get_or_create_fiber_impl(self_, std::move(func), std::move(file), std::move(line));
    self_.n_busy_fibers_.set(self_.n_busy_fibers_.get() + static_cast<int64_t>(1));
    const auto old_fiber = self_.save_running_fiber();
    self_.set_running_fiber(fiber);
    self_.register_fiber(fiber);
    const auto status = (rusty::detail::deref_if_pointer_like(fiber)).status_.get();
    if (rusty::detail::deref_if_pointer_like(status) == rusty::clone(FiberStatus_INIT())) {
        ((rusty::detail::deref_if_pointer_like(fiber))).run();
    } else {
        verify(rusty::detail::deref_if_pointer_like(status) == rusty::clone(FiberStatus_RECYCLED()));
        ((rusty::detail::deref_if_pointer_like(fiber))).continue_();
    }
    if (((rusty::detail::deref_if_pointer_like(fiber))).finished()) {
        rusty::Rc<Fiber>& fiber_ref = fiber;
        self_.recycle(fiber_ref);
    }
    self_.run_loop(false, true);
    self_.restore_running_fiber(std::move(old_fiber));
    return std::move(fiber);
}
/*RUSTYCPP:GEN-END id=reactor.52*/

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

// One line of scaffolding for the DSL below: a DSL `rusty::Task<void>`
// parameter lowers to the bogus `rusty::Task<void_>`, but through a type
// alias it lowers verbatim. `TaskVoid` IS `rusty::Task<void>`, so the
// forward declaration near the top of this file still declares this very
// function.
using TaskVoid = rusty::Task<void>;

// @safe - Spawn a stackless task and schedule its first poll on this
// reactor. The Task<void> sibling of reactor_spawn_stackless_task_with_result
// (GEN id=reactor.27); apart from the thread-pinning `verify` and the absence
// of a completion callback it is an exact mirror of it.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching GEN block. Three
// lowering rules shape it, each shared with the with_result sibling:
//   * the parameter is `self_` — a DSL parameter literally named `self` is
//     swallowed into a receiver and the body emits `this->`.
//   * `std::atomic` + `std::memory_order` become
//     `rusty::sync::atomic::{AtomicUsize,AtomicBool}` + `Ordering::*`
//     (`exchange` is spelled `swap`). That also DELETES the `mutable` on both
//     wake-state fields: rusty's atomic load/store/swap are already const, so
//     they reach through `Arc::operator->`'s `const T*`. The one genuinely
//     mutable field (the Task) becomes a `RefCell` for the same reason,
//     borrowed in a scope so no borrow is held across the ready-path store —
//     matching the original, which held none.
//   * `early_wake` is `.clone()`d into each closure: a DSL `move ||` closure
//     MOVES its captures, and this body reads `early_wake` again after
//     registering the poller.
// The two fn-body-local structs lower verbatim (hoisted to the top of the
// emitted body); neither carries generic parameters, which is what keeps the
// emitted declarations non-template — see the sibling's note.
#if RUSTYCPP_RUST
fn reactor_spawn_stackless_task_impl(self_: &Reactor, task: TaskVoid) {
    // SAFETY: shared state is heap-owned; the reactor outlives the poller.
    // Both counters are atomic because the waker may fire from another
    // thread.
    struct EarlyWakeState {
        reactor: *const Reactor,
        idx: rusty::sync::atomic::AtomicUsize,
        pending_wake: rusty::sync::atomic::AtomicBool
    }
    // SAFETY: TaskState is only reached through the Arc captured by the
    // poller closure, which runs on this reactor's thread.
    struct TaskState {
        task: rusty::RefCell<TaskVoid>,
        early_wake: rusty::Arc<EarlyWakeState>
    }

    verify(rusty::thread::current_id() == self_.thread_id_.get());
    let kUnregisteredSlot: usize = usize::MAX;
    let rp: *const Reactor = &raw const self_;
    let seed = EarlyWakeState {
        reactor: rp,
        idx: rusty::sync::atomic::AtomicUsize::new(kUnregisteredSlot),
        pending_wake: rusty::sync::atomic::AtomicBool::new(false)
    };
    let early_wake: rusty::Arc<EarlyWakeState> = rusty::Arc::<EarlyWakeState>::make(seed);

    // Each `move ||` closure gets its own clone; early_wake is read again
    // after the poller is registered.
    let ew_waker = early_wake.clone();
    let mut early_waker = rusty::Waker {
        wake_fn: move || {
            let slot = ew_waker.idx.load(rusty::sync::atomic::Ordering::Acquire);
            if slot == kUnregisteredSlot {
                ew_waker.pending_wake.store(true, rusty::sync::atomic::Ordering::Release);
            } else {
                (*ew_waker.reactor).enqueue_stackless_task(slot);
            }
        }
    };
    let wp: *mut rusty::Waker = &raw mut early_waker;
    let mut early_ctx = rusty::Context { waker: wp };
    // Named binding: a bare `&mut local` argument lowers to a pointer, and a
    // last-use local argument is std::move()d — neither binds
    // `Task::poll(rusty::Context&)`. An annotated `&mut` let emits a real
    // C++ reference.
    let ectx: &mut rusty::Context = &mut early_ctx;
    if task.poll(ectx).is_ready() {
        return;
    }

    let ew_state = early_wake.clone();
    let ts = TaskState {
        task: rusty::RefCell::<TaskVoid>::new(task),
        early_wake: ew_state
    };
    let state: rusty::Arc<TaskState> = rusty::Arc::<TaskState>::make(ts);
    let idx = self_.register_stackless_poller(move |ctx: &mut rusty::Context| -> bool {
        // Scoped so the task borrow is released before the ready-path store.
        let mut ready: bool = false;
        {
            let tguard = state.task.borrow_mut();
            ready = (*tguard).poll(ctx).is_ready();
        }
        if !ready {
            return false;
        }
        (*state.early_wake).idx.store(kUnregisteredSlot, rusty::sync::atomic::Ordering::Release);
        true
    });
    early_wake.idx.store(idx, rusty::sync::atomic::Ordering::Release);
    if early_wake.pending_wake.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
        self_.enqueue_stackless_task(idx);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.76 version=1 rust_sha256=39f9e6838fcf5358bb2dbaf051ee8d48f8e5debda9b884277a61116ec3f28741*/
void reactor_spawn_stackless_task_impl(const Reactor& self_, TaskVoid task) {
    struct EarlyWakeState {
        const Reactor* reactor;
        rusty::sync::atomic::AtomicUsize idx;
        rusty::sync::atomic::AtomicBool pending_wake;
    };
    struct TaskState {
        rusty::RefCell<TaskVoid> task;
        rusty::Arc<EarlyWakeState> early_wake;
    };
    verify(rusty::thread::current_id() == self_.thread_id_.get());
    size_t kUnregisteredSlot = std::numeric_limits<size_t>::max();
    const Reactor* rp = &self_;
    auto seed = EarlyWakeState{.reactor = rp, .idx = rusty::sync::atomic::AtomicUsize::new_(std::move(kUnregisteredSlot)), .pending_wake = rusty::sync::atomic::AtomicBool::new_(false)};
    const rusty::Arc<EarlyWakeState> early_wake = rusty::Arc<EarlyWakeState>::make(std::move(seed));
    auto ew_waker = rusty::clone(early_wake);
    auto early_waker = rusty::Waker{.wake_fn = [=, ew_waker = std::move(ew_waker), kUnregisteredSlot = std::move(kUnregisteredSlot)]() {
const auto slot = (*ew_waker).idx.load(rusty::sync::atomic::Ordering::Acquire);
if (rusty::detail::deref_if_pointer_like(slot) == rusty::detail::deref_if_pointer_like(kUnregisteredSlot)) {
    (*ew_waker).pending_wake.store(true, rusty::sync::atomic::Ordering::Release);
} else {
    ((*(*ew_waker).reactor)).enqueue_stackless_task(std::move(slot));
}
}};
    rusty::Waker* wp = &early_waker;
    auto early_ctx = rusty::Context{.waker = wp};
    rusty::Context& ectx = early_ctx;
    if (task.poll(ectx).is_ready()) {
        return;
    }
    auto ew_state = rusty::clone(early_wake);
    auto ts = TaskState{.task = rusty::RefCell<TaskVoid>::new_(std::move(task)), .early_wake = std::move(ew_state)};
    rusty::Arc<TaskState> state = rusty::Arc<TaskState>::make(std::move(ts));
    const auto idx = self_.register_stackless_poller([=, kUnregisteredSlot = std::move(kUnregisteredSlot), state = std::move(state)](rusty::Context& ctx) -> bool {
bool ready = false;
{
    auto tguard = (*state).task.borrow_mut();
    ready = ((*tguard)).poll(ctx).is_ready();
}
if (!ready) {
    return false;
}
(rusty::detail::deref_if_pointer_like((*state).early_wake)).idx.store(std::move(kUnregisteredSlot), rusty::sync::atomic::Ordering::Release);
return true;
});
    (*early_wake).idx.store(std::move(idx), rusty::sync::atomic::Ordering::Release);
    if ((*early_wake).pending_wake.swap(false, rusty::sync::atomic::Ordering::AcqRel)) {
        self_.enqueue_stackless_task(std::move(idx));
    }
}
/*RUSTYCPP:GEN-END id=reactor.76*/

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
// channel is polled with try_recv() after each epoll_wait.) The old
// "its Epoll()/map defaults are alias ctors with no DSL spelling"
// verdict was stale: a DSL struct literal over a DSL AGGREGATE lowers to
// designated init and every foreign alias-ctor call passes through
// verbatim. It stays its OWN fn on purpose — folding the literal into
// pollworker_create's body makes each field value pick up that fn's
// class RETURN type as a bogus qualifier
// (`rusty::Rc<rusty::RefCell<PollThreadWorker>>::Epoll()`), i.e. the
// class-return mis-qualification defect surfacing in a new place.
#if RUSTYCPP_RUST
fn pollworker_make(receiver: PollCmdReceiver) -> PollThreadWorker {
    PollThreadWorker {
        receiver_: receiver,
        poll_: Epoll(),
        fd_to_pollable_: FdPollableMap(),
        mode_: FdModeMap(),
        pending_remove_: FdSet(),
        jobs_: JobSet(),
        stop_: false,
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.79 version=1 rust_sha256=168b69f83b5c7193c0bc61fa5b67d89144bf1970259c8e429adf0839cd817899*/
PollThreadWorker pollworker_make(PollCmdReceiver receiver) {
    return PollThreadWorker{.receiver_ = std::move(receiver), .poll_ = Epoll(), .fd_to_pollable_ = FdPollableMap(), .mode_ = FdModeMap(), .pending_remove_ = FdSet(), .jobs_ = JobSet(), .stop_ = false};
}
/*RUSTYCPP:GEN-END id=reactor.79*/

// Create the worker and wrap it in the shared RefCell.
#if RUSTYCPP_RUST
fn pollworker_create(receiver: PollCmdReceiver) -> rusty::Rc<rusty::RefCell<PollThreadWorker>> {
    let mut worker = pollworker_make(receiver);
    rusty::Rc::<rusty::RefCell<PollThreadWorker>>::make(worker)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.37 version=1 rust_sha256=ff4895c81826163125160dcd83c4d0d9a0d168ea3b2b8bcd634d6c152077c731*/
rusty::Rc<rusty::RefCell<PollThreadWorker>> pollworker_create(PollCmdReceiver receiver) {
    auto worker = pollworker_make(std::move(receiver));
    return rusty::Rc<rusty::RefCell<PollThreadWorker>>::make(std::move(worker));
}
/*RUSTYCPP:GEN-END id=reactor.37*/

// Key-set snapshot for poll_loop's three map sweeps, as DSL. The stated
// blocker has expired: the old hand-written loop existed only because
// `for (auto [fd, poll] : self.fd_to_pollable_)` destructures the
// `std::tuple<const K&, V&>` that hashbrown's stl_iter_t yields and the
// DSL has no spelling for that binding -- but the map port also exposes
// `keys()`, whose Item is a plain `const K&`, so the pair never appears.
// With an indexable `Vec<i32>` in hand, all three of poll_loop's sweeps
// stay plain DSL loops that re-`get()` the proxy per fd (that re-lookup
// is what makes each sweep robust against an entry a previous sweep
// already erased). Cost is unchanged: one Vec<int> plus one hash lookup
// per registered fd per poll iteration, against an epoll_wait syscall
// and the same N virtual calls. Same shape as pollworker_take_removals.
#if RUSTYCPP_RUST
fn pollworker_snapshot_fds(w: &mut PollThreadWorker) -> Vec<i32> {
    let mut fds: Vec<i32> = Vec::new();
    let mut ks = w.fd_to_pollable_.keys();
    for fd in ks {
        fds.push(*fd);
    }
    fds
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.91 version=1 rust_sha256=0a69a6fb999ae3089f649fbd15391df35ad4acd55e796982601e4b1e994f5e9c*/
rusty::Vec<int32_t> pollworker_snapshot_fds(PollThreadWorker& w) {
    rusty::Vec<int32_t> fds = rusty::Vec<int32_t>::new_();
    auto ks = w.fd_to_pollable_.keys();
    for (auto&& fd : rusty::for_in(ks)) {
        fds.push(std::move(rusty::detail::deref_if_pointer_like(fd)));
    }
    return std::move(fds);
}
/*RUSTYCPP:GEN-END id=reactor.91*/

// The poll thread's main loop as inline Rust DSL. The structure is
// unchanged from the hand-written original: epoll_wait -> channel
// commands -> deferred removals -> reactor run_loop -> pending-write
// sweep -> closed-pollable sweep, then a shutdown pass that unregisters
// everything and drops the maps.
//
// Lowering notes (each one was a stated blocker that has since expired):
//   * the `Wait` callback is a plain (non-`move`) DSL closure, so it
//     emits a by-reference `[&]` lambda — exactly the capture the old
//     hand-written `[&self]` had, and the only capture that is correct
//     here (the worker outlives the call).
//   * `PollMode::` / `PollReady::` are namespace constants, not DSL
//     enums, so they lower as plain paths (no variant-call trap).
//   * reaching a virtual through the map needs the named-Box binding
//     `let p: &mut Box<PollableBase> = opt.unwrap();` so the call lowers
//     to `->` instead of copying the move-only Box (playbook §7.13, the
//     idiom pollworker_close_proxy_of already ships).
//   * the three `for (auto [fd, poll] : ...)` sweeps are replaced by
//     index loops over the pollworker_snapshot_fds kernel above; the
//     `w.fd_to_pollable_.get(fd)` re-lookup is what makes each sweep
//     robust against an entry the previous sweep already erased.
// NOTE the pre-seeded block id below: a new DSL block in this file
// auto-numbers into an id an existing block already holds (§7.32).
#if RUSTYCPP_RUST
fn pollworker_poll_loop(w: &mut PollThreadWorker) {
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[poll_loop] Starting poll loop"));
    while !w.stop_ {
        pollworker_trigger_job(w);

        // Wait for events (epoll_wait with short timeout). Dispatch
        // through proxy storage by fd; no Pollable* userdata assumptions.
        w.poll_.Wait(|fd: i32, ready_events: i32| {
            let poll_opt = w.fd_to_pollable_.get(fd);
            if poll_opt.is_none() {
                return;
            }
            let p: &mut Box<PollableBase> = poll_opt.unwrap();

            if (ready_events & PollReady::READABLE) != 0i32 {
                p.handle_read();
            }
            if (ready_events & PollReady::WRITABLE) != 0i32 {
                let new_mode = p.handle_write();
                if new_mode != PollMode::NO_CHANGE {
                    pollworker_do_update_mode(w, fd, new_mode);
                }
            }
            if (ready_events & PollReady::ERROR) != 0i32 {
                p.handle_error();
            }
        });

        // Process commands from the channel (non-blocking try_recv).
        pollworker_process_commands(w);
        pollworker_trigger_job(w);
        // Process deferred removals.
        pollworker_process_pending_removals(w);
        pollworker_trigger_job(w);
        let reactor = Reactor::get_reactor();
        (*reactor).run_loop(false, true);

        // One key snapshot serves both sweeps below: neither of them
        // adds an fd to fd_to_pollable_ (do_update_mode only touches
        // mode_/poll_), so the key set cannot grow in between.
        let fds = pollworker_snapshot_fds(w);

        // Check for pending write updates (set by end_reply() during
        // fiber execution). Reads a pollable's interior-mutable
        // pending_write_update_ flag through the shared Arc; no cast.
        let mut i: usize = 0;
        while i < fds.len() {
            let fd = fds[i];
            let opt = w.fd_to_pollable_.get(fd);
            if opt.is_some() {
                let p: &mut Box<PollableBase> = opt.unwrap();
                if p.check_pending_write_update() {
                    pollworker_do_update_mode(w, fd, PollMode::READ | PollMode::WRITE);
                }
            }
            i += 1;
        }

        // Check for pollables closed by handle_error() and remove them.
        // This prevents fd reuse issues when an old connection is closed
        // but not removed. Collect first, mutate second — close() can
        // re-enter, so the scan must not be mutating as it goes.
        let mut closed_fds: Vec<i32> = Vec::new();
        let mut j: usize = 0;
        while j < fds.len() {
            let fd = fds[j];
            let opt = w.fd_to_pollable_.get(fd);
            if opt.is_some() {
                let p: &mut Box<PollableBase> = opt.unwrap();
                if p.is_closed() {
                    closed_fds.push(fd);
                }
            }
            j += 1;
        }
        let mut n: usize = 0;
        while n < closed_fds.len() {
            let fd = closed_fds[n];
            let proxy_opt = w.fd_to_pollable_.get(fd);
            if proxy_opt.is_some() {
                // Remove from epoll if still registered.
                if w.mode_.contains_key(fd) {
                    w.poll_.Remove(fd);
                }
                // Invoke the close callback before erasing the map entry
                // so cleanup hooks run.
                let p: &mut Box<PollableBase> = proxy_opt.unwrap();
                p.close();
                w.fd_to_pollable_.remove(fd);
                w.mode_.remove(fd);
            }
            n += 1;
        }
    }

    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[poll_loop] Exited while loop (stop_=true), starting cleanup"));
    // Shutdown cleanup — unregister all remaining pollables. Only the
    // keys matter here, so the proxies are never touched.
    let rest = pollworker_snapshot_fds(w);
    let mut k: usize = 0;
    while k < rest.len() {
        let fd = rest[k];
        if w.mode_.contains_key(fd) {
            w.poll_.Remove(fd);
        }
        k += 1;
    }
    w.fd_to_pollable_.clear();
    w.mode_.clear();
    w.pending_remove_.clear();
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[poll_loop] Cleanup complete, poll_loop exiting"));
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.pollworker_poll_loop version=1 rust_sha256=482a03960f026206117e86d78777e452c268e0ea8fc83703b831401da1d37db8*/
void pollworker_poll_loop(PollThreadWorker& w) {
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[poll_loop] Starting poll loop"));
    while (rusty::detail::rust_not(w.stop_)) {
        pollworker_trigger_job(w);
        w.poll_.Wait([&](int32_t fd, int32_t ready_events) {
auto poll_opt = w.fd_to_pollable_.get(std::move(fd));
if (poll_opt.is_none()) {
    return;
}
rusty::Box<PollableBase>& p = poll_opt.unwrap();
if (((rusty::detail::deref_if_pointer_like(ready_events) & PollReady::READABLE)) != static_cast<int32_t>(0)) {
    p->handle_read();
}
if (((rusty::detail::deref_if_pointer_like(ready_events) & PollReady::WRITABLE)) != static_cast<int32_t>(0)) {
    const auto new_mode = p->handle_write();
    if (rusty::detail::deref_if_pointer_like(new_mode) != rusty::clone(PollMode::NO_CHANGE)) {
        pollworker_do_update_mode(w, std::move(fd), std::move(new_mode));
    }
}
if (((rusty::detail::deref_if_pointer_like(ready_events) & PollReady::ERROR)) != static_cast<int32_t>(0)) {
    p->handle_error();
}
});
        pollworker_process_commands(w);
        pollworker_trigger_job(w);
        pollworker_process_pending_removals(w);
        pollworker_trigger_job(w);
        const auto reactor = Reactor::get_reactor();
        ((rusty::detail::deref_if_pointer_like(reactor))).run_loop(false, true);
        const auto fds = pollworker_snapshot_fds(w);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len(fds)) {
            const auto fd = fds[i];
            auto opt = w.fd_to_pollable_.get(std::move(fd));
            if (opt.is_some()) {
                rusty::Box<PollableBase>& p = opt.unwrap();
                if (p->check_pending_write_update()) {
                    pollworker_do_update_mode(w, std::move(fd), rusty::clone(PollMode::READ) | rusty::clone(PollMode::WRITE));
                }
            }
            i += 1;
        }
        rusty::Vec<int32_t> closed_fds = rusty::Vec<int32_t>::new_();
        size_t j = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(j) < rusty::len(fds)) {
            auto fd = fds[j];
            auto opt = w.fd_to_pollable_.get(std::move(fd));
            if (opt.is_some()) {
                rusty::Box<PollableBase>& p = opt.unwrap();
                if (p->is_closed()) {
                    closed_fds.push(std::move(fd));
                }
            }
            j += 1;
        }
        size_t n = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(n) < rusty::len(closed_fds)) {
            const auto fd = closed_fds[n];
            auto proxy_opt = w.fd_to_pollable_.get(std::move(fd));
            if (proxy_opt.is_some()) {
                if (w.mode_.contains_key(std::move(fd))) {
                    w.poll_.Remove(std::move(fd));
                }
                rusty::Box<PollableBase>& p = proxy_opt.unwrap();
                p->close();
                w.fd_to_pollable_.remove(std::move(fd));
                w.mode_.remove(std::move(fd));
            }
            n += 1;
        }
    }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[poll_loop] Exited while loop (stop_=true), starting cleanup"));
    const auto rest = pollworker_snapshot_fds(w);
    size_t k = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(k) < rusty::len(rest)) {
        const auto fd = rest[k];
        if (w.mode_.contains_key(std::move(fd))) {
            w.poll_.Remove(std::move(fd));
        }
        k += 1;
    }
    w.fd_to_pollable_.clear();
    w.mode_.clear();
    w.pending_remove_.clear();
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[poll_loop] Cleanup complete, poll_loop exiting"));
}
/*RUSTYCPP:GEN-END id=reactor.pollworker_poll_loop*/

// @unsafe - calls try_recv and std::visit
// Non-blocking receive: drain every pending command.
//
// The seven-arm dispatch is a DSL `match` over the PollCommand enum now.
// It could not be before: a struct-variant arm bound its payload
// `const auto&`, so handing the move-only `Box<PollableBase>` (or the
// `Arc<Job>`) to a handler hit the deleted copy constructor. Fixed in
// rusty-cpp bf5fc12c -- bindings are `auto&&`, which is `const F&` under
// a const visit parameter and `F&` under the mutable one, so `match &mut`
// finally means what Rust means by it.
#if RUSTYCPP_RUST
fn pollworker_process_commands(self_: &mut PollThreadWorker) {
    loop {
        let result = self_.receiver_.try_recv();
        if result.is_err() {
            // Empty or disconnected -- either way, stop draining.
            break;
        }
        let mut cmd = result.unwrap();
        match &mut cmd {
            PollCommand::AddPollable { pollable } => {
                pollworker_do_add_pollable(self_, pollable);
            }
            PollCommand::RemovePollable { fd } => {
                pollworker_do_remove_pollable(self_, fd);
            }
            PollCommand::ClosePollable { fd } => {
                pollworker_do_close_pollable(self_, fd);
            }
            PollCommand::UpdateMode { fd, new_mode } => {
                pollworker_do_update_mode(self_, fd, new_mode);
            }
            PollCommand::AddJob { job } => {
                pollworker_do_add_job(self_, job);
            }
            PollCommand::RemoveJob { job } => {
                pollworker_do_remove_job(self_, job);
            }
            PollCommand::Shutdown => {
                self_.stop_ = true;
            }
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.78 version=1 rust_sha256=1756965547c09088f825ff15321b420a13cb8acfe79895edd57dae741cdf117e*/
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void pollworker_process_commands(PollThreadWorker& self_) {
    PollThreadWorker* self__shadow1 = &self_;
    while (true) {
        auto result = (*self__shadow1).receiver_.try_recv();
        if (result.is_err()) {
            break;
        }
        auto cmd = result.unwrap();
        {
            auto&& _m = cmd;
            std::visit(overloaded {
                [&](std::variant_alternative_t<0, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>& _v) {
                    auto&& pollable = _v.pollable;
                    pollworker_do_add_pollable(*self__shadow1, std::move(pollable));
                },
                [&](std::variant_alternative_t<1, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>& _v) {
                    auto&& fd = _v.fd;
                    pollworker_do_remove_pollable(*self__shadow1, std::move(fd));
                },
                [&](std::variant_alternative_t<2, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>& _v) {
                    auto&& fd = _v.fd;
                    pollworker_do_close_pollable(*self__shadow1, std::move(fd));
                },
                [&](std::variant_alternative_t<3, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>& _v) {
                    auto&& fd = _v.fd;
                    auto&& new_mode = _v.new_mode;
                    pollworker_do_update_mode(*self__shadow1, std::move(fd), std::move(new_mode));
                },
                [&](std::variant_alternative_t<4, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>& _v) {
                    auto&& job = _v.job;
                    pollworker_do_add_job(*self__shadow1, std::move(job));
                },
                [&](std::variant_alternative_t<5, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>& _v) {
                    auto&& job = _v.job;
                    pollworker_do_remove_job(*self__shadow1, std::move(job));
                },
                [&](std::variant_alternative_t<6, rusty::detail::variant_underlying_type_t<decltype(rusty::detail::deref_if_pointer(_m))>>&) {
                    (*self__shadow1).stop_ = true;
                },
            }, rusty::detail::deref_if_pointer(_m));
        }
    }
}
/*RUSTYCPP:GEN-END id=reactor.78*/

// The Job trait's legacy virtual surface takes `&mut self`, while the
// scheduler owns jobs through shared Arcs. Keep the unsafe cast localized and
// retain an Arc in the work closure so its raw pointer cannot outlive the job.
#if RUSTYCPP_RUST
fn job_ready(job: &rusty::Arc<Job>) -> bool {
    let job_ptr: *const Job = &raw const **job;
    let job_mut: *mut Job = job_ptr as *mut Job;
    unsafe { (*job_mut).Ready() }
}

fn job_spawn_work(job: &rusty::Arc<Job>) {
    let owned = job.clone();
    Fiber::create_run(move || {
        let job_ptr: *const Job = &raw const *owned;
        let job_mut: *mut Job = job_ptr as *mut Job;
        unsafe { (*job_mut).Work(); }
    });
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.94 version=1 rust_sha256=fa38b5b317e02ceaf358db4db0130d566be1e9fc8c92588bf55a49c048aa1b88*/
bool job_ready(const rusty::Arc<Job>& job) {
    const Job* job_ptr = &rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer_like(job));
    Job* const job_mut = const_cast<Job*>(reinterpret_cast<const Job*>(job_ptr));
    // @unsafe
    {
        return ((*job_mut)).Ready();
    }
}

void job_spawn_work(const rusty::Arc<Job>& job) {
    const auto owned = rusty::clone(job);
    Fiber::create_run([=]() {
const Job* job_ptr = &rusty::detail::deref_if_pointer_like(owned);
Job* const job_mut = const_cast<Job*>(reinterpret_cast<const Job*>(job_ptr));
// @unsafe
{
    ((*job_mut)).Work();
}
});
}
/*RUSTYCPP:GEN-END id=reactor.94*/

// Job trigger pass: run every ready job on a fiber, requeue the rest.
// `jobs_` is a std::set with no drain(), so mem::take moves the whole
// set out and leaves an empty one behind — same net effect as the old
// copy-then-clear, and equally necessary: a Work() body may re-add jobs
// while the pass is in flight, so the pass must not iterate `jobs_`.
#if RUSTYCPP_RUST
fn pollworker_trigger_job(w: &mut PollThreadWorker) {
    let jobs_exec = core::mem::take(&mut w.jobs_);
    for job in jobs_exec.iter() {
        if job_ready(job) {
            // Ready jobs ran (or are running) — do NOT re-add them.
            job_spawn_work(job);
        } else {
            // Not ready yet — check again on the next pass.
            w.jobs_.insert(job);
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.59 version=1 rust_sha256=600ba3f0da805b47c38b584c3140b7169a1afa1fcd4b26f22e4b3a02700e97b0*/
void pollworker_trigger_job(PollThreadWorker& w) {
    const auto jobs_exec = rusty::mem::take(w.jobs_);
    for (auto&& job : rusty::for_in(rusty::iter(jobs_exec))) {
        if (job_ready(std::move(job))) {
            job_spawn_work(std::move(job));
        } else {
            w.jobs_.insert(std::move(job));
        }
    }
}
/*RUSTYCPP:GEN-END id=reactor.59*/

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

// @safe - Box-trait arrow dispatch; no longer a kernel. What actually
// blocked it: the `PollableProxy` ALIAS hides the Box from the emitter,
// so a `&PollableProxy` param plus `(*p).fd()` lowers to a DOT call on
// the handle (`((p)).fd()`) — a hard compile error, not a silent wrong
// answer. Re-binding to a Box-TYPED local restores the arrow (playbook
// §7.13), the same idiom pollworker_close_proxy_of already uses, and the
// emitted signature stays `const PollableProxy&` for the forward decl.
#if RUSTYCPP_RUST
fn pollable_proxy_fd(p: &PollableProxy) -> i32 {
    let b: &Box<PollableBase> = p;
    b.fd()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.85 version=1 rust_sha256=900cf3ef12554c9b7217df6b67c04fb2d26c0cf29f46c269a3ae150b10add898*/
int32_t pollable_proxy_fd(const PollableProxy& p) {
    const rusty::Box<PollableBase>& b = p;
    return b->fd();
}
/*RUSTYCPP:GEN-END id=reactor.85*/
// Drains the pending-remove set into an indexable Vec for the DSL
// sweep. HashSet::drain() empties the set as it yields — the same
// idiom the client's pending-future map uses — replacing the old
// iterate-then-clear ("no rusty::iter shim" cause expired).
#if RUSTYCPP_RUST
fn pollworker_take_removals(w: &mut PollThreadWorker) -> Vec<i32> {
    // The HashSet port has no drain(); take the whole set (leaves an
    // empty one behind — same net effect as the old iterate-then-clear)
    // and copy the fds out through iter().
    let taken = core::mem::take(&mut w.pending_remove_);
    let mut v: Vec<i32> = Vec::new();
    for fd in taken.iter() {
        v.push(*fd);
    }
    v
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.31 version=1 rust_sha256=8ce055e559981cf9202809a490982d6bd3927a8e81598eb2a27e04db8e7b4b8a*/
rusty::Vec<int32_t> pollworker_take_removals(PollThreadWorker& w) {
    const auto taken = rusty::mem::take(w.pending_remove_);
    rusty::Vec<int32_t> v = rusty::Vec<int32_t>::new_();
    for (auto&& fd : rusty::for_in(rusty::iter(taken))) {
        v.push(std::move(rusty::detail::deref_if_pointer_like(fd)));
    }
    return std::move(v);
}
/*RUSTYCPP:GEN-END id=reactor.31*/
// @safe - Box-trait arrow dispatch; same alias-hides-the-Box rebinding
// as pollable_proxy_fd above.
#if RUSTYCPP_RUST
fn pollable_proxy_mode(p: &PollableProxy) -> i32 {
    let b: &Box<PollableBase> = p;
    b.poll_mode()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.88 version=1 rust_sha256=fc42e79c03df77cf4e7c432964fc3522d7ab055080627dda6a699a8817ae5684*/
int32_t pollable_proxy_mode(const PollableProxy& p) {
    const rusty::Box<PollableBase>& b = p;
    return b->poll_mode();
}
/*RUSTYCPP:GEN-END id=reactor.88*/
// The map hands back Option<Box&>; the named-Box binding makes close()
// lower to `->` (playbook §7.13).
#if RUSTYCPP_RUST
fn pollworker_close_proxy_of(w: &mut PollThreadWorker, fd: i32) {
    // The map port's non-const get() returns Option<V&>; the &mut-typed
    // Box binding keeps the mutable overload selected so close()'s
    // non-const dispatch compiles.
    let proxy_opt = w.fd_to_pollable_.get(fd);
    if proxy_opt.is_some() {
        let p: &mut Box<PollableBase> = proxy_opt.unwrap();
        p.close();
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.32 version=1 rust_sha256=991c4898103f9a77280f2acfd05f062144007f67a8b4a2c3b1aaa98a5f7d4aab*/
void pollworker_close_proxy_of(PollThreadWorker& w, int32_t fd) {
    auto proxy_opt = w.fd_to_pollable_.get(std::move(fd));
    if (proxy_opt.is_some()) {
        rusty::Box<PollableBase>& p = proxy_opt.unwrap();
        p->close();
    }
}
/*RUSTYCPP:GEN-END id=reactor.32*/

// @safe - Update poll mode directly (bypasses channel); only safe on
// the poll thread. The "dyn-trait ref params have no verified DSL
// spelling" verdict was stale: `&mut Pollable` lowers to a plain
// `Pollable&`, which the PollThreadWorker::update_mode DSL method above
// had already been proving from the CALLING side all along.
#if RUSTYCPP_RUST
fn pollworker_update_mode(w: &mut PollThreadWorker, poll: &mut Pollable, new_mode: i32) {
    pollworker_do_update_mode(w, poll.fd(), new_mode);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.89 version=1 rust_sha256=c2a4fe7a33358755dff5caae27b16d5e50ca7fcc5532b445b5b33b5e6dbfaba5*/
void pollworker_update_mode(PollThreadWorker& w, Pollable& poll, int32_t new_mode) {
    pollworker_do_update_mode(w, poll.fd(), std::move(new_mode));
}
/*RUSTYCPP:GEN-END id=reactor.89*/

// =============================================================================
// PollThread Implementation
// =============================================================================


// @safe - ThreadId->u64 bit_cast helper. `platform::threading::thread_id`
// is `std::thread::id` (default backend) or `pthread_t` (POSIX backend).
// The two hand-written static_asserts are gone because `std::bit_cast`
// already enforces exactly them (equal size + trivially copyable) as
// hard constraints — the DSL body loses no compile-time checking, and
// `decltype(tid.as_native())` was only ever there to name the type for
// them. Stays in the anonymous namespace: the GEN lands inside it, and
// the sole caller (pollthread_create's spawn closure) is below.
namespace {
#if RUSTYCPP_RUST
fn thread_id_to_u64(tid: rusty::thread::ThreadId) -> u64 {
    std::bit_cast::<u64>(tid.as_native())
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.90 version=1 rust_sha256=e1a6aa470859ad3ba5ab7dc5bd44f4ccf933eac03a8531a720a027e0fe72b5f3*/
uint64_t thread_id_to_u64(rusty::thread::ThreadId tid);

uint64_t thread_id_to_u64(rusty::thread::ThreadId tid) {
    return std::bit_cast<uint64_t>(tid.as_native());
}
/*RUSTYCPP:GEN-END id=reactor.90*/

} // namespace

// The poll-thread factory, authored as inline Rust DSL. Three spellings
// are load-bearing:
//   * `rrr::PollThread(...)` is the fieldwise-CTOR call, NOT a
//     `PollThread { field: v }` struct literal: PollThread has an
//     `impl Drop`, so its GEN is move-only WITH a fieldwise ctor (not
//     an aggregate) and a struct literal lowers to an ill-formed
//     designated-initializer list. The `rrr::` qualification dodges the
//     class-return-type mis-qualification that would otherwise emit
//     `rusty::Arc<PollThread>::PollThread(...)`.
//   * `thread_id_ptr` is bound to a TYPED local: `&raw const` written
//     directly as a call argument is dropped.
//   * `worker` / `guard` are TYPED lets, so the emitted accesses are
//     `worker->borrow_mut()` and `&*guard` (an untyped let emits a dot,
//     which rusty::Rc does not offer for RefCell::borrow_mut).
// @unsafe - takes the address of an atomic field and hands the raw
// pointer to the spawned worker; the Arc keeps the PollThread (and thus
// the atomic) alive until that thread finishes, and the borrow_mut
// guard outlives the whole poll_loop() call so the raw TLS
// `g_current_poll_worker` stays valid. rusty-cpp cannot express either
// lifetime relationship.
#if RUSTYCPP_RUST
fn pollthread_create() -> rusty::Arc<PollThread> {
    let (sender, receiver) = rusty::sync::mpsc::channel::<PollCommand>();
    let seed = rrr::PollThread(sender,
                               PollJoinSlot::new(rusty::None),
                               rusty::sync::atomic::AtomicU64::new(0u64),
                               rusty::sync::atomic::AtomicBool::new(false));
    let arc: rusty::Arc<PollThread> = rusty::Arc::<PollThread>::new_(seed);
    // rusty atomic ops are const, so a const* suffices through the Arc.
    let thread_id_ptr: *const rusty::sync::atomic::AtomicU64 = &raw const arc.poll_thread_id_bits_;
    let handle = rusty::thread::spawn(move |rx: PollCmdReceiver| {
        let tid = rusty::thread::current_id();
        (*thread_id_ptr).store(thread_id_to_u64(tid), rusty::sync::atomic::Ordering::Release);
        // Raw TLS pointer (not a re-borrow) so fibers on this thread can
        // reach the worker while the borrow_mut guard is held.
        let worker: rusty::Rc<rusty::RefCell<PollThreadWorker>> = PollThreadWorker::create(rx);
        let guard: rusty::RefMut<PollThreadWorker> = worker.borrow_mut();
        g_current_poll_worker = &raw mut *guard;
        guard.poll_loop();
        g_current_poll_worker = core::ptr::null_mut();
    }, receiver);
    {
        let mut slot = (*arc).join_handle_.lock().unwrap();
        *slot = rusty::Some(handle);
    }
    arc
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.84 version=1 rust_sha256=7db7fe3a19c05b5910328cc49a04396014e46a528321401fe6a8b712ffc1efdd*/
rusty::Arc<PollThread> pollthread_create() {
    auto [sender, receiver] = rusty::detail::deref_if_pointer_like(rusty::sync::mpsc::channel<PollCommand>());
    auto seed = rrr::PollThread(std::move(sender), PollJoinSlot::new_(rusty::None), rusty::sync::atomic::AtomicU64::new_(static_cast<uint64_t>(0)), rusty::sync::atomic::AtomicBool::new_(false));
    rusty::Arc<PollThread> arc = rusty::Arc<PollThread>::new_(std::move(seed));
    const rusty::sync::atomic::AtomicU64* thread_id_ptr = &(*arc).poll_thread_id_bits_;
    auto handle = rusty::thread::spawn([=, thread_id_ptr = std::move(thread_id_ptr)](PollCmdReceiver rx) {
const auto tid = rusty::thread::current_id();
((*thread_id_ptr)).store(thread_id_to_u64(std::move(tid)), rusty::sync::atomic::Ordering::Release);
const rusty::Rc<rusty::RefCell<PollThreadWorker>> worker = PollThreadWorker::create(std::move(rx));
const rusty::RefMut<PollThreadWorker> guard = worker->borrow_mut();
g_current_poll_worker = &*guard;
guard->poll_loop();
g_current_poll_worker = rusty::ptr::null_mut();
}, std::move(receiver));
    {
        auto&& slot = rusty::deref_call((rusty::detail::deref_if_pointer_like(arc)).join_handle_.lock(), rusty::detail::__mdisp_unwrap{});
        rusty::detail::deref_if_pointer_like(slot) = rusty::Some(std::move(handle));
    }
    return std::move(arc);
}
/*RUSTYCPP:GEN-END id=reactor.84*/

// The PollThread drop body, authored as inline Rust DSL: gettid via a
// route-2 unsafe{} syscall (SYS_gettid is a macro identifier that
// lowers as-is), int-arg Log_debug, and the shutdown() method call on
// the by-ref PollThread (non-`self` param name so it emits pt.method,
// not this->).
#if RUSTYCPP_RUST
fn pollthread_drop(pt: &PollThread) {
    let tid: i64 = unsafe { syscall(SYS_gettid) };
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::~PollThread] Destructor called from TID={}", tid as i32));
    pt.shutdown();
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("[PollThread::~PollThread] Destructor complete"));
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.pollthread_drop version=1 rust_sha256=d997d493e9ba232af8440e612512bc2690aa85273c4756a0315120c6b7610429*/
void pollthread_drop(const PollThread& pt) {
    const int64_t tid = syscall(SYS_gettid);
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::~PollThread] Destructor called from TID={}", static_cast<int32_t>(tid)));
    pt.shutdown();
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("[PollThread::~PollThread] Destructor complete"));
}
/*RUSTYCPP:GEN-END id=reactor.pollthread_drop*/






// @safe - Sends update mode command via channel (send wrapped @unsafe)
// SAFETY: Channel send is thread-safe



// --- from fiber_context_runtime.cc --------------------------------------

// fiber_swap_context is implemented in arch-specific files:
//   fiber_context_x86_64.cc  (x86_64)
//   fiber_context_aarch64.cc (AArch64/ARM64)

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

// The srpc_fiber C-engine boundary, authored as inline Rust DSL. The
// handles arrive as PARAMETERS (a `*mut srpc_fiber`, and the
// Function/yield pair), so the deliberately hand-written fiber_task_t
// shell needs no new `friend` declarations: each member passes its own
// private member in. Same shape as the Fiber member delegations at
// :5145 (`void Fiber::run() const { fiber_run(*this); }`). `f as bool`
// is the DSL spelling of `static_cast<bool>(fn_)` — rusty::Function has
// an explicit operator bool and no is_valid().
#if RUSTYCPP_RUST
// The one C -> C++ reentry point. C linkage and the raw void-pointer cast are
// both authored here so the generated symbol remains the C engine's callback.
#[no_mangle]
pub unsafe extern "C" fn fiber_task_entry_thunk(arg: *mut core::ffi::c_void) {
    let task: *mut fiber_task_t = arg as *mut fiber_task_t;
    unsafe { (*task).run_body(); }
}

fn fiber_engine_start(fib: *mut srpc_fiber, arg: *mut core::ffi::c_void) {
    unsafe {
        srpc_fiber_init(fib, kDefaultStackBytes, fiber_task_entry_thunk, arg);
        // Match Boost.Coroutine2 pull_type behavior: run immediately on
        // construction.
        srpc_fiber_resume(fib);
    }
}

fn fiber_engine_resume(fib: *mut srpc_fiber) {
    unsafe { srpc_fiber_resume(fib); }
}

fn fiber_engine_yield(fib: *mut srpc_fiber) {
    unsafe { srpc_fiber_yield(fib); }
}

fn fiber_engine_destroy(fib: *mut srpc_fiber) {
    unsafe { srpc_fiber_destroy(fib); }
}

fn fiber_task_body_invoke(f: &mut FiberTaskFn, y: &mut fiber_yield_t) {
    verify(f as bool);
    (*f)(y);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.87 version=1 rust_sha256=445bcc5e2dfe00f73bf3d695c59e02ebb942c6e72154ed510033c0205a4122af*/
extern "C" void fiber_task_entry_thunk(rusty::ffi::c_void* arg);
void fiber_engine_start(srpc_fiber* fib, rusty::ffi::c_void* arg);
void fiber_engine_resume(srpc_fiber* fib);
void fiber_engine_yield(srpc_fiber* fib);
void fiber_engine_destroy(srpc_fiber* fib);

// @unsafe
extern "C" void fiber_task_entry_thunk(rusty::ffi::c_void* arg) {
    fiber_task_t* const task = const_cast<fiber_task_t*>(reinterpret_cast<const fiber_task_t*>(arg));
    // @unsafe
    {
        ((*task)).run_body();
    }
}

void fiber_engine_start(srpc_fiber* fib, rusty::ffi::c_void* arg) {
    // @unsafe
    {
        srpc_fiber_init(fib, std::move(kDefaultStackBytes), std::move(fiber_task_entry_thunk), arg);
        srpc_fiber_resume(fib);
    }
}

void fiber_engine_resume(srpc_fiber* fib) {
    // @unsafe
    {
        srpc_fiber_resume(fib);
    }
}

void fiber_engine_yield(srpc_fiber* fib) {
    // @unsafe
    {
        srpc_fiber_yield(fib);
    }
}

void fiber_engine_destroy(srpc_fiber* fib) {
    // @unsafe
    {
        srpc_fiber_destroy(fib);
    }
}

void fiber_task_body_invoke(FiberTaskFn& f, fiber_yield_t& y) {
    verify(static_cast<bool>(f));
    (f)(y);
}
/*RUSTYCPP:GEN-END id=reactor.87*/

// @unsafe - the mem-init list stays C++ (yield_ needs `*this`); the
// engine handshake it used to inline is the DSL above. Hands `this` to
// the C engine as the entry-thunk argument.
fiber_task_t::fiber_task_t(TaskFn fn)
    : fn_(std::move(fn)),
      yield_(fiber_yield_t::new_(*this)) {
  fiber_engine_start(&fib_, this);
}

fiber_task_t::~fiber_task_t() { fiber_engine_destroy(&fib_); }

void fiber_task_t::operator()() { fiber_engine_resume(&fib_); }

void fiber_task_t::yield_to_caller() { fiber_engine_yield(&fib_); }

void fiber_task_t::run_body() { fiber_task_body_invoke(fn_, yield_); }

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
// Authored as inline Rust DSL — the waitall_make_from construction
// spelling, plus the §7.59 turbofish factory call for finalize_event_
// (a variadic factory WITH an argument also lowers).
#if RUSTYCPP_RUST
fn quorum_event_make(n_total: i32, quorum: i32) -> Arc<QuorumEvent> {
    let sp = rusty::Arc::<QuorumEvent>::make(
        rusty::Cell::<EventStatus>::new(rrr::EventStatus::INIT),      // status_
        rusty::thread::current_id(),                             // owner_thread_
        EventState {},                                           // state_
        rusty::Cell::<bool>::new(true),                          // prunable_
        rusty::sync::Weak::<EventPollable>(),                    // self_
        rusty::Cell::<i32>::new(0i32),                           // n_voted_yes_
        rusty::Cell::<i32>::new(0i32),                           // n_voted_no_
        rusty::RefCell::<rusty::HashMap<u16, rrr::i64>>(rusty::HashMap::<u16, rrr::i64>::new()), // xids_
        n_total,                                                 // n_total_
        quorum,                                                  // quorum_
        rusty::Cell::<QuorumPolicy>::new(QuorumPolicy::DEFAULT), // policy_
        rusty::Cell::<bool>::new(false),                         // committed_seen_
        rusty::Cell::<i32>::new(0i32),                           // num_leader_
        rusty::Cell::<i32>::new(0i32),                           // n_leader_yes_
        rusty::Cell::<i32>::new(0i32),                           // n_leader_no_
        rusty::Cell::<i64>::new(0i64),                           // highest_term_
        rusty::Cell::<bool>::new(false),                         // timeouted_
        rusty::Cell::<u32>::new(0u32),                           // leader_id_
        rusty::Cell::<i64>::new(-1i64),                          // par_id_
        rusty::Cell::<u64>::new(18446744073709551615u64),        // id_ (u64 -1)
        rrr::create_sp_int_event(n_total),       // finalize_event_
    );
    event_state_seed(sp.state_);
    return sp;
}

fn create_sp_quorum_event(n_total: i32, quorum: i32) -> Arc<QuorumEvent> {
    reactor_setup_sp_event::<QuorumEvent>(quorum_event_make(n_total, quorum))
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.38 version=1 rust_sha256=04df3a715f2309fdb6137888bf0396e12c0d50c627d8b437203d41ed37ce4b9c*/
rusty::Arc<QuorumEvent> quorum_event_make(int32_t n_total, int32_t quorum) {
    auto sp = rusty::Arc<QuorumEvent>::make(rusty::Cell<EventStatus>::new_(rusty::clone(rusty::clone(rrr::EventStatus_INIT()))), rusty::thread::current_id(), EventState{}, rusty::Cell<bool>::new_(true), rusty::sync::Weak<EventPollable>(), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::RefCell<rusty::HashMap<uint16_t, rrr::i64>>(rusty::HashMap<uint16_t, rrr::i64>()), std::move(n_total), std::move(quorum), rusty::Cell<QuorumPolicy>::new_(rusty::clone(rusty::clone(QuorumPolicy_DEFAULT()))), rusty::Cell<bool>::new_(false), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<int64_t>::new_(static_cast<int64_t>(0)), rusty::Cell<bool>::new_(false), rusty::Cell<uint32_t>::new_(static_cast<uint32_t>(0)), rusty::Cell<int64_t>::new_(static_cast<int64_t>(-1)), rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(18446744073709551615)), rrr::create_sp_int_event(std::move(n_total)));
    event_state_seed(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.state_); }) { return (__r.state_); } else if constexpr (requires { (__r.state__field); }) { return (__r.state__field); } else if constexpr (requires { ((*__r).state_); }) { return ((*__r).state_); } else { return ((*__r).state__field); } }(sp)));
    return std::move(sp);
}

rusty::Arc<QuorumEvent> create_sp_quorum_event(int32_t n_total, int32_t quorum) {
    return reactor_setup_sp_event<QuorumEvent>(quorum_event_make(std::move(n_total), std::move(quorum)));
}
/*RUSTYCPP:GEN-END id=reactor.38*/

// The pair Vec type behind QuorumFinalizeFn's parameter — std::pair
// has no DSL spelling, so both the alias and the copy-out kernel below
// carry that shape for the DSL finalize body.
using QuorumDanglingVec = rusty::Vec<std::pair<uint16_t, rrr::i64> >;

// @unsafe - copy-out of the xids_ map into the pair Vec. The stated
// cause ("std::pair has no DSL spelling") never bound here: the alias
// carries the pair shape and the loop only forwards whatever the map
// yields, so `rusty::iter` + `for_in` reach it without the body naming a
// pair at all — the same shape pollworker_take_removals uses over the
// HashSet. The tuple-to-pair conversion at `push` is the C++23 pair-like
// constructor, i.e. exactly what the hand-written `v.push(it)` relied on
// (verified standalone). Binding the RefMut to a named `guard` also
// makes the borrow outlive the loop explicitly instead of leaning on
// range-for temporary lifetime extension. Read-only copy, no aliasing;
// runs BEFORE the wait, per comment A in the DSL body below.
#if RUSTYCPP_RUST
fn quorum_collect_dangling(qe: *const QuorumEvent) -> QuorumDanglingVec {
    let mut v: QuorumDanglingVec = QuorumDanglingVec::new();
    let guard = (*qe).xids_.borrow_mut();
    for it in (*guard).iter() {
        v.push(it);
    }
    v
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.96 version=1 rust_sha256=56a64b9c54ab59ec90b01f0c70794616c7599275085d4d40dbeccc19b22747d1*/
QuorumDanglingVec quorum_collect_dangling(const QuorumEvent* qe) {
    QuorumDanglingVec v = QuorumDanglingVec::new_();
    auto&& guard = (*qe).xids_.borrow_mut();
    for (auto&& it : rusty::for_in(rusty::iter((rusty::detail::deref_if_pointer_like(guard))))) {
        v.push(std::move(it));
    }
    return std::move(v);
}
/*RUSTYCPP:GEN-END id=reactor.96*/

// Spawns a background fiber that parks on the finalize event; faithful
// port of the former QuorumEvent::finalize. The closure captures a raw
// QuorumEvent pointer instead of the old `&self` — same lifetime
// discipline: `self` is only touched (final_ev clone + dangling
// copy-out) BEFORE wait_timeout (comment A), after which the quorum
// event may already be freed. The finalize_func call uses the &mut
// alias (a by-value Vec local would be move-wrapped at its last use
// and fail to bind the Function's Vec& parameter).
#if RUSTYCPP_RUST
fn quorum_event_finalize(qe: &QuorumEvent, timeout: u64,
                         finalize_func: QuorumFinalizeFn) {
    let qe_ptr: *const QuorumEvent = &raw const *qe;
    Fiber::create_run(move || {
        let final_ev = (*qe_ptr).finalize_event_.clone(); // comment A
        let mut dangling_rpc: QuorumDanglingVec = quorum_collect_dangling(qe_ptr);
        (*final_ev).wait_timeout(timeout);
        // A: by the time this fires, the quorum event could have been
        // freed. Avoid touching qe_ptr or its members after this line.
        if (*final_ev).status_.get() == rrr::EventStatus::TIMEOUT {
            // Didn't receive all RPC replies.
            let dr: &mut QuorumDanglingVec = &mut dangling_rpc;
            let _ret = finalize_func(dr);
            // Drain guard: a TIMEOUT'd event is never evicted by the
            // reactor loop (extract takes READY, retain drops DONE), so
            // a registered finalize_event_ would otherwise linger in the
            // queues forever at broadcast rate. Mark it DONE here (we
            // run on the owner thread) so the next pass evicts and
            // prune can free it.
            (*final_ev).status_.set(rrr::EventStatus::DONE);
        }
    });
}

// Reads/clears the reactor's shared slow_ flag (matches the former
// QuorumEvent::is_slow / Event::is_slow); the param is unused — the
// flag is reactor-global.
fn quorum_event_is_slow(_qe: &QuorumEvent) -> bool {
    let r = Reactor::get_reactor();
    let result: bool = (*r).slow_.get();
    (*r).slow_.set(false);
    result
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.39 version=1 rust_sha256=4f7bcbb304bfc65da1a49e471b0f1d83f0dca8671c938f4dcf9a9a19b4e57584*/
void quorum_event_finalize(const QuorumEvent& qe, uint64_t timeout, QuorumFinalizeFn finalize_func) {
    const QuorumEvent* qe_ptr = &qe;
    Fiber::create_run([=, finalize_func = std::move(finalize_func), qe_ptr = std::move(qe_ptr), timeout = std::move(timeout)]() mutable {
const auto final_ev = rusty::clone((*qe_ptr).finalize_event_);
QuorumDanglingVec dangling_rpc = quorum_collect_dangling(qe_ptr);
((rusty::detail::deref_if_pointer_like(final_ev))).wait_timeout(std::move(timeout));
if ((rusty::detail::deref_if_pointer_like(final_ev)).status_.get() == rusty::clone(rrr::EventStatus_TIMEOUT())) {
    QuorumDanglingVec& dr = dangling_rpc;
    const auto _ret = finalize_func(dr);
    (rusty::detail::deref_if_pointer_like(final_ev)).status_.set(rusty::clone(rusty::clone(rrr::EventStatus_DONE())));
}
});
}

bool quorum_event_is_slow(const QuorumEvent& _qe) {
    const auto r = Reactor::get_reactor();
    bool result = (rusty::detail::deref_if_pointer_like(r)).slow_.get();
    (rusty::detail::deref_if_pointer_like(r)).slow_.set(false);
    return std::move(result);
}
/*RUSTYCPP:GEN-END id=reactor.39*/


}  // namespace janus (definitions)
