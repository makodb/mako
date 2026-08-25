//! Canonical Rust source for the historical `srpc.reactor` provider.
//!
//! This file intentionally retains the historical `.cpp` path.  The crate
//! view is `#[path = "../reactor/reactor.rs"] pub mod reactor;` in the
//! generated `src/lib.rs`, which points straight back at this source of truth.
//!
//! Direct Rust execution of this module is intentionally unsupported for now.
//! The inert `cfg_attr(any(), thread_local)` markers below preserve the
//! generated-C++ `thread_local` contract, but rustc sees ordinary mutable
//! globals.  Cargo therefore supplies parsing, type, auto-trait, and facade
//! checks only; native generated-C++ TLS and multithread runtime tests remain
//! mandatory promotion gates.
//!
//! Stackless wakeups use a private owner-thread ingress.  Wakers retain only
//! thread-safe heap tickets/queues; the Reactor pointer never crosses threads,
//! and every Context/Waker allocation remains stable through Task destruction.
//! Native generated-C++ race, teardown, layout, and symbol gates are still
//! mandatory before promotion.

// `non_upper_case_globals` is NOT repeated here: `src/lib.rs` already carries
// it on `pub mod reactor;`, and repeating it is a `duplicated_attributes`.
#![allow(
    non_camel_case_types,
    non_snake_case,
    unsafe_code,
    unused_imports,
    unused_mut,
)]
// The `static mut` thread-locals above are this file's rustc-side MODEL of the
// generated C++ `thread_local` namespace variables (module header, paragraph 2).
// Under that model every read is a shared reference to a `static mut`, so
// `static_mut_refs` fires once per access — 15 times — for a hazard the real
// lowering does not have: each C++ object is per-thread, so no two threads ever
// alias one. The lint cannot be fixed per site: `thread_local!` changes the
// generated storage, and dropping `mut` (rustc's own suggestion for the
// `RefCell` one) does not compile, because a non-`mut` static requires `Sync`
// and `RefCell<Option<Rc<Fiber>>>` is not. Native generated-C++ TLS, race and
// teardown gates remain mandatory and are what actually check this.
#![allow(static_mut_refs)]

use rusty::cpp_inherit;
use std::cell::{Cell, RefCell, RefMut};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::rc::Rc;
use std::sync::{Arc, Weak};
use std::sync::atomic::{AtomicBool, AtomicU64};

use crate::basetypes::Time;
use crate::epoll_wrapper::{Epoll, PollMode, PollReady, Pollable};
use crate::misc::Job;
use crate::pollable_proxy::{PollableBase, PollableProxy};
use crate::logging::Log;
use cpp::srpc::{debugging as cpp_debugging, logging as cpp_logging};
use cpp::std as cpp_std;
use rusty as cpp;

type LegacyStdString = String;
pub type SrcFileCStr = &'static str;
pub type EventTestFn = rusty::Function<dyn Fn(i32) -> bool>;
pub type FiberFn = rusty::Function<dyn FnMut()>;
pub type FiberTaskFn = rusty::Function<dyn FnMut(&mut fiber_yield_t)>;
pub type StacklessPollFn = rusty::Function<dyn FnMut(&mut rusty::Context) -> bool>;
pub type TaskVoid = rusty::Task<()>;
pub type PollCmdReceiver = rusty::sync::mpsc::Receiver<PollCommand>;
pub type FdPollableMap = HashMap<i32, PollableProxy>;
pub type FdModeMap = HashMap<i32, i32>;
pub type FdSet = HashSet<i32>;
pub type JobSet = rusty::ReactorJobSet<Arc<dyn Job>>;
pub type PollJoinSlot = rusty::Mutex<Option<rusty::thread::JoinHandle<()>>>;
// The historical callback ABI is Vec<std::pair<u16, i64>>, not a Rust tuple.
// Use the checked facade that maps exactly to std::pair in generated C++.
pub type QuorumDanglingVec = Vec<rusty::StdPair<u16, i64>>;
pub type QuorumFinalizeFn = rusty::Function<dyn FnMut(&mut QuorumDanglingVec) -> bool>;
pub type StacklessProfileCountU64 = rusty::sync::atomic::AtomicU64;
pub type StacklessProfileCountUsize = rusty::sync::atomic::AtomicUsize;
// rustc models C `char` as an i8 on the supported Unix targets, while the
// production C++ declaration must retain the distinct built-in `char` type.
// `rust-type-map.toml` maps this established facade name to C++ `char`.
type LegacyCChar = i8;

type srpc_fiber_ctx = rusty::ReactorFiberContext;
type srpc_fiber = rusty::ReactorFiberState;

unsafe extern "C" {
    fn srpc_fiber_init(
        fiber: *mut srpc_fiber,
        stack_bytes: usize,
        entry_fn: unsafe extern "C" fn(*mut core::ffi::c_void),
        entry_arg: *mut core::ffi::c_void,
    );
    fn srpc_fiber_destroy(fiber: *mut srpc_fiber);
    fn srpc_fiber_resume(fiber: *mut srpc_fiber);
    fn srpc_fiber_yield(fiber: *mut srpc_fiber);
    fn getenv(name: *const LegacyCChar) -> *mut LegacyCChar;
    // Reactor platform / build-configuration facade; see reactor/srpc_fiber.h.
    // Neither fact can be a Rust constant: `SYS_gettid`'s number is
    // arch-specific and `REUSING_FIBER` is a build flag, and canonical Rust
    // has to compile under rustc where neither macro exists.  The C shim is
    // compiled by the same build, with the same flags, against the same
    // platform headers, so it answers for the library that is actually built.
    fn srpc_reactor_gettid() -> i64;
    fn srpc_reactor_reusing_fiber() -> i32;
}

// The calling thread's kernel thread id.  Replaces `syscall(SYS_gettid)` with
// the literal 186, which is correct only on x86-64 Linux.
fn current_thread_gettid() -> i64 {
    // The shim takes no arguments, touches no Rust state, and cannot fail.
    unsafe { srpc_reactor_gettid() }
}

// The historical `REUSING_FIBER` macro:
//     #if defined(REUSE_FIBER) || defined(REUSE_CORO)
// Deliberately NOT a `pub const`: the incumbent public surface had no such
// entity, and a module-exported constant would freeze one build's answer
// under the name of a macro that consumers can still define differently.
fn reusing_fiber() -> bool {
    // Same contract as above: an argument-free query over a compile-time
    // constant in the shim translation unit.
    unsafe { srpc_reactor_reusing_fiber() != 0 }
}

// NOT named `verify`, for exactly the reason spelled out for
// `reactor_log_line` below, and MEASURED here rather than assumed:
// `srpc.debugging` exports `template<typename Expr> void verify(const Expr&,
// source_location = current())` into the SAME C++ namespace `srpc`. A local
// non-template `srpc::verify(bool)` is an exact match for a `bool` argument
// and therefore BEATS that template in overload resolution — so the forward
// below resolved back to ITSELF. Infinite recursion is UB, so at the
// production `-O2` the whole body was deleted: `srpc::verify(bool)` compiled
// to `push %rbp; mov %rsp,%rbp; pop %rbp; ret` and EVERY assertion in this
// file silently did nothing, while an unoptimized build stack-overflowed.
// The incumbent had no wrapper at all; it called the imported template
// directly, which is what this rename restores.
fn reactor_verify(value: bool) {
    // The checked foreign facade models the same abort-on-false contract as
    // the imported `srpc.debugging` provider.
    unsafe { cpp_debugging::verify(value) };
}

// NOT named `log_line`: the imported `srpc::logging::log_line` lands in the
// same C++ namespace `srpc`, so a same-named local wrapper joins its overload
// set and the forwarding call below resolves back to ITSELF. The parameter is
// `LegacyStdString` (the established alias every other module uses for a
// value that crosses into the C++ logger) rather than `String`, so the
// forward is a plain `const std::string&` bind instead of an impossible
// `rusty::String` -> `std::string` conversion.
fn reactor_log_line(level: i32, line: i32, file: *const i8, message: LegacyStdString) {
    // The production logger consumes the message synchronously and retains no
    // borrow; the owned Rust value therefore has exactly the required extent.
    unsafe { cpp_logging::log_line(level, line, file, &message) };
}

fn move_matching<T, F>(source: &mut VecDeque<T>, destination: &mut VecDeque<T>, mut predicate: F)
where
    F: FnMut(&T) -> bool,
{
    let count = source.len();
    for _ in 0..count {
        let item = source.pop_front().unwrap();
        if predicate(&item) {
            destination.push_back(item);
        } else {
            source.push_back(item);
        }
    }
}

#[cfg_attr(any(), thread_local)]
pub static mut sp_reactor_th_: Option<Rc<Reactor>> = Option::<Rc<Reactor>>::None;
#[cfg_attr(any(), thread_local)]
pub static mut sp_disk_reactor_th_: Option<Rc<Reactor>> = Option::<Rc<Reactor>>::None;
// MEASURED, not assumed: rustc's own `static_mut_refs` suggestion here —
// "this type already provides interior mutability, so its binding doesn't
// need to be declared as mutable" — DOES NOT COMPILE. A non-`mut` static
// requires `Sync`, and `RefCell<Option<Rc<Fiber>>>` is neither. `static mut`
// is how rustc models a C++ namespace-scope `thread_local` in this facade
// (see the module header); the real per-thread storage comes from the
// marker above. See the crate-level `static_mut_refs` allow below.
#[cfg_attr(any(), thread_local)]
pub static mut sp_running_fiber_th_: RefCell<Option<Rc<Fiber>>> = RefCell::new(Option::<Rc<Fiber>>::None);

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
#[repr(i32)]
pub enum EventStatus {
    INIT = 0,
    WAIT = 1,
    READY = 2,
    DONE = 3,
    TIMEOUT = 4,
    DEBUG = 5,
}

#[repr(C)]
pub struct EventState {
    pub __debug_creator: i32,
    pub test_: RefCell<EventTestFn>,
    pub wakeup_time_: Cell<u64>,
    pub rcd_wait_: Cell<bool>,
    pub wait_place_: RefCell<String>,
    pub wp_fiber_: RefCell<rusty::rc::Weak<Fiber>>,
}

impl EventState {
    pub fn new() -> Self {
        Self {
            __debug_creator: 0,
            test_: RefCell::new(Default::default()),
            wakeup_time_: Cell::new(0),
            rcd_wait_: Cell::new(false),
            wait_place_: RefCell::new(String::new()),
            wp_fiber_: RefCell::new(rusty::rc::Weak::new()),
        }
    }
}

pub trait EventPollable {
    fn test(&self) -> bool;
    fn is_ready(&self) -> bool;
    fn log(&self);
    fn status(&self) -> EventStatus;
    fn set_status(&self, s: EventStatus);
    fn wakeup_time(&self) -> u64;
    fn prunable(&self) -> bool;
    fn set_prunable(&self, v: bool);
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>>;
}

trait EventCore: EventPollable {
    fn core_status(&self) -> &Cell<EventStatus>;
    fn core_owner_thread(&self) -> rusty::thread::ThreadId;
    fn core_state(&self) -> &EventState;
    fn core_state_mut(&mut self) -> &mut EventState;
    fn core_self(&self) -> &Weak<dyn EventPollable>;
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable>;
    fn core_is_composite(&self) -> bool;
}

fn event_core_set_self<W: EventCore>(ev: &mut W, p: Weak<dyn EventPollable>) {
    *ev.core_self_mut() = p;
}
fn event_core_wakeup_time<W: EventCore>(ev: &W) -> u64 {
    ev.core_state().wakeup_time_.get()
}
fn event_core_upgrade_fiber<W: EventCore>(ev: &W) -> Option<Rc<Fiber>> {
    ev.core_state().wp_fiber_.borrow().upgrade()
}

fn event_core_record_place<W: EventCore>(self_: &W, file: SrcFileCStr, line: i32) {
    let tag: String = format!("{}:{}", file, line);
    let mut g = self_.core_state().wait_place_.borrow_mut();
    g.push_str(&tag);
    self_.core_state().rcd_wait_.set(true);
}

#[repr(C)]
pub struct BoxEvent<Type> {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub content_: RefCell<Type>,
    pub is_set_: Cell<bool>,
}

impl<Type: Clone + Default + 'static> BoxEvent<Type> {
    pub fn get(&self) -> Type {
        boxevent_get(self)
    }
    pub fn set(&self, c: &Type) {
        boxevent_set(self, c)
    }
    pub fn clear(&self) {
        boxevent_clear(self)
    }
    pub fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    pub fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    pub fn is_composite_event(&self) -> bool {
        false
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
        event_core_set_self(self, self_ptr)
    }
}

#[cpp_inherit]
impl<Type: Clone + Default + 'static> EventPollable for BoxEvent<Type> {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl<Type: Clone + Default + 'static> EventCore for BoxEvent<Type> {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
}

fn boxevent_make<Type: Clone + Default + 'static>() -> Arc<BoxEvent<Type>> {
    let sp: Arc<BoxEvent<Type>> = Arc::new(BoxEvent::<Type> {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<BoxEvent<Type>>::new(),
        content_: RefCell::new(Default::default()),
        is_set_: Cell::new(false),
    });
    event_state_seed(&sp.state_);
    sp
}

// Returns the slot payload by value (copy out of the RefCell).
fn boxevent_get<Type: Clone>(ev: &BoxEvent<Type>) -> Type {
    let g = ev.content_.borrow();
    (*g).clone()
}

fn boxevent_set<Type: Clone + Default + 'static>(ev: &BoxEvent<Type>, c: &Type) {
    ev.is_set_.set(true);
    {
        let mut g = ev.content_.borrow_mut();
        *g = c.clone();
    }
    ev.test();
}

fn boxevent_clear<Type: Default>(ev: &BoxEvent<Type>) {
    ev.is_set_.set(false);
    let mut g = ev.content_.borrow_mut();
    let _old = core::mem::take(&mut *g);
}

#[repr(C)]
pub struct IntEvent {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub value_: Cell<i32>,
    pub target_: Cell<i32>,
}

impl IntEvent {
    pub fn get(&self) -> i32 {
        self.value_.get()
    }
    pub fn set(&self, n: i32) -> i32 {
        int_event_set(self, n)
    }
    pub fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    pub fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    pub fn record_place(&self, file: SrcFileCStr, line: i32) {
        event_core_record_place(self, file, line)
    }
    pub fn get_fiber_id(&self) -> u64 {
        event_core_get_fiber_id()
    }
    pub fn is_composite_event(&self) -> bool {
        false
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl EventCore for IntEvent {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
}

fn int_event_set(ev: &IntEvent, n: i32) -> i32 {
    let t: i32 = ev.value_.get();
    ev.value_.set(n);
    event_test_impl(ev);
    t
}

fn int_event_is_ready(ev: &IntEvent) -> bool {
    let guard = ev.state_.test_.borrow();
    if !guard.is_empty() {
        return (*guard)(ev.value_.get());
    }
    ev.value_.get() >= ev.target_.get()
}

#[repr(C)]
pub struct SharedIntEvent {
    pub value_: i32,
    pub events_: Vec<Arc<IntEvent>>,
}

impl SharedIntEvent {
    pub fn set(&mut self, v: &i32) -> i32 {
        shared_int_event_set(self, *v)
    }

    pub fn wait(&mut self, f: EventTestFn) {
        shared_int_event_wait(self, f)
    }

    pub fn wait_until_gte(&mut self, x: i32, timeout: i32) -> bool {
        shared_int_event_wait_until_gte(self, x, timeout)
    }
}

#[repr(C)]
pub struct NeverEvent {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
}

impl NeverEvent {
    pub fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    pub fn record_place(&self, file: SrcFileCStr, line: i32) {
        event_core_record_place(self, file, line)
    }
    pub fn is_composite_event(&self) -> bool {
        false
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl EventCore for NeverEvent {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
}

#[repr(C)]
pub struct TimeoutEvent {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub wakeup_time_: u64,
    pub wait_us_: u64,
}

impl TimeoutEvent {
    pub fn wait(&self) {
        event_wait_impl(self, self.wait_us_)
    }
    pub fn is_composite_event(&self) -> bool {
        false
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl EventCore for TimeoutEvent {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
}

fn timeout_event_is_ready(self_: &TimeoutEvent) -> bool {
    Time::now(true) > self_.wakeup_time_
}

#[repr(C)]
pub struct WaitAny {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub events_: Vec<Arc<dyn EventPollable>>,
}

impl WaitAny {
    pub fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    pub fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    pub fn is_composite_event(&self) -> bool {
        true
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl EventCore for WaitAny {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { true }
}

#[repr(C)]
pub struct WaitAll {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub events_: RefCell<Vec<Arc<dyn EventPollable>>>,
}

impl WaitAll {
    pub fn add_event(&self, x: Arc<dyn EventPollable>) {
        // Bind the guard, then deref — chaining `.borrow_mut().push(x)`
        // mis-lowers to push(Vec::from_iter(x)). See §7.33.
        let mut g = self.events_.borrow_mut();
        (*g).push(x);
    }
    pub fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    pub fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    pub fn is_composite_event(&self) -> bool {
        true
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl EventCore for WaitAll {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { true }
}

pub const kDefaultStackBytes: usize = 1usize << 20;

#[repr(C)]
pub struct fiber_yield_t {
    pub task_: *mut fiber_task_t,
}

impl fiber_yield_t {
    pub fn new(task: &mut fiber_task_t) -> fiber_yield_t {
        fiber_yield_t {
            task_: task as *mut fiber_task_t,
        }
    }
}

#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
#[repr(C)]
pub struct fiber_task_t {
    pub fn_: FiberTaskFn,
    pub yield_: fiber_yield_t,
    pub fib_: srpc_fiber,
    pub _pin: rusty::marker::PhantomPinned,
}

impl fiber_task_t {
    pub fn new(fn_: FiberTaskFn) -> fiber_task_t {
        fiber_task_t {
            fn_,
            yield_: fiber_yield_t { task_: core::ptr::null_mut() },
            // srpc_fiber_init overwrites every field before use. Its C ABI
            // representation consists only of nullable pointers/integers, so
            // the all-zero bit pattern is valid on both sides of the port.
            fib_: unsafe {
                core::mem::MaybeUninit::<srpc_fiber>::zeroed().assume_init()
            },
            _pin: rusty::marker::PhantomPinned {},
        }
    }
}

impl Drop for fiber_task_t {
    #[cfg_attr(any(), cpp_noexcept)]
    fn drop(&mut self) {
        fiber_engine_destroy(&mut self.fib_);
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
#[repr(i32)]
pub enum FiberStatus {
    INIT = 0,
    STARTED = 1,
    PAUSED = 2,
    RESUMED = 3,
    FINISHED = 4,
    FINALIZING = 5,
    RECYCLED = 6,
}

#[cfg_attr(any(), thread_local)]
pub static mut g_fiber_global_id: u64 = 0;

fn fiber_next_global_id() -> u64 {
    unsafe {
        let r = g_fiber_global_id;
        g_fiber_global_id = r + 1u64;
        r
    }
}

#[repr(C)]
pub struct Fiber {
    pub dep_id_: u64,
    pub need_finalize_: bool,
    pub id: Cell<u64>,
    pub status_: Cell<FiberStatus>,
    pub needs_finalize_: Cell<bool>,
    pub func_: RefCell<FiberFn>,
    pub fiber_task_: RefCell<Option<Box<fiber_task_t>>>,
    pub fiber_yield_: Cell<*mut fiber_yield_t>,
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
    pub _pin: rusty::marker::PhantomPinned,
}

impl Fiber {
    pub fn new(func: FiberFn) -> Fiber {
        Fiber {
            dep_id_: 0u64,
            need_finalize_: false,
            id: Cell::<u64>::new(fiber_next_global_id()),
            status_: Cell::<FiberStatus>::new(FiberStatus::INIT),
            needs_finalize_: Cell::<bool>::new(false),
            func_: RefCell::<FiberFn>::new(func),
            fiber_task_: Default::default(),
            fiber_yield_: Cell::<*mut fiber_yield_t>::new(core::ptr::null_mut()),
            _pin: rusty::marker::PhantomPinned {},
        }
    }

    pub fn current_fiber() -> Option<Rc<Fiber>> {
        fiber_current_fiber()
    }

    pub fn create_run<Func>(func: Func) -> Rc<Fiber>
    where
        Func: FnMut() + 'static,
    {
        Fiber::create_run_impl(FiberFn::from_callable(func), "", 0i64)
    }

    pub fn create_run_impl(func: FiberFn, file: SrcFileCStr, line: i64) -> Rc<Fiber> {
        fiber_create_run_impl(func, file, line)
    }

    pub fn sleep(microseconds: u64) {
        fiber_sleep(microseconds);
    }

    pub fn run(&self) {
        fiber_run(self);
    }

    pub fn yield_(&self) {
        fiber_do_yield(self);
    }

    pub fn continue_(&self) {
        fiber_do_continue(self);
    }

    pub fn finished(&self) -> bool {
        fiber_is_finished(self)
    }
}

fn fiber_registry_key(fiber: &Rc<Fiber>) -> usize {
    let ptr: *const Fiber = Rc::<Fiber>::as_ptr(fiber);
    ptr as usize
}

#[repr(C)]
pub struct StacklessTaskEntry {
    pub active: bool,
    pub queued: bool,
    pub poll_once: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool>,
}

const STACKLESS_UNREGISTERED_SLOT: usize = usize::MAX;

struct StacklessWakeTicket {
    slot: rusty::sync::atomic::AtomicUsize,
    enqueued: rusty::sync::atomic::AtomicBool,
}

struct StacklessWakeIngress {
    accepting: rusty::sync::atomic::AtomicBool,
    pending: rusty::Mutex<VecDeque<Arc<StacklessWakeTicket>>>,
}

struct StacklessWakeBinding {
    ticket: Arc<StacklessWakeTicket>,
    waker: rusty::Waker,
    context: rusty::Context,
}

struct StacklessWakeOwner {
    reactor_key: usize,
    ingress: Option<Arc<StacklessWakeIngress>>,
    bindings: Vec<Option<Box<StacklessWakeBinding>>>,
}

struct StacklessResultTaskState<T, OnReady> {
    // C++ destroys fields in reverse declaration order.  Keep Task last so
    // its retained Context pointer is destroyed before the early binding.
    early_binding: Box<StacklessWakeBinding>,
    on_ready: RefCell<Option<OnReady>>,
    task: RefCell<rusty::Task<T>>,
}

struct StacklessVoidTaskState {
    // See StacklessResultTaskState: Task must die before its Context/Waker.
    early_binding: Box<StacklessWakeBinding>,
    task: RefCell<TaskVoid>,
}

// ---------------------------------------------------------------------------
// Teardown owes waiters an error (W2)
// ---------------------------------------------------------------------------
//
// `accepting=false` makes a late foreign wake a defined no-op.  That is
// memory-safe, and it is also completely silent: it says nothing to the thread
// blocked waiting for the completion that wake would have driven.  Silence is
// exactly the observed client-hang shape -- a close-style wait whose only
// release is an `on_ready` callback that teardown destroyed -- so every
// teardown path that can strand a waiter routes through the counters below and
// logs at ERROR.  A cancelled waiter is an error; it is never nothing.
//
// What the carrier can and cannot promise here is worth stating exactly.  The
// completion callback's argument cannot be synthesised at teardown: the spawn
// API takes `FnMut(T)` and teardown has no `T`, and inventing one would report
// a success that did not happen.  So the guarantee is:
//
//   (a) the callback, the Task and every capture they own are destroyed
//       promptly, on the owner thread, at one defined point -- so a
//       cancellation-safe capture (a promise/sender whose Drop completes its
//       peer with an error) reaches its error path there rather than at some
//       unbounded later time;
//   (b) the cancellation is counted and logged at ERROR, so it is observable
//       instead of silent, and the native battery can assert the error path
//       was taken;
//   (c) no teardown path leaks the task state -- a leak would be the one truly
//       unreleasable form of silence, because the captures would never run.
//
// The residual obligation that stays with the caller is (a)'s premise: a
// waiter released only from inside the callback *body* must be backed by a
// cancellation-safe capture.  `stackless_client_hang_regression` variant (b) in
// the native battery pins that contract end to end.

struct StacklessCancelCounters {
    teardown_tasks: rusty::sync::atomic::AtomicU64,
    admitted_completions: rusty::sync::atomic::AtomicU64,
    pending_wakes: rusty::sync::atomic::AtomicU64,
    rejected_spawns: rusty::sync::atomic::AtomicU64,
}

// Deliberately the same shape as `g_stackless_profile`, which the incumbent
// object proves carries no owned strong symbol (it is absent from the 300-entry
// manifest).  Atomics give interior mutability, so the binding need not be mut.
static g_stackless_cancel: StacklessCancelCounters = StacklessCancelCounters {
    teardown_tasks: rusty::sync::atomic::AtomicU64::new(0u64),
    admitted_completions: rusty::sync::atomic::AtomicU64::new(0u64),
    pending_wakes: rusty::sync::atomic::AtomicU64::new(0u64),
    rejected_spawns: rusty::sync::atomic::AtomicU64::new(0u64),
};

// Plain aggregate: no derives, no methods, so it contributes no symbol either.
pub struct StacklessCancelReport {
    pub teardown_tasks: u64,
    pub admitted_completions: u64,
    pub pending_wakes: u64,
    pub rejected_spawns: u64,
}

// Generic on purpose.  Every helper this repair adds stays a template so it
// cannot introduce an ordinary strong symbol into the exact owned manifest --
// the same C7 discipline the wake registry already follows.
pub fn stackless_cancel_report<WakeDomain>() -> StacklessCancelReport {
    StacklessCancelReport {
        teardown_tasks: g_stackless_cancel.teardown_tasks.load(rusty::sync::atomic::Ordering::Relaxed),
        admitted_completions: g_stackless_cancel.admitted_completions.load(rusty::sync::atomic::Ordering::Relaxed),
        pending_wakes: g_stackless_cancel.pending_wakes.load(rusty::sync::atomic::Ordering::Relaxed),
        rejected_spawns: g_stackless_cancel.rejected_spawns.load(rusty::sync::atomic::Ordering::Relaxed),
    }
}

// MEASURED allow (clippy::extra_unused_type_parameters).  `WakeDomain` is not
// dead: it is the domain tag that gives each wake domain its OWN
// `static thread_local` in the emitted C++, because these lower to
//     template<typename WakeDomain> ... { static thread_local ... OWNERS ...; }
// Dropping it collapses every domain onto one slot AND moves the ABI: the four
// functions stop being templates, so they stop being weak/linkonce
// instantiations and become provider-owned STRONG symbols.  Measured on the
// real object (R/M/T1-srpc.reactor.*): 301 -> 305 unique demangled strong
// symbols, only-AFTER = 4 —
//     srpc::stackless_wake_owners_slot@srpc.reactor()
//     srpc::stackless_wake_reactor_key@srpc.reactor(srpc::Reactor const&)
//     srpc::stackless_wake_request@srpc.reactor(Arc<StacklessWakeIngress> const&,
//                                             Arc<StacklessWakeTicket> const&)
//     srpc::stackless_wake_binding_context@srpc.reactor(Box<StacklessWakeBinding>&)
// which the frozen incumbent oracle does not have.  The removal also cascades:
// four more `WakeDomain` parameters become "unused" the moment these four go,
// and clippy's own --fix leaves the crate not compiling (E0107 x4,
// R/M/fix-T1.log) because it does not update the turbofish call sites.
// The C++ ABI contract wins.
#[allow(clippy::extra_unused_type_parameters)]
fn stackless_wake_owners_slot<WakeDomain>() -> *mut *mut Vec<StacklessWakeOwner> {
    // Keep only a trivially destructible pointer in TLS.  A function-local
    // Vec would be constructed after the namespace TLS Reactor Rc and hence
    // destroyed before that Reactor at thread exit, invalidating every stable
    // Context binding before Reactor::drop could destroy its Tasks.
    #[cfg_attr(any(), thread_local)]
    static mut OWNERS: *mut Vec<StacklessWakeOwner> = core::ptr::null_mut();
    &raw mut OWNERS
}

fn stackless_wake_owners_existing_ptr<WakeDomain>() -> *mut Vec<StacklessWakeOwner> {
    unsafe { *stackless_wake_owners_slot::<WakeDomain>() }
}

fn stackless_wake_owners_ptr<WakeDomain>() -> *mut Vec<StacklessWakeOwner> {
    unsafe {
        let slot = &mut *stackless_wake_owners_slot::<WakeDomain>();
        if (*slot).is_null() {
            let owners = Box::new(Vec::<StacklessWakeOwner>::new());
            *slot = Box::into_raw(owners);
        }
        *slot
    }
}

fn stackless_wake_release_empty_storage<WakeDomain>(owners_ptr: *mut Vec<StacklessWakeOwner>) {
    let mut has_active_owner = false;
    unsafe {
        {
            let owners = &mut *owners_ptr;
            let mut i: usize = 0usize;
            while i < owners.len() {
                if owners[i].reactor_key != STACKLESS_UNREGISTERED_SLOT {
                    has_active_owner = true;
                    break;
                }
                i += 1usize;
            }
        }
        if !has_active_owner {
            let slot = &mut *stackless_wake_owners_slot::<WakeDomain>();
            reactor_verify(*slot == owners_ptr);
            *slot = core::ptr::null_mut();
            drop(Box::from_raw(owners_ptr));
        }
    }
}

// MEASURED allow — see the `extra_unused_type_parameters` note on `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn stackless_wake_reactor_key<WakeDomain>(reactor: &Reactor) -> usize {
    reactor as *const Reactor as usize
}

// MEASURED allow — see the `extra_unused_type_parameters` note on `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn stackless_wake_request<WakeDomain>(ingress: &Arc<StacklessWakeIngress>, ticket: &Arc<StacklessWakeTicket>) {
    if !ingress.accepting.load(rusty::sync::atomic::Ordering::Acquire) {
        return;
    }
    if ticket.enqueued.swap(true, rusty::sync::atomic::Ordering::AcqRel) {
        return;
    }
    let mut pending = ingress.pending.lock().unwrap();
    if ingress.accepting.load(rusty::sync::atomic::Ordering::Acquire) {
        (*pending).push_back(ticket.clone());
    } else {
        ticket.enqueued.store(false, rusty::sync::atomic::Ordering::Release);
    }
}

fn stackless_wake_ingress<WakeDomain>(reactor: &Reactor) -> Arc<StacklessWakeIngress> {
    reactor_verify(rusty::thread::current_id() == reactor.thread_id_.get());
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    let mut reusable: usize = STACKLESS_UNREGISTERED_SLOT;
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                return owners[i].ingress.as_ref().unwrap().clone();
            }
            if reusable == STACKLESS_UNREGISTERED_SLOT
                && owners[i].reactor_key == STACKLESS_UNREGISTERED_SLOT
            {
                reusable = i;
            }
            i += 1usize;
        }
    }

    let ingress = Arc::new(StacklessWakeIngress {
        accepting: rusty::sync::atomic::AtomicBool::new(true),
        pending: rusty::Mutex::new(VecDeque::<Arc<StacklessWakeTicket>>::new()),
    });
    let owner = StacklessWakeOwner {
        reactor_key: key,
        ingress: Some(ingress.clone()),
        bindings: Vec::new(),
    };
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        if reusable == STACKLESS_UNREGISTERED_SLOT {
            owners.push(owner);
        } else {
            owners[reusable] = owner;
        }
    }
    ingress
}

fn stackless_wake_make_binding<WakeDomain>(ingress: Arc<StacklessWakeIngress>) -> Box<StacklessWakeBinding> {
    let ticket = Arc::new(StacklessWakeTicket {
        slot: rusty::sync::atomic::AtomicUsize::new(STACKLESS_UNREGISTERED_SLOT),
        enqueued: rusty::sync::atomic::AtomicBool::new(false),
    });
    let wake_ingress = ingress.clone();
    let wake_ticket = ticket.clone();
    let wake_fn: Box<dyn Fn() + Send + Sync> = Box::new(move || {
        stackless_wake_request::<WakeDomain>(&wake_ingress, &wake_ticket);
    });
    let waker = rusty::Waker { wake_fn };
    let mut binding = Box::new(StacklessWakeBinding {
        ticket,
        waker,
        context: rusty::Context { waker: core::ptr::null_mut() },
    });
    binding.context.waker = &raw mut binding.waker;
    binding
}

// MEASURED allow — see the `extra_unused_type_parameters` note on `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn stackless_wake_binding_context<WakeDomain>(binding: &mut Box<StacklessWakeBinding>) -> &mut rusty::Context {
    &mut binding.context
}

fn stackless_wake_attach<WakeDomain>(reactor: &Reactor, idx: usize, binding: Box<StacklessWakeBinding>) {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                while owners[i].bindings.len() <= idx {
                    owners[i].bindings.push(None);
                }
                reactor_verify(owners[i].bindings[idx].is_none());
                binding.ticket.slot.store(idx, rusty::sync::atomic::Ordering::Release);
                owners[i].bindings[idx] = Some(binding);
                return;
            }
            i += 1usize;
        }
    }
    reactor_verify(false);
}

fn stackless_wake_context_ptr<WakeDomain>(reactor: &Reactor, idx: usize) -> *mut rusty::Context {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                reactor_verify(idx < owners[i].bindings.len());
                let binding = owners[i].bindings[idx].as_mut().unwrap();
                return &raw mut binding.context;
            }
            i += 1usize;
        }
    }
    core::ptr::null_mut()
}

fn stackless_wake_close<WakeDomain>(reactor: &Reactor, idx: usize) {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                if idx < owners[i].bindings.len()
                    && owners[i].bindings[idx].is_some()
                {
                    let binding = owners[i].bindings[idx].as_ref().unwrap();
                    binding.ticket.slot.store(
                        STACKLESS_UNREGISTERED_SLOT,
                        rusty::sync::atomic::Ordering::Release,
                    );
                }
                return;
            }
            i += 1usize;
        }
    }
}

fn stackless_wake_detach<WakeDomain>(reactor: &Reactor, idx: usize) {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    let mut retired: Option<Box<StacklessWakeBinding>> = None;
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                if idx < owners[i].bindings.len() {
                    retired = owners[i].bindings[idx].take();
                }
                break;
            }
            i += 1usize;
        }
    }
    drop(retired);
}

fn stackless_wake_take_pending<WakeDomain>(reactor: &Reactor) -> Vec<usize> {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    let mut ingress: Option<Arc<StacklessWakeIngress>> = None;
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                ingress = owners[i].ingress.as_ref().cloned();
                break;
            }
            i += 1usize;
        }
    }
    let mut ready: Vec<usize> = Vec::new();
    if ingress.is_none() {
        return ready;
    }
    let ingress = ingress.unwrap();
    let mut pending = ingress.pending.lock().unwrap();
    while !(*pending).is_empty() {
        let ticket = (*pending).pop_front().unwrap();
        ticket.enqueued.store(false, rusty::sync::atomic::Ordering::Release);
        let idx = ticket.slot.load(rusty::sync::atomic::Ordering::Acquire);
        if idx != STACKLESS_UNREGISTERED_SLOT {
            ready.push(idx);
        }
    }
    ready
}

fn stackless_wake_shutdown_begin<WakeDomain>(reactor: &Reactor) {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    let mut ingress: Option<Arc<StacklessWakeIngress>> = None;
    let owners_ptr = stackless_wake_owners_existing_ptr::<WakeDomain>();
    if owners_ptr.is_null() {
        return;
    }
    unsafe {
        let owners = &mut *owners_ptr;
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                ingress = owners[i].ingress.as_ref().cloned();
                let mut j: usize = 0usize;
                while j < owners[i].bindings.len() {
                    if owners[i].bindings[j].is_some() {
                        let binding = owners[i].bindings[j].as_ref().unwrap();
                        binding.ticket.slot.store(
                            STACKLESS_UNREGISTERED_SLOT,
                            rusty::sync::atomic::Ordering::Release,
                        );
                    }
                    j += 1usize;
                }
                break;
            }
            i += 1usize;
        }
    }
    if let Some(ingress) = ingress {
        // Reject first.  stackless_wake_request re-checks `accepting` under this
        // same lock before pushing, so once this store is visible no producer
        // can enqueue again and the drain below is final rather than racy.
        ingress.accepting.store(false, rusty::sync::atomic::Ordering::Release);
        // Now drain what is already queued.  These are admitted wakes that will
        // never be delivered: count them as cancelled, and release the ticket
        // Arcs here so no allocation outlives the last waker Arc.  Leaving them
        // queued would also leave `enqueued=true` forever on tickets a foreign
        // waker still holds.
        let mut drained: u64 = 0u64;
        {
            let mut pending = ingress.pending.lock().unwrap();
            while !(*pending).is_empty() {
                let ticket = (*pending).pop_front().unwrap();
                ticket.enqueued.store(false, rusty::sync::atomic::Ordering::Release);
                drained += 1u64;
            }
        }
        if drained > 0u64 {
            g_stackless_cancel.pending_wakes.fetch_add(drained, rusty::sync::atomic::Ordering::Relaxed);
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), format!("[Reactor::teardown] cancelling {} admitted stackless wake(s) that will never be delivered", drained));
        }
    }
}

fn stackless_wake_unregister<WakeDomain>(reactor: &Reactor) {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    let owners_ptr = stackless_wake_owners_existing_ptr::<WakeDomain>();
    if owners_ptr.is_null() {
        return;
    }
    unsafe {
        {
            let owners = &mut *owners_ptr;
            let mut i: usize = 0usize;
            while i < owners.len() {
                if owners[i].reactor_key == key {
                    owners[i].bindings.clear();
                    owners[i].ingress = None;
                    owners[i].reactor_key = STACKLESS_UNREGISTERED_SLOT;
                    break;
                }
                i += 1usize;
            }
        }
    }
    stackless_wake_release_empty_storage::<WakeDomain>(owners_ptr);
}

#[cfg_attr(any(), thread_local)]
pub static mut reactor_clients_th_: rusty::HashMap<String, Vec<PollableProxy>> = rusty::HashMap::<String, Vec<PollableProxy>>::new();

#[cfg_attr(any(), thread_local)]
pub static mut reactor_prune_hwm_th_: usize = 64usize;

#[repr(C)]
pub struct Reactor {
    pub server_id_: Cell<i32>,
    pub all_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub waiting_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub timeout_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub composite_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub fibers_: RefCell<BTreeMap<usize, Rc<Fiber>>>,
    pub available_fibers_: RefCell<Vec<Rc<Fiber>>>,
    pub looping_: Cell<bool>,
    pub slow_: Cell<bool>,
    pub slow_count_: Cell<i32>,
    pub trying_count_: Cell<i32>,
    pub thread_id_: Cell<rusty::thread::ThreadId>,
    pub n_created_fibers_: Cell<i64>,
    pub n_busy_fibers_: Cell<i64>,
    pub n_active_fibers_: Cell<i64>,
    pub n_active_fibers_2_: Cell<i64>,
    pub n_idle_fibers_: Cell<i64>,
    pub stackless_tasks_: RefCell<Vec<StacklessTaskEntry>>,
    pub free_stackless_task_slots_: RefCell<Vec<usize>>,
    pub ready_stackless_tasks_: RefCell<VecDeque<usize>>,
    pub _pin: rusty::marker::PhantomPinned,
}

impl Reactor {
    pub fn new() -> Reactor {
        Reactor {
            server_id_: Default::default(),
            all_events_: Default::default(),
            waiting_events_: Default::default(),
            timeout_events_: Default::default(),
            composite_events_: Default::default(),
            fibers_: RefCell::new(BTreeMap::<usize, Rc<Fiber>>::new()),
            available_fibers_: Default::default(),
            looping_: Default::default(),
            slow_: Default::default(),
            slow_count_: Default::default(),
            trying_count_: Default::default(),
            // A directly constructed Reactor is thread-affine too.  Seed the
            // owner here so Drop and the private wake registry remain valid
            // outside the TLS factories; those factories may set the same id
            // again without changing the historical layout or signature.
            thread_id_: Cell::new(rusty::thread::current_id()),
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

    pub fn get_reactor() -> Rc<Reactor> {
        reactor_tls_get()
    }
    pub fn get_disk_reactor() -> Rc<Reactor> {
        reactor_tls_get_disk()
    }
    pub fn save_running_fiber(&self) -> Option<Rc<Fiber>> {
        reactor_tls_save_running()
    }
    pub fn restore_running_fiber(&self, old_fiber: Option<Rc<Fiber>>) {
        reactor_tls_restore_running(old_fiber);
    }
    pub fn set_running_fiber(&self, fiber: &Rc<Fiber>) {
        reactor_tls_set_running(fiber);
    }
    pub fn run_loop(&self, infinite: bool, do_check_timeout: bool) {
        reactor_verify(rusty::thread::current_id() == self.thread_id_.get());
        self.looping_.set(infinite);
        loop {
            let mut found_ready_events = true;
            while found_ready_events {
                found_ready_events = false;
                if self.process_stackless_tasks() {
                    found_ready_events = true;
                }
                let mut ready_events: VecDeque<Arc<dyn EventPollable>> = Default::default();
                {
                    let mut waiting_guard = self.waiting_events_.borrow_mut();
                    let mut i: usize = 0usize;
                    while i < waiting_guard.len() {
                        let ev = (*waiting_guard)[i].clone();
                        (*ev).test();
                        i += 1usize;
                    }
                    let n_before = ready_events.len();
                    move_matching(&mut waiting_guard, &mut ready_events, move |ev: &Arc<dyn EventPollable>| -> bool {
                        (*ev).status() == EventStatus::READY
                    });
                    if ready_events.len() > n_before {
                        found_ready_events = true;
                    }
                    waiting_guard.retain(move |ev: &Arc<dyn EventPollable>| -> bool {
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
                    move_matching(&mut composite_guard, &mut ready_events, move |ev: &Arc<dyn EventPollable>| -> bool {
                        (*ev).status() == EventStatus::READY
                    });
                    if ready_events.len() > n_before {
                        found_ready_events = true;
                    }
                    composite_guard.retain(move |ev: &Arc<dyn EventPollable>| -> bool {
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
                            if let Some(fiber) = option_fiber {
                                // Block-expression bind: the registry lookup IS
                                // the initial value, so there is no dead `false`
                                // to discard, and the borrow guard still dies at
                                // the closing brace — before continue_fiber can
                                // re-enter and borrow `fibers_` again.
                                let known = {
                                    let fibers_guard = self.fibers_.borrow();
                                    (*fibers_guard).contains_key(&fiber_registry_key(&fiber))
                                };
                                if known {
                                    reactor_verify(fiber.status_.get() == FiberStatus::PAUSED);
                                    if (*ev).status() == EventStatus::READY {
                                        (*ev).set_status(EventStatus::DONE);
                                    } else {
                                        reactor_verify((*ev).status() == EventStatus::TIMEOUT);
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

    pub fn prune_finished_events(&self) {
        let mut guard = self.all_events_.borrow_mut();
        if guard.len() < unsafe { reactor_prune_hwm_th_ } {
            return;
        }
        guard.retain(move |e: &Arc<dyn EventPollable>| -> bool {
            Arc::strong_count(e) > 1usize || !(*e).prunable()
        });
        unsafe { reactor_prune_hwm_th_ = guard.len() * 2usize + 64usize };
    }
    pub fn create_run_fiber(&self, func: rusty::Function<dyn FnMut()>) -> Rc<Fiber> {
        reactor_create_run_fiber_impl(self, func)
    }
    pub fn continue_fiber(&self, fiber: &Rc<Fiber>) {
        // Save current running fiber for nesting support.
        let mut old_fiber: Option<Rc<Fiber>> = None;
        {
            let guard = unsafe { sp_running_fiber_th_.borrow() };
            if (*guard).is_some() {
                old_fiber = Some((*guard).as_ref().unwrap().clone());
            }
        }
        {
            let mut guard = unsafe { sp_running_fiber_th_.borrow_mut() };
            *guard = Some(fiber.clone());
        }
        {
            let guard = unsafe { sp_running_fiber_th_.borrow() };
            let running: &Rc<Fiber> = (*guard).as_ref().unwrap();
            reactor_verify(!running.finished());
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
            let guard = unsafe { sp_running_fiber_th_.borrow() };
            let running: &Rc<Fiber> = (*guard).as_ref().unwrap();
            if running.finished() {
                let mut fiber_ref = running.clone();
                self.recycle(&mut fiber_ref);
            }
        }
        {
            let mut guard = unsafe { sp_running_fiber_th_.borrow_mut() };
            *guard = old_fiber;
        }
    }

    pub fn display_waiting_ev(&self) {
        reactor_log_line(Log::INFO, 0i32, core::ptr::null(), format!("waiting_events_: {}, composite_events_: {}",
                 self.waiting_events_.borrow().len(), self.composite_events_.borrow().len()));
    }

    pub fn register_fiber(&self, fiber: &Rc<Fiber>) {
        let mut guard = self.fibers_.borrow_mut();
        let inserted = guard.insert(fiber_registry_key(fiber), fiber.clone()).is_none();
        if !inserted {
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), "[DEBUG] RegisterFiber: Failed to insert fiber into fibers_ registry!".to_string());
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), format!("[DEBUG] fibers_ size: {}, REUSING_FIBER: {}", guard.len(), reusing_fiber()));
        }
        reactor_verify(inserted);
        reactor_verify(!guard.is_empty());
    }

    pub fn recycle(&self, fiber: &mut Rc<Fiber>) {
        // Fixes fibers not being recycled when they don't finish immediately.
        if reusing_fiber() {
            fiber.status_.set(FiberStatus::RECYCLED);
            let empty_fn: rusty::Function<dyn FnMut()> = Default::default();
            *fiber.func_.borrow_mut() = empty_fn;
            self.n_idle_fibers_.set(self.n_idle_fibers_.get() + 1i64);
            self.available_fibers_.borrow_mut().push(fiber.clone());
        }
        self.n_busy_fibers_.set(self.n_busy_fibers_.get() - 1i64);
        self.fibers_.borrow_mut().remove(&fiber_registry_key(fiber));
    }

    pub fn enqueue_stackless_task(&self, idx: usize) {
        reactor_verify(rusty::thread::current_id() == self.thread_id_.get());
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

    pub fn register_stackless_poller(&self, poller: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool>) -> usize {
        let ingress = stackless_wake_ingress::<()>(self);
        if !ingress.accepting.load(rusty::sync::atomic::Ordering::Acquire) {
            // Reactor teardown has started.  Destroy the rejected Task-bearing
            // closure without publishing a slot or a Context binding.  Dropping
            // it here destroys the completion callback and its captures on the
            // owner thread, which is what releases a cancellation-safe waiter.
            drop(poller);
            // Refusing a spawn is a cancellation, so it is reported, never
            // silent: the caller believes it has scheduled work that will now
            // never run, and anything waiting on that work must be told.
            g_stackless_cancel.rejected_spawns.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), "[Reactor::register_stackless_poller] cancelling a spawn refused during teardown; the task and its completion callback are destroyed now, so waiters are released with an error instead of blocking forever".to_string());
            return STACKLESS_UNREGISTERED_SLOT;
        }
        let scanned: usize = 0usize;
        let mut idx: usize = STACKLESS_UNREGISTERED_SLOT;
        {
            let mut free_guard = self.free_stackless_task_slots_.borrow_mut();
            if !free_guard.is_empty() {
                idx = *free_guard.last().unwrap();
                free_guard.pop();
                let tasks_guard = self.stackless_tasks_.borrow();
                if idx >= tasks_guard.len() {
                    idx = STACKLESS_UNREGISTERED_SLOT;
                }
            }
        }
        if idx == STACKLESS_UNREGISTERED_SLOT {
            let mut tasks_guard = self.stackless_tasks_.borrow_mut();
            tasks_guard.push(StacklessTaskEntry { active: true, queued: false, poll_once: poller });
            stackless_profile_note_register(scanned, false, tasks_guard.len());
            idx = tasks_guard.len() - 1usize;
        } else {
            let mut tasks_guard = self.stackless_tasks_.borrow_mut();
            (*tasks_guard)[idx].active = true;
            (*tasks_guard)[idx].queued = false;
            (*tasks_guard)[idx].poll_once = poller;
            stackless_profile_note_register(scanned, true, tasks_guard.len());
        }
        let binding = stackless_wake_make_binding::<()>(ingress);
        stackless_wake_attach::<()>(self, idx, binding);
        idx
    }

    // MEASURED allow, not a style waiver.  Clippy is right that `idx as usize`
    // is a no-op cast in Rust — `idx` is already `usize`.  It is not a no-op in
    // the emitter: with the cast the free-slot push lowers to
    // `free_guard->push(static_cast<size_t>(idx))`; without it the argument is
    // re-inferred as a collect and lowers to
    // `free_guard->push(std::move(rusty::Vec<size_t>::from_iter(std::move(idx))))`,
    // which fails to compile (3 errors, incl. "no viable conversion from
    // rusty::port::vec::Vec<unsigned long> to unsigned long" and a
    // `rusty::iter` static_assert).  The C++ ABI contract wins over the lint;
    // the cast is load-bearing and stays.  Scoped to this one item.
    #[allow(clippy::unnecessary_cast)]
    pub fn process_stackless_tasks(&self) -> bool {
        reactor_verify(rusty::thread::current_id() == self.thread_id_.get());
        let ingress_ready = stackless_wake_take_pending::<()>(self);
        for idx in ingress_ready {
            self.enqueue_stackless_task(idx);
        }
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
                        if (*tasks_guard)[idx].active && !(*tasks_guard)[idx].poll_once.is_empty() {
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
                    if ready {
                        // Close the ticket before publishing the slot for
                        // reuse. Then destroy the Task-bearing poll closure
                        // before releasing its stable Context/Waker binding.
                        stackless_wake_close::<()>(self, idx);
                        let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                        if idx < tasks_guard.len() {
                            stackless_profile_note_poll_ready();
                            (*tasks_guard)[idx].active = false;
                            (*tasks_guard)[idx].queued = false;
                            let empty_fn: rusty::Function<dyn FnMut(&mut rusty::Context) -> bool> = Default::default();
                            (*tasks_guard)[idx].poll_once = empty_fn;
                        }
                        drop(tasks_guard);
                        // Task/coroutine destruction may run arbitrary awaiter
                        // destructors that re-enter registration.  Do not
                        // publish this index for reuse until both the old Task
                        // and its retained Context/Waker binding are gone.
                        drop(poll_fn);
                        stackless_wake_detach::<()>(self, idx);
                        let mut free_guard = self.free_stackless_task_slots_.borrow_mut();
                        // `idx as usize` is a no-op cast in Rust (idx is already
                        // usize) but is load-bearing for the emitter: dropping it
                        // makes the argument lower as
                        // `rusty::Vec<size_t>::from_iter(std::move(idx))`, which
                        // does not compile.  See the item-level allow above.
                        free_guard.push(idx as usize);
                    } else {
                        let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                        if idx < tasks_guard.len() {
                            // Put the function back for the next poll.
                            (*tasks_guard)[idx].poll_once = poll_fn;
                        }
                    }
                }
            }
        }
        stackless_profile_report_periodic_shim();
        did_work
    }

    pub fn check_timeout(&self, ready_events: &mut VecDeque<Arc<dyn EventPollable>>) {
        let time_now: u64 = Time::now(true);
        let mut guard = self.timeout_events_.borrow_mut();
        // First pass: update the status of timed-out events. The Arc is
        // cloned per slot so no reference into the guard is held across
        // the status mutations.
        let mut i: usize = 0usize;
        while i < guard.len() {
            let event = (*guard)[i].clone();
            if (*event).status() == EventStatus::WAIT {
                let wakeup_time = (*event).wakeup_time();
                reactor_verify(wakeup_time > 0u64);
                if time_now >= wakeup_time {
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
        move_matching(&mut guard, ready_events, move |sp: &Arc<dyn EventPollable>| -> bool {
            let status = (*sp).status();
            status == EventStatus::READY || status == EventStatus::TIMEOUT
        });
        // Drop events that are DONE.
        guard.retain(move |sp: &Arc<dyn EventPollable>| -> bool {
            (*sp).status() != EventStatus::DONE
        });
    }
}

impl Drop for Reactor {
    fn drop(&mut self) {
        reactor_verify(rusty::thread::current_id() == self.thread_id_.get());
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), format!("[Reactor::~Reactor] Starting destruction, all_events_.len()={}, fibers_.size()={}",
                  self.all_events_.borrow().len(), self.fibers_.borrow().len()));
        // Reject new foreign wakes first. Destroy every Task-bearing closure
        // while its stable Context/Waker binding still exists, then retire the
        // private ingress. Reactor's public field layout remains unchanged.
        stackless_wake_shutdown_begin::<()>(self);
        // Count what teardown is about to cancel BEFORE the queues are cleared.
        // `ready_stackless_tasks_` holds completions that were already admitted
        // -- polled ready, or woken and queued -- and would have delivered their
        // callback on the next drain.  Discarding them silently is precisely the
        // "teardown begins between admission and wake" hang; they are cancelled
        // waiters and are reported as such.
        let admitted: u64 = self.ready_stackless_tasks_.borrow().len() as u64;
        self.ready_stackless_tasks_.borrow_mut().clear();
        self.free_stackless_task_slots_.borrow_mut().clear();
        let mut outstanding: u64 = 0u64;
        {
            let tasks_guard = self.stackless_tasks_.borrow();
            let mut i: usize = 0usize;
            while i < tasks_guard.len() {
                if (*tasks_guard)[i].active {
                    outstanding += 1u64;
                }
                i += 1usize;
            }
        }
        if outstanding > 0u64 || admitted > 0u64 {
            g_stackless_cancel.teardown_tasks.fetch_add(outstanding, rusty::sync::atomic::Ordering::Relaxed);
            g_stackless_cancel.admitted_completions.fetch_add(admitted, rusty::sync::atomic::Ordering::Relaxed);
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), format!("[Reactor::~Reactor] cancelling {} outstanding stackless task(s) and {} already-admitted completion(s); their callbacks and captures are destroyed below, which is how waiters learn this failed rather than hanging",
                      outstanding, admitted));
        }
        // Drop Task/coroutine frames after releasing the RefCell borrow:
        // cancellation destructors may re-enter registration.  Registration
        // now observes accepting=false and rejects without touching a slot.
        let retired_tasks = {
            let mut tasks_guard = self.stackless_tasks_.borrow_mut();
            core::mem::take(&mut *tasks_guard)
        };
        drop(retired_tasks);
        stackless_wake_unregister::<()>(self);
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[Reactor::~Reactor] Destructor body complete, about to destroy member variables".to_string());
    }
}

pub fn reactor_spawn_stackless_task_with_result<T: 'static, OnReady>(self_: &Reactor, mut task: rusty::Task<T>, mut on_ready: OnReady)
where
    OnReady: FnMut(T) + 'static,
{
    reactor_verify(rusty::thread::current_id() == self_.thread_id_.get());
    let ingress = stackless_wake_ingress::<()>(self_);
    let mut early_binding = stackless_wake_make_binding::<()>(ingress);
    let early_ticket = early_binding.ticket.clone();
    let ectx: &mut rusty::Context = stackless_wake_binding_context::<()>(&mut early_binding);
    let mut early_poll = task.poll(ectx);
    if early_poll.is_ready() {
        on_ready(early_poll.value);
        // Task retains Context*. Destroy it explicitly while the heap binding
        // is still alive; the binding is dropped on return afterwards.
        drop(task);
        return;
    }

    let ts = StacklessResultTaskState {
        early_binding,
        on_ready: RefCell::<Option<OnReady>>::new(Some(on_ready)),
        task: RefCell::<rusty::Task<T>>::new(task),
    };
    let state: Arc<StacklessResultTaskState<T, OnReady>> = Arc::new(ts);
    let completion_ticket = early_ticket.clone();
    let poller = StacklessPollFn::from_callable(move |ctx: &mut rusty::Context| -> bool {
        // Scoped so the task borrow is released before on_ready runs.
        let poll_result = state.task.borrow_mut().poll(ctx);
        if !poll_result.is_ready() {
            return false;
        }
        completion_ticket.slot.store(
            STACKLESS_UNREGISTERED_SLOT,
            rusty::sync::atomic::Ordering::Release,
        );
        // take() moves the callback out and leaves None, so it fires once.
        let cb: Option<OnReady> = {
            let mut cbguard = state.on_ready.borrow_mut();
            (*cbguard).take()
        };
        if let Some(mut f) = cb {
            f(poll_result.value);
        }
        true
    });
    let idx = self_.register_stackless_poller(poller);
    if idx == STACKLESS_UNREGISTERED_SLOT {
        // Teardown refused the registration.  register_stackless_poller has
        // already destroyed the poller -- and with it the Task, the completion
        // callback and its captures -- and recorded the cancellation.  Do not
        // pretend the spawn succeeded by publishing a slot or draining the
        // ingress; returning quietly here is what would leave the caller's
        // waiter blocked on a completion that can never arrive.
        return;
    }
    early_ticket.slot.store(idx, rusty::sync::atomic::Ordering::Release);
    let ingress_ready = stackless_wake_take_pending::<()>(self_);
    for ready_idx in ingress_ready {
        self_.enqueue_stackless_task(ready_idx);
    }
}

fn reactor_setup_sp_event<Ev: EventCore + 'static>(ev0: Arc<Ev>) -> Arc<Ev> {
    let mut ev = ev0;
    {
        let opt = Arc::get_mut(&mut ev);
        reactor_verify(opt.is_some());
        let m: &mut Ev = opt.unwrap();
        m.core_state_mut().__debug_creator = 1;
    }
    let base: Arc<dyn EventPollable> = ev.clone();
    let self_weak = Arc::downgrade(&base);
    unsafe {
        // The value has not yet been published; initialize its self weak-link
        // through the stable Arc allocation before entering the reactor queue.
        let raw = Arc::as_ptr(&ev) as *mut Ev;
        *(*raw).core_self_mut() = self_weak;
    }
    let reactor = Reactor::get_reactor();
    {
        let stored: Arc<dyn EventPollable> = ev.clone();
        let mut guard = reactor.all_events_.borrow_mut();
        (*guard).push_back(stored);
    }
    (*reactor).prune_finished_events();
    ev
}

// Per-type creation entry points (the event_make dispatcher's named
// branches, one honest factory each — the callsite-rewrite campaign
// migrates reactor_create_sp_event<Ev> sites onto these).
pub fn create_sp_int_event(target: i32) -> Arc<IntEvent> {
    reactor_setup_sp_event::<IntEvent>(int_event_make(target))
}

pub fn create_sp_timeout_event(wait_us: u64) -> Arc<TimeoutEvent> {
    reactor_setup_sp_event::<TimeoutEvent>(timeout_event_make(wait_us))
}

pub fn create_sp_never_event() -> Arc<NeverEvent> {
    reactor_setup_sp_event::<NeverEvent>(never_event_make())
}

pub fn create_sp_waitany(a: Arc<dyn EventPollable>, b: Arc<dyn EventPollable>) -> Arc<WaitAny> {
    reactor_setup_sp_event::<WaitAny>(waitany_make(a, b))
}

pub fn create_sp_waitall() -> Arc<WaitAll> {
    reactor_setup_sp_event::<WaitAll>(waitall_make())
}

pub fn create_sp_waitall_from(evs: &Vec<Arc<dyn EventPollable>>) -> Arc<WaitAll> {
    reactor_setup_sp_event::<WaitAll>(waitall_make_from(evs))
}

pub fn create_sp_box_event<T: Clone + Default + 'static>() -> Arc<BoxEvent<T>> {
    reactor_setup_sp_event::<BoxEvent<T>>(boxevent_make::<T>())
}

pub enum PollCommand {
    AddPollable { pollable: Box<dyn PollableBase> },
    RemovePollable { fd: i32 },
    ClosePollable { fd: i32 },
    UpdateMode { fd: i32, new_mode: i32 },
    AddJob { job: Arc<dyn Job> },
    RemoveJob { job: Arc<dyn Job> },
    Shutdown,
}

#[cfg_attr(any(), thread_local)]
pub static mut g_current_poll_worker: *mut PollThreadWorker = core::ptr::null_mut();

#[repr(C)]
pub struct PollThreadWorker {
    pub receiver_: PollCmdReceiver,
    pub poll_: Epoll,
    pub fd_to_pollable_: FdPollableMap,
    pub mode_: FdModeMap,
    pub pending_remove_: FdSet,
    pub jobs_: JobSet,
    pub stop_: bool,
}

impl PollThreadWorker {
    // Factory: worker wrapped in Rc<RefCell<>> for its thread.
    pub fn create(receiver: PollCmdReceiver) -> Rc<RefCell<PollThreadWorker>> {
        pollworker_create(receiver)
    }

    // Main polling loop — epoll events + channel commands.
    pub fn poll_loop(&mut self) {
        pollworker_poll_loop(self)
    }

    // Direct mode update (bypasses the channel; poll-thread only).
    pub fn update_mode(&mut self, poll: &mut dyn Pollable, new_mode: i32) {
        pollworker_update_mode(self, poll, new_mode)
    }
}

pub fn pollworker_is_on_poll_thread() -> bool {
    unsafe { !g_current_poll_worker.is_null() }
}

fn u64_to_thread_id(bits: u64) -> rusty::thread::ThreadId {
    // The production facade wraps the platform's opaque thread id. Preserve
    // the incumbent byte-level round trip without pretending that its native
    // type is an integer in generated C++.
    unsafe { core::mem::transmute::<u64, rusty::thread::ThreadId>(bits) }
}

#[repr(C)]
pub struct PollThread {
    pub sender_: rusty::sync::mpsc::Sender<PollCommand>,
    pub join_handle_: PollJoinSlot,
    // Thread id of the poll thread as raw u64 bits (bit_cast of the
    // native id) — used to detect self-join attempts in shutdown.
    pub poll_thread_id_bits_: AtomicU64,
    pub shutdown_called_: AtomicBool,
}

impl PollThread {
    // Factory: spawns the worker thread; returns the Arc handle.
    pub fn create() -> Arc<PollThread> {
        pollthread_create()
    }

    // Explicit shutdown: send CmdShutdown, join unless self-join.
    pub fn shutdown(&self) {
        let main_tid: i64 = current_thread_gettid();
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), format!("[PollThread::shutdown] Called from TID={}", main_tid as i32));
        if self.shutdown_called_.swap(true, rusty::sync::atomic::Ordering::AcqRel) {
            reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Already called, returning".to_string());
            return;
        }
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Sending CmdShutdown".to_string());
        // `Sender::send` fails ONLY when the receiver is gone (mpsc.hpp:87-96
        // returns Err(Disconnected) iff !receiver_alive_), i.e. the poll
        // worker has already exited. The join below is what actually ends the
        // thread, so a dropped Shutdown command is unobservable. Discarded
        // explicitly rather than silently: C++ never warned here, so the
        // incumbent's identical discard was invisible.
        let _dropped_when_worker_gone = self.sender_.send(PollCommand::Shutdown);
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] CmdShutdown sent".to_string());
        // Thread-safe read of the poll thread's id.
        let current_tid = rusty::thread::current_id();
        let poll_tid = u64_to_thread_id(
            self.poll_thread_id_bits_.load(rusty::sync::atomic::Ordering::Acquire));
        if current_tid == poll_tid {
            reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Called from poll thread, skipping join".to_string());
            return;
        }
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Acquiring join_handle lock...".to_string());
        // Scoped so the guard drops BEFORE the "Released" log below, as the
        // C++ block did.
        {
            let mut guard = self.join_handle_.lock().unwrap();
            reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] join_handle lock acquired".to_string());
            if (*guard).is_some() {
                reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Calling thread.join()...".to_string());
                (*guard).take().unwrap().join();
                reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] thread.join() completed!".to_string());
            } else {
                reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] join_handle is None, thread already joined".to_string());
            }
        }
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Released join_handle lock".to_string());
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] Complete".to_string());
    }

    pub fn add_proxy(&self, poll: PollableProxy) {
        // Err == the poll worker exited; there is no epoll set left to add to.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::AddPollable { pollable: poll });
    }

    pub fn remove(&self, poll: &mut dyn Pollable) {
        // Err == the poll worker exited; the fd it would unregister is gone too.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::RemovePollable { fd: poll.fd() });
    }

    // fd-keyed variant (remove only reads .fd() anyway); lets
    // shim-only callers avoid the Pollable base entirely.
    pub fn remove_fd(&self, fd: i32) {
        // Err == the poll worker exited; the fd it would unregister is gone too.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::RemovePollable { fd });
    }

    // Thread-safe close: removes from epoll, closes socket, drops
    // proxy ownership.
    pub fn request_close(&self, fd: i32) {
        // Err == the poll worker exited; it already closed everything it owned.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::ClosePollable { fd });
    }

    pub fn update_mode(&self, fd: i32, new_mode: i32) {
        let result = self.sender_.send(PollCommand::UpdateMode { fd, new_mode });
        if result.is_err() {
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), "PollThread::update_mode: send failed! Channel disconnected?".to_string());
        }
    }

    pub fn add(&self, job: Arc<dyn Job>) {
        // Err == the poll worker exited; there is no loop left to run the job.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::AddJob { job });
    }

    // For testing — worker state is not reachable across the channel.
    pub fn get_remove_count(&self) -> i32 {
        0
    }
}

impl Drop for PollThread {
    fn drop(&mut self) {
        pollthread_drop(self)
    }
}

// ---------------------------------------------------------------------------
// Namespace-placement contract for the Quorum family (H1 / compiler contract 1)
// ---------------------------------------------------------------------------
//
// The Quorum family is the one part of this module that does NOT live in the
// module-wide `srpc` namespace.  The incumbent ABI roots it directly in global
// `janus`: 46 strong entries plus QuorumEvent's RTTI/vtable identity, all of
// them still attached to module `srpc.reactor`
// (`janus::QuorumEvent@srpc.reactor::...`).  `srpc::QuorumEvent` and
// `srpc::janus::QuorumEvent` mangle differently and are NOT substitutes; nor is
// a namespace alias or a type alias.
//
// The contract is carried by an inert `#[cfg_attr(any(), cpp_namespace(::janus))]`
// marker on each item that introduces a C++ namespace-scope entity: the three
// types and the five free functions below.  Rules:
//
//   * The target is spelled ABSOLUTELY.  A leading `::` is semantic and means
//     module-global placement, never nesting under the configured
//     cxx-namespace.  A relative target would be ambiguous about exactly the
//     distinction this contract exists to make.
//   * Members follow their enclosing type, so `impl` blocks are deliberately
//     NOT marked.  Marking them as well would be a redundant overlapping
//     placement contract, which the compiler is required to reject atomically.
//   * Type aliases (`QuorumDanglingVec`, `QuorumFinalizeFn`) are NOT marked:
//     they resolve away in the mangling and carry no namespace identity.
//
// rustc never sees the attribute -- `any()` is unconditionally false -- so the
// Cargo lane is bit-for-bit unaffected by these markers.

#[cfg_attr(any(), cpp_namespace(::janus))]
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
#[repr(i32)]
pub enum QuorumPolicy {
    DEFAULT = 0,
    ALL_NO = 1,
    LEADER_AND = 2,
    COMMITTED_SHORT = 3,
    ALWAYS_READY = 4,
}

#[cfg_attr(any(), cpp_namespace(::janus))]
#[repr(C)]
pub struct QuorumEvent {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: rusty::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub n_voted_yes_: Cell<i32>,
    pub n_voted_no_: Cell<i32>,
    pub xids_: RefCell<HashMap<u16, i64>>,
    pub n_total_: i32,
    pub quorum_: i32,
    pub policy_: Cell<QuorumPolicy>,
    pub committed_seen_: Cell<bool>,
    pub num_leader_: Cell<i32>,
    pub n_leader_yes_: Cell<i32>,
    pub n_leader_no_: Cell<i32>,
    pub highest_term_: Cell<i64>,
    pub timeouted_: Cell<bool>,
    pub leader_id_: Cell<u32>,
    pub par_id_: Cell<i64>,
    pub id_: Cell<u64>,
    pub finalize_event_: Arc<IntEvent>,
}

impl QuorumEvent {
    pub fn add_xid(&self, site: u16, xid: i64) {
        self.xids_.borrow_mut().insert(site, xid);
    }
    pub fn remove_xid(&self, site: u16) {
        self.xids_.borrow_mut().remove(&site);
    }
    pub fn finalize(&self, timeout: u64, finalize_func: QuorumFinalizeFn) {
        quorum_event_finalize(self, timeout, finalize_func)
    }
    pub fn yes(&self) -> bool {
        let base = self.n_voted_yes_.get() >= self.quorum_;
        if self.policy_.get() == QuorumPolicy::LEADER_AND {
            return base && self.n_leader_yes_.get() >= self.num_leader_.get();
        }
        base
    }
    pub fn no(&self) -> bool {
        if self.policy_.get() == QuorumPolicy::ALL_NO {
            return self.n_voted_no_.get() == self.n_total_;
        }
        reactor_verify(self.n_total_ >= self.quorum_);
        let base = self.n_voted_no_.get() > (self.n_total_ - self.quorum_);
        if self.policy_.get() == QuorumPolicy::LEADER_AND {
            return base || self.n_leader_no_.get() > 0;
        }
        base
    }
    pub fn vote_yes(&self) {
        self.n_voted_yes_.set(self.n_voted_yes_.get() + 1);
        event_test_impl(self);
        let fe = self.finalize_event_.clone();
        if fe.status_.get() != EventStatus::TIMEOUT && fe.status_.get() != EventStatus::DONE {
            (*fe).set(self.n_voted_yes_.get() + self.n_voted_no_.get());
        }
    }
    pub fn vote_no(&self) {
        self.n_voted_no_.set(self.n_voted_no_.get() + 1);
        event_test_impl(self);
        let fe = self.finalize_event_.clone();
        if fe.status_.get() != EventStatus::TIMEOUT && fe.status_.get() != EventStatus::DONE {
            (*fe).set(self.n_voted_yes_.get() + self.n_voted_no_.get());
        }
    }
    pub fn is_composite_event(&self) -> bool {
        true
    }
    pub fn wait(&self) {
        event_wait_impl(self, 0u64)
    }
    pub fn wait_timeout(&self, timeout: u64) {
        event_wait_impl(self, timeout)
    }
    pub fn get_fiber_id(&self) -> u64 {
        event_core_get_fiber_id()
    }
    pub fn is_slow(&self) -> bool {
        quorum_event_is_slow(self)
    }
    pub fn get_self(&self) -> Option<Arc<dyn EventPollable>> {
        self.self_.upgrade()
    }
    pub fn set_self(&mut self, self_ptr: Weak<dyn EventPollable>) {
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
    fn upgrade_fiber(&self) -> Option<Rc<Fiber>> {
        event_core_upgrade_fiber(self)
    }
}

impl EventCore for QuorumEvent {
    fn core_status(&self) -> &Cell<EventStatus> { &self.status_ }
    fn core_owner_thread(&self) -> rusty::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { true }
}

#[cfg_attr(any(), cpp_namespace(::janus))]
#[repr(C)]
pub struct QuorumEventWrapper {
    pub q_: Arc<QuorumEvent>,
}

impl QuorumEventWrapper {
    pub fn new(n_total: i32, quorum: i32) -> QuorumEventWrapper {
        QuorumEventWrapper { q_: create_sp_quorum_event(n_total, quorum) }
    }
    // MEASURED allow.  Rust auto-derefs `&self.q_` (an `&Arc<QuorumEvent>`) to
    // `&QuorumEvent` here, but the emitter does not: it lowers the accessor
    // body verbatim to `return this->q_;` and the module stops compiling —
    // "no viable conversion from returned value of type
    // 'const rusty::Arc<QuorumEvent>' to function return type
    // 'const QuorumEvent'" (R/M/obj-D1.log).  The explicit `&(*self.q_)`
    // lowers to `return *this->q_;`, which is the historical accessor.
    // The C++ ABI contract wins; scoped to this one item.
    #[allow(clippy::explicit_auto_deref)]
    pub fn q(&self) -> &QuorumEvent {
        &(*self.q_)
    }
    pub fn wait(&self) {
        (*self.q_).wait()
    }
    pub fn wait_timeout(&self, timeout: u64) {
        (*self.q_).wait_timeout(timeout)
    }
    pub fn log(&self) {
        (*self.q_).log()
    }
    pub fn get_fiber_id(&self) -> u64 {
        (*self.q_).get_fiber_id()
    }
    pub fn vote_yes(&self) {
        (*self.q_).vote_yes()
    }
    pub fn vote_no(&self) {
        (*self.q_).vote_no()
    }
    pub fn yes(&self) -> bool {
        (*self.q_).yes()
    }
    pub fn no(&self) -> bool {
        (*self.q_).no()
    }
    pub fn is_ready(&self) -> bool {
        (*self.q_).is_ready()
    }
    pub fn is_slow(&self) -> bool {
        (*self.q_).is_slow()
    }
    pub fn test(&self) -> bool {
        (*self.q_).test()
    }
    pub fn add_xid(&self, site: u16, xid: i64) {
        (*self.q_).add_xid(site, xid)
    }
    pub fn remove_xid(&self, site: u16) {
        (*self.q_).remove_xid(site)
    }
    pub fn finalize(&self, timeout: u64, f: QuorumFinalizeFn) {
        (*self.q_).finalize(timeout, f)
    }
}

fn event_wait_impl<W: EventCore>(ev: &W, timeout: u64) {
    reactor_verify(unsafe { sp_reactor_th_.is_some() });
    // `.clone()` binds a *value* Rc (not a reference).  The field access is
    // spelled without an explicit `(*…)`: Rust auto-derefs the Rc, and the
    // emitter lowers the bare receiver to a direct `(*reactor_th).thread_id_`,
    // so it reaches through the Rc either way.  (Spelling the deref in Rust
    // instead lowers to the generic `deref_if_pointer_like(reactor_th)` —
    // equivalent, and what this file used to emit.)
    let reactor_th = unsafe { sp_reactor_th_.as_ref().unwrap().clone() };
    reactor_verify(reactor_th.thread_id_.get() == rusty::thread::current_id());
    if ev.core_status().get() == EventStatus::DONE {
        return; // second use of the event
    }
    if ev.is_ready() {
        ev.core_status().set(EventStatus::DONE); // no need to wait
    } else {
        // The event may be created in a different fiber; for now only one
        // fiber can wait on an event. Capture the running fiber to wake later.
        let fiber_opt = Fiber::current_fiber();
        reactor_verify(fiber_opt.is_some()); // can't wait outside a fiber
        let fiber = fiber_opt.unwrap();

        let reactor_rc = Reactor::get_reactor();
        // Inline `borrow_mut().push_back(…)`: the RefMut temporary releases at
        // the end of each statement — before the yield below — so the reactor
        // loop can re-borrow these queues while this fiber sleeps.  With the
        // receiver spelled bare these lower to
        // `(*reactor_rc).waiting_events_.borrow_mut()->push_back(…)` — the
        // guard is still a per-statement temporary.
        reactor_rc.waiting_events_.borrow_mut().push_back(ev.core_self().upgrade().unwrap());

        // Composite events (WaitAll/WaitAny/Quorum) need periodic polling; add
        // them to a smaller scanned queue. Regular RPC events self-notify.
        if ev.core_is_composite() {
            reactor_rc.composite_events_.borrow_mut().push_back(ev.core_self().upgrade().unwrap());
        }

        if timeout > 0 {
            let now = Time::now(true);
            ev.core_state().wakeup_time_.set(now + timeout);
            reactor_rc.timeout_events_.borrow_mut().push_back(ev.core_self().upgrade().unwrap());
        }

        // Transpiled Weak has no implicit Rc→Weak conversion; use the static
        // Rc::downgrade(rc) factory (mirrors std::rc::Rc::downgrade). `fiber` is
        // cloned (a refcount bump) so the factory consumes the temporary and the
        // original `fiber` stays live for the checks below.
        *ev.core_state().wp_fiber_.borrow_mut() = ::rusty::port::rc::Rc::<Fiber>::downgrade(&fiber);
        ev.core_status().set(EventStatus::WAIT);
        let fiber_status = fiber.status_.get();
        reactor_verify(fiber_status != FiberStatus::FINISHED && fiber_status != FiberStatus::RECYCLED);
        (*fiber).yield_();
    }
}

fn event_test_impl<W: EventCore>(ev: &W) -> bool {
    reactor_verify(ev.core_state().__debug_creator != 0);
    if ev.is_ready() {
        if ev.core_status().get() == EventStatus::INIT {
            ev.core_status().set(EventStatus::DONE);
        } else if ev.core_status().get() == EventStatus::WAIT {
            if rusty::thread::current_id() == ev.core_owner_thread() {
                // Owner-thread-only: upgrading the weak fiber ref mutates a plain
                // (non-atomic) Rc strong count; doing this from a foreign thread
                // races the owner's own Rc<Fiber> clones and corrupts the count.
                // The upgraded handle is used only for this liveness assertion.
                let option_fiber = ev.core_state().wp_fiber_.borrow().upgrade();
                reactor_verify(option_fiber.is_some());
                reactor_verify(ev.core_status().get() != EventStatus::DEBUG);
            }
            ev.core_status().set(EventStatus::READY);
        } else if ev.core_status().get() == EventStatus::READY {
            reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "event status ready, triggered?".to_string());
        } else if ev.core_status().get() == EventStatus::DONE
            || ev.core_status().get() == EventStatus::TIMEOUT
        {
            // do nothing
        } else {
            reactor_verify(false);
        }
        return true;
    } else if ev.core_status().get() == EventStatus::DONE {
        ev.core_status().set(EventStatus::INIT);
    }
    false
}

fn event_core_get_fiber_id() -> u64 {
    let fiber_opt = Fiber::current_fiber();
    reactor_verify(fiber_opt.is_some());
    fiber_opt.unwrap().id.get()
}

fn event_state_seed(st: &EventState) {
    {
        let mut g = st.wait_place_.borrow_mut();
        *g = "not recorded".to_string();
    }
    let fiber_opt = Fiber::current_fiber();
    if let Some(rc_fiber) = fiber_opt {
        // Load-bearing re-annotation: without it the if-let payload lowers to
        // `decltype(auto)` and the static `Rc::<Fiber>::downgrade(&rc_fiber)`
        // call re-resolves as a UFCS member call `rc_fiber.downgrade()`, which
        // does not compile (`this_` unbound).
        let rc_fiber: Rc<Fiber> = rc_fiber;
        let mut g2 = st.wp_fiber_.borrow_mut();
        *g2 = rusty::port::rc::Rc::<Fiber>::downgrade(&rc_fiber);
    }
}

// MEASURED allow (clippy::arc_with_non_send_sync).  Clippy is right that these
// event types are not Send+Sync — they hold `Cell`/`RefCell`/`Rc` and are
// owner-thread-only by design — and that Rust would prefer `Rc`.  `Rc` is not
// available to us: `Arc<T>` IS the reactor's historical handle type on the
// wire, and swapping it moves the ABI.
//
// The experiment (R/M/gen-ARCPROBE, R/M/obj-ARCPROBE.log): flipping just
// `never_event_make` to `Rc` changes the exported declaration from
//     rusty::Arc<NeverEvent> never_event_make();
// to
//     rusty::Rc<NeverEvent> never_event_make();
// and the module then fails to compile at the very first consumer,
// `reactor_setup_sp_event<NeverEvent>` ("no known conversion from
// 'rusty::Rc<NeverEvent>' to 'rusty::Arc<NeverEvent>'").  Eighteen entries of
// the frozen incumbent symbol oracle carry `rusty::Arc` in their mangled
// signature, and `Reactor`'s own constructor embeds four
// `rusty::RefCell<rusty::VecDeque<rusty::Arc<srpc::EventPollable>>>` fields, so
// the layout oracle moves too.  The C++ ABI contract wins.
//
// Each of the eight factories carries the allow on its own item; this note is
// the shared reason.
#[allow(clippy::arc_with_non_send_sync)]
fn never_event_make() -> Arc<NeverEvent> {
    let sp = Arc::new(NeverEvent {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<NeverEvent>::new(),
    });
    event_state_seed(&sp.state_);
    sp
}

// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
fn timeout_event_make(wait_us: u64) -> Arc<TimeoutEvent> {
    let sp = Arc::new(TimeoutEvent {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<TimeoutEvent>::new(),
        wakeup_time_: Time::now(true) + wait_us,
        wait_us_: wait_us,
    });
    event_state_seed(&sp.state_);
    sp
}

// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
fn int_event_make(target: i32) -> Arc<IntEvent> {
    let sp = Arc::new(IntEvent {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<IntEvent>::new(),
        value_: Cell::new(0),
        target_: Cell::new(target),
    });
    event_state_seed(&sp.state_);
    sp
}

// MEASURED allow.  `vec![a, b]` is rejected by the transpiler outright, before
// any output is written: "unexpanded macro invocation `vec` is not supported in
// a file containing cpp_name because it can synthesize hidden calls, items, or
// types" (R/M/gen-B2.log).  The explicit new()+push()+push() is the supported
// spelling.  Scoped to this item.
#[allow(clippy::vec_init_then_push)]
// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
fn waitany_make(a: Arc<dyn EventPollable>, b: Arc<dyn EventPollable>) -> Arc<WaitAny> {
    let mut events: Vec<Arc<dyn EventPollable>> =
        Vec::<Arc<dyn EventPollable>>::new();
    events.push(a);
    events.push(b);
    let sp = Arc::new(WaitAny {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<WaitAny>::new(),
        events_: events,
    });
    event_state_seed(&sp.state_);
    sp
}

// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
fn waitall_make() -> Arc<WaitAll> {
    let sp = Arc::new(WaitAll {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<WaitAll>::new(),
        events_: RefCell::new(Vec::new()),
    });
    event_state_seed(&sp.state_);
    sp
}

// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
fn waitall_make_from(evs: &Vec<Arc<dyn EventPollable>>) -> Arc<WaitAll> {
    let mut events: Vec<Arc<dyn EventPollable>> =
        Vec::<Arc<dyn EventPollable>>::with_capacity(evs.len());
    for ev in evs {
        events.push(ev.clone());
    }
    let sp = Arc::new(WaitAll {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<WaitAll>::new(),
        events_: RefCell::new(events),
    });
    event_state_seed(&sp.state_);
    sp
}

fn shared_int_event_set(sie: &mut SharedIntEvent, v: i32) -> i32 {
    let ret: i32 = sie.value_;
    sie.value_ = v;
    let mut i: usize = 0usize;
    while i < sie.events_.len() {
        let ev: &Arc<IntEvent> = &sie.events_[i];
        if ev.status_.get() <= EventStatus::WAIT && ev.target_.get() <= v {
            (*ev).set(v);
        }
        i += 1usize;
    }
    ret
}

fn int_event_raw_ptr(ev: &Arc<IntEvent>) -> *const IntEvent {
    let p: *const IntEvent = Arc::as_ptr(ev);
    p
}

fn shared_int_event_wait_until_gte(sie: &mut SharedIntEvent, x: i32, timeout: i32) -> bool {
    if sie.value_ >= x {
        return false;
    }
    let ev: Arc<IntEvent> = create_sp_int_event(1);
    ev.value_.set(sie.value_);
    ev.target_.set(x);
    sie.events_.push(ev.clone());
    (*ev).wait_timeout(timeout as u64);
    // Remove the event from the waiter list once it reaches a terminal
    // state (READY or TIMEOUT).
    let if_timeout: bool = ev.status_.get() == EventStatus::TIMEOUT;
    let ev_ptr: *const IntEvent = int_event_raw_ptr(&ev);
    sie.events_.retain(move |item: &Arc<IntEvent>| {
        int_event_raw_ptr(item) != ev_ptr
    });
    if_timeout
}

fn shared_int_event_wait(sie: &mut SharedIntEvent, f: EventTestFn) {
    if f(sie.value_) {
        return;
    }
    let ev: Arc<IntEvent> = create_sp_int_event(1);
    ev.value_.set(sie.value_);
    {
        let mut guard = ev.state_.test_.borrow_mut();
        *guard = f;
    }
    sie.events_.push(ev.clone());
    (*ev).wait();
}

fn fiber_fn_present(f: *const RefCell<FiberFn>) -> bool {
    let g = unsafe { (*f).borrow() };
    !(*g).is_empty()
}

fn fiber_fn_invoke(f: *const RefCell<FiberFn>) {
    // borrow_mut: rusty::Function::operator() is non-const.
    let mut g = unsafe { (*f).borrow_mut() };
    (*g)();
}

fn fiber_fn_clear(f: *const RefCell<FiberFn>) {
    let mut g = unsafe { (*f).borrow_mut() };
    let mut empty: FiberFn = Default::default();
    *g = empty;
}

fn fiber_install_task(t: *const RefCell<Option<Box<fiber_task_t>>>,
                      task: FiberTaskFn) {
    // Box first to pin the address, then start the engine (which RUNS the body
    // up to its first yield), then wrap/store. The RefCell borrow must remain
    // last so user fiber code never runs while that borrow is held.
    let mut boxed = Box::new(fiber_task_t::new(task));
    let task_ref: &mut fiber_task_t = boxed.as_mut();

    // Establish the self-pointer only after Box has fixed task's address.
    let yield_value: fiber_yield_t = fiber_yield_t::new(task_ref);
    task_ref.yield_ = yield_value;

    // Bind the entry argument before borrowing fib_: this keeps the two raw
    // pointers' evaluation and lifetimes unambiguous in both Rust and C++.
    let task_ptr: *mut fiber_task_t = task_ref as *mut fiber_task_t;
    let entry_arg: *mut core::ffi::c_void =
        task_ptr as *mut core::ffi::c_void;
    fiber_engine_start(&mut task_ref.fib_, entry_arg);

    let mut installed = Some(boxed);
    let mut g = unsafe { (*t).borrow_mut() };
    *g = installed;
}

fn fiber_task_invoke(t: *const RefCell<Option<Box<fiber_task_t>>>) {
    let mut g = unsafe { (*t).borrow_mut() };
    let bx: &mut Box<fiber_task_t> = (*g).as_mut().unwrap();
    fiber_engine_resume(&mut bx.fib_);
}

fn fiber_yield_invoke_ptr(y: *mut fiber_yield_t) {
    unsafe { fiber_yield_invoke(&mut *y) };
}

fn reactor_live_fiber_count() -> usize {
    let reactor = Reactor::get_reactor();
    let guard = reactor.fibers_.borrow();
    (*guard).len()
}

fn reactor_dec_active_fibers() {
    let reactor = Reactor::get_reactor();
    reactor.n_active_fibers_.set(reactor.n_active_fibers_.get() - 1i64);
}

fn fiber_run_wrapper(fb: &Fiber, y: *mut fiber_yield_t) {
    fb.fiber_yield_.set(y);
    reactor_verify(fiber_fn_present(&fb.func_));
    loop {
        let sz = reactor_live_fiber_count();
        reactor_verify(sz > 0usize);
        reactor_verify(fiber_fn_present(&fb.func_));
        fiber_fn_invoke(&fb.func_);
        fiber_fn_clear(&fb.func_);
        fb.status_.set(FiberStatus::FINISHED);
        if fb.needs_finalize_.get() {
            reactor_log_line(Log::INFO, 0i32, core::ptr::null(), "Warning: We did not deal with backlog issues".to_string());
            fb.needs_finalize_.set(false);
        }
        reactor_dec_active_fibers();
        fiber_yield_invoke_ptr(y);
    }
}

fn fiber_run(fb: &Fiber) {
    {
        let tguard = fb.fiber_task_.borrow();
        reactor_verify((*tguard).is_none());
    }
    reactor_verify(fb.status_.get() == FiberStatus::INIT);
    fb.status_.set(FiberStatus::STARTED);
    let sz = reactor_live_fiber_count();
    reactor_verify(sz > 0usize);
    // The closure only reads through this pointer; keep the constness instead
    // of manufacturing a mutable pointer with a const-removal kernel.
    let self_ptr: *const Fiber = fb as *const Fiber;
    let mut task: FiberTaskFn = FiberTaskFn::from_callable(move |yy: &mut fiber_yield_t| {
        unsafe {
            // The initial callback must run before fiber_install_task stores
            // its Box. This also proves no RefCell borrow spans engine start.
            {
                let tguard = (*self_ptr).fiber_task_.borrow();
                reactor_verify((*tguard).is_none());
            }
            fiber_run_wrapper(&*self_ptr, &raw mut *yy);
        }
    });
    fiber_install_task(&fb.fiber_task_, task);
    {
        let tguard = fb.fiber_task_.borrow();
        reactor_verify((*tguard).is_some());
    }
}

fn fiber_do_yield(fb: &Fiber) {
    let y: *mut fiber_yield_t = fb.fiber_yield_.get();
    reactor_verify(!y.is_null());
    let s = fb.status_.get();
    reactor_verify(s == FiberStatus::STARTED || s == FiberStatus::RESUMED
        || s == FiberStatus::FINALIZING);
    fb.status_.set(FiberStatus::PAUSED);
    reactor_dec_active_fibers();
    fiber_yield_invoke_ptr(y);
}

fn fiber_do_continue(fb: &Fiber) {
    let s = fb.status_.get();
    reactor_verify(s == FiberStatus::PAUSED || s == FiberStatus::RECYCLED);
    {
        let tguard = fb.fiber_task_.borrow();
        reactor_verify((*tguard).is_some());
    }
    fb.status_.set(FiberStatus::RESUMED);
    fiber_task_invoke(&fb.fiber_task_);
    // some events might have been triggered from last fiber,
    // but you have to manually call the scheduler to loop.
}

fn fiber_is_finished(fb: &Fiber) -> bool {
    let s = fb.status_.get();
    s == FiberStatus::FINISHED || s == FiberStatus::RECYCLED
}

fn fiber_do_finalize(fb: &Fiber) {
    fb.needs_finalize_.set(false);
}

// MEASURED allow.  Clippy's `c"MAKO_ASYNC_PROFILE"` is the better Rust, but the
// emitter has no lowering for C-string literals: it writes
// `rusty::as_ptr((/* TODO: literal */))` and the module stops compiling
// ("expected expression", R/M/obj-B2b.log).  The byte-string form lowers to a
// real `std::array<uint8_t, 19>{{ 0x4d, ... , 0x00 }}`.  Scoped to this item.
#[allow(clippy::manual_c_str_literals)]
fn stackless_profile_env() -> bool {
    let env: *const LegacyCChar = unsafe {
        getenv(b"MAKO_ASYNC_PROFILE\0".as_ptr() as *const LegacyCChar)
    };
    if env.is_null() {
        return false;
    }
    unsafe { *env != 0 as LegacyCChar && *env != 48 as LegacyCChar }
}

fn stackless_profile_enabled() -> bool {
    static ENABLED_STATE: rusty::sync::atomic::AtomicUsize =
        rusty::sync::atomic::AtomicUsize::new(0);
    loop {
        let observed = ENABLED_STATE.load(rusty::sync::atomic::Ordering::Acquire);
        if observed == 2 {
            return false;
        }
        if observed == 3 {
            return true;
        }
        if observed == 0
            && ENABLED_STATE
                .compare_exchange(
                    0,
                    1,
                    rusty::sync::atomic::Ordering::AcqRel,
                    rusty::sync::atomic::Ordering::Acquire,
                )
                .is_ok()
        {
            let enabled = stackless_profile_env();
            ENABLED_STATE.store(
                if enabled { 3 } else { 2 },
                rusty::sync::atomic::Ordering::Release,
            );
            return enabled;
        }
        // Match C++ magic-static initialization: racing callers wait for the
        // one initializer instead of evaluating getenv independently.
        core::hint::spin_loop();
    }
}

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

fn stackless_profile_update_max_slots(slots: usize) {
    g_stackless_profile.max_slots.fetch_max(slots, rusty::sync::atomic::Ordering::Relaxed);
}

fn stackless_profile_report_periodic() {
    if !stackless_profile_enabled() {
        return;
    }
    #[cfg_attr(any(), thread_local)] static mut last_report_us: u64 = 0;
    let now_us: u64 = Time::now(true);
    if unsafe { last_report_us } == 0u64 {
        unsafe { last_report_us = now_us };
        return;
    }
    if now_us - unsafe { last_report_us } < 1000000u64 {
        return;
    }
    unsafe { last_report_us = now_us };

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
    reactor_log_line(Log::INFO, 0i32, core::ptr::null(), format!("[async-prof] reg_calls={} avg_scan={:.2} reuse={} new={} max_slots={} poll_calls={} poll_ready={} enqueue_calls={}",
        reg_calls, avg_scan, reg_reuse, reg_new, max_slots, poll_calls, poll_ready, enqueue_calls));
}

fn stackless_profile_note_enqueue() {
    if stackless_profile_enabled() {
        g_stackless_profile.enqueue_calls.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    }
}

fn reactor_poll_one(r: &Reactor, idx: usize, poll_fn: *mut StacklessPollFn) -> bool {
    if stackless_profile_enabled() {
        g_stackless_profile.poll_calls.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    }
    // The binding is heap-stable and owned by the private owner-thread
    // registry. Task::poll may retain Context* until the Task is destroyed.
    let ctx_ptr = stackless_wake_context_ptr::<()>(r, idx);
    reactor_verify(!ctx_ptr.is_null());
    let ctx_ref: &mut rusty::Context = unsafe { &mut *ctx_ptr };
    unsafe { (*poll_fn)(ctx_ref) }
}

fn stackless_profile_note_poll_ready() {
    if stackless_profile_enabled() {
        g_stackless_profile.poll_ready.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    }
}

fn stackless_profile_report_periodic_shim() {
    stackless_profile_report_periodic();
}

fn stackless_profile_note_register(scanned: usize, reuse: bool, slots_now: usize) {
    if !stackless_profile_enabled() {
        return;
    }
    g_stackless_profile.reg_calls.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    g_stackless_profile.reg_scan_steps.fetch_add(scanned as u64, rusty::sync::atomic::Ordering::Relaxed);
    if reuse {
        g_stackless_profile.reg_reuse.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
    } else {
        g_stackless_profile.reg_new.fetch_add(1u64, rusty::sync::atomic::Ordering::Relaxed);
        stackless_profile_update_max_slots(slots_now);
    }
}

fn fiber_current_fiber() -> Option<Rc<Fiber>> {
    let guard = unsafe { sp_running_fiber_th_.borrow() };
    Some((*guard).as_ref()?.clone())
}

// `pub` restores the incumbent carrier's visibility. In the hand-written
// reactor/reactor.cpp this declaration lived inside `export namespace srpc`
// (line 1899 of the c6c55ba carrier), so importers could name it; the
// promotion to canonical Rust dropped the `pub` and made it module-private.
// srpc.server calls it by name through cpp-module-index.toml
// (`[modules."srpc::reactor".symbols.fiber_create_run_impl]`, kind =
// "function"), which only resolves against an exported declaration. The
// strong symbol itself is unchanged and was already in the reactor ratchet
// and in the frozen incumbent oracle (line 197 of
// /var/tmp/reactor-incumbent-owned.unique.demangled).
pub fn fiber_create_run_impl(func: FiberFn, file: SrcFileCStr, line: i64) -> Rc<Fiber> {
    let reactor_rc = Reactor::get_reactor();
    reactor_create_run_fiber_at_impl(&reactor_rc, func, file, line)
}

pub fn fiber_sleep(microseconds: u64) {
    if microseconds == 0u64 {
        return;
    }
    let x = create_sp_timeout_event(microseconds);
    (*x).wait();
}

fn reactor_make() -> Rc<Reactor> {
    Rc::new(Reactor::new())
}

fn reactor_log_create(disk: bool) {
    if disk {
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "create a disk fiber scheduler".to_string());
        return;
    }
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "create a fiber scheduler".to_string());
    if !reusing_fiber() {
        reactor_log_line(Log::WARN, 0i32, core::ptr::null(), "reusing fiber not enabled!".to_string());
    }
}

fn reactor_tls_get() -> Rc<Reactor> {
    unsafe {
        if sp_reactor_th_.is_none() {
            reactor_log_create(false);
            let r = reactor_make();
            r.thread_id_.set(rusty::thread::current_id());
            sp_reactor_th_ = Some(r);
        }
        sp_reactor_th_.as_ref().unwrap().clone()
    }
}

fn reactor_tls_get_disk() -> Rc<Reactor> {
    unsafe {
        if sp_disk_reactor_th_.is_none() {
            reactor_log_create(true);
            let r = reactor_make();
            r.thread_id_.set(rusty::thread::current_id());
            sp_disk_reactor_th_ = Some(r);
        }
        sp_disk_reactor_th_.as_ref().unwrap().clone()
    }
}

fn reactor_tls_save_running() -> Option<Rc<Fiber>> {
    let guard = unsafe { sp_running_fiber_th_.borrow() };
    if (*guard).is_some() {
        return Some((*guard).as_ref().unwrap().clone());
    }
    None
}

fn reactor_tls_restore_running(old_fiber: Option<Rc<Fiber>>) {
    let mut guard = unsafe { sp_running_fiber_th_.borrow_mut() };
    *guard = old_fiber;
}

fn reactor_tls_set_running(fiber: &Rc<Fiber>) {
    let mut guard = unsafe { sp_running_fiber_th_.borrow_mut() };
    *guard = Some(fiber.clone());
}

fn reactor_get_or_create_fiber_impl(self_: &Reactor, func: FiberFn, file: SrcFileCStr, line: i64) -> Rc<Fiber> {
    let mut available_guard = self_.available_fibers_.borrow_mut();
    if reusing_fiber() && !available_guard.is_empty() {
        self_.n_idle_fibers_.set(self_.n_idle_fibers_.get() - 1i64);
        let fiber: Rc<Fiber> = available_guard.pop().unwrap();
        // Cell/RefCell interior mutability re-stamps the recycled fiber
        // through the shared handle (safe: single-threaded).
        fiber.id.set(fiber_next_global_id());
        *fiber.func_.borrow_mut() = func;
        // Keep the existing task/stack so continue_() can resume from the
        // fiber's yield point.
        reactor_verify((*fiber.fiber_task_.borrow()).is_some());
        fiber.status_.set(FiberStatus::RECYCLED);
        fiber
    } else {
        let fiber: Rc<Fiber> = Rc::new(Fiber::new(func));
        self_.n_created_fibers_.set(self_.n_created_fibers_.get() + 1i64);
        if self_.n_created_fibers_.get() % 1024i64 == 0i64 {
            reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), format!("created {}, busy {}, idle {} fibers on server {}, recent {}:{}",
                           self_.n_created_fibers_.get(),
                           self_.n_busy_fibers_.get(),
                           self_.n_idle_fibers_.get(),
                           self_.server_id_.get(),
                           file,
                           line));
        }
        fiber
    }
}

fn reactor_create_run_fiber_impl(self_: &Reactor, func: FiberFn) -> Rc<Fiber> {
    reactor_create_run_fiber_at_impl(self_, func, "", 0i64)
}

fn reactor_create_run_fiber_at_impl(self_: &Reactor, func: FiberFn, file: SrcFileCStr, line: i64) -> Rc<Fiber> {
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
    let status = fiber.status_.get();
    if status == FiberStatus::INIT {
        (*fiber).run();
    } else {
        reactor_verify(status == FiberStatus::RECYCLED);
        (*fiber).continue_();
    }
    if (*fiber).finished() {
        // Named binding: `&mut local` lowers to a POINTER, which will not
        // bind to recycle's `Rc<Fiber>&`; a typed `&mut` binding lowers
        // to a reference.
        let fiber_ref: &mut Rc<Fiber> = &mut fiber;
        self_.recycle(fiber_ref);
    }

    // Step 6: Process events
    self_.run_loop(false, true);

    // Step 7: Restore previous running fiber
    self_.restore_running_fiber(old_fiber);

    fiber
}

// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
fn reactor_spawn_stackless_task_impl(self_: &Reactor, mut task: TaskVoid) {
    reactor_verify(rusty::thread::current_id() == self_.thread_id_.get());
    let ingress = stackless_wake_ingress::<()>(self_);
    let mut early_binding = stackless_wake_make_binding::<()>(ingress);
    let early_ticket = early_binding.ticket.clone();
    let ectx: &mut rusty::Context = stackless_wake_binding_context::<()>(&mut early_binding);
    if task.poll(ectx).is_ready() {
        // Task retains Context*. Keep the binding alive through destruction.
        drop(task);
        return;
    }

    let ts = StacklessVoidTaskState {
        early_binding,
        task: RefCell::<TaskVoid>::new(task),
    };
    let state: Arc<StacklessVoidTaskState> = Arc::new(ts);
    let completion_ticket = early_ticket.clone();
    let poller = StacklessPollFn::from_callable(move |ctx: &mut rusty::Context| -> bool {
        // Scoped so the task borrow is released before the ready-path store.
        let ready: bool = {
            let mut tguard = state.task.borrow_mut();
            (*tguard).poll(ctx).is_ready()
        };
        if !ready {
            return false;
        }
        completion_ticket.slot.store(
            STACKLESS_UNREGISTERED_SLOT,
            rusty::sync::atomic::Ordering::Release,
        );
        true
    });
    let idx = self_.register_stackless_poller(poller);
    if idx == STACKLESS_UNREGISTERED_SLOT {
        // See reactor_spawn_stackless_task_with_result: the rejected poller and
        // its Task are already destroyed and the cancellation is already
        // recorded.  A suspended Task<void> destroyed here never resumes its
        // continuation, so this must not look like a successful spawn.
        return;
    }
    early_ticket.slot.store(idx, rusty::sync::atomic::Ordering::Release);
    let ingress_ready = stackless_wake_take_pending::<()>(self_);
    for ready_idx in ingress_ready {
        self_.enqueue_stackless_task(ready_idx);
    }
}

fn pollworker_make(receiver: PollCmdReceiver) -> PollThreadWorker {
    PollThreadWorker {
        receiver_: receiver,
        poll_: Epoll::new(),
        fd_to_pollable_: Default::default(),
        mode_: Default::default(),
        pending_remove_: Default::default(),
        jobs_: Default::default(),
        stop_: false,
    }
}

fn pollworker_create(receiver: PollCmdReceiver) -> Rc<RefCell<PollThreadWorker>> {
    let mut worker = pollworker_make(receiver);
    Rc::new(RefCell::new(worker))
}

fn pollworker_snapshot_fds(w: &mut PollThreadWorker) -> Vec<i32> {
    let mut fds: Vec<i32> = Vec::new();
    let mut ks = w.fd_to_pollable_.keys();
    for fd in ks {
        fds.push(*fd);
    }
    fds
}

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn pollworker_poll_loop(w: &mut PollThreadWorker) {
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[poll_loop] Starting poll loop".to_string());
    while !w.stop_ {
        pollworker_trigger_job(w);

        // Wait for events (epoll_wait with short timeout). Dispatch
        // through proxy storage by fd; no Pollable* userdata assumptions.
        // Collect the readiness batch first so the callback does not retain a
        // mutable borrow of `poll_` while dispatch re-enters the worker.
        let mut ready_batch: Vec<(i32, i32)> = Vec::new();
        w.poll_.Wait(|fd: i32, ready_events: i32| {
            ready_batch.push((fd, ready_events));
        });
        for (fd, ready_events) in ready_batch {
            let mut write_mode: Option<i32> = None;
            // The re-annotating `let p: &mut Box<dyn PollableBase> = p;` below
            // is load-bearing, not style — here and at every other if-let over
            // `fd_to_pollable_` in this file.  An if-let payload carries no
            // annotation, and the emitter does not model `HashMap::get_mut`'s
            // return type, so a bare `p` lowers to `decltype(auto)`; the
            // auto-deref Rust performs (`Box<dyn PollableBase>` ->
            // `dyn PollableBase`) is then not reproduced and the generated
            // `p.handle_read()` fails to compile against
            // `rusty::Box<PollableBase>`.  Restating the type hands the emitter
            // back the `rusty::Box<PollableBase>&` binding it needs.
            let opt = w.fd_to_pollable_.get_mut(&fd);
            if let Some(p) = opt {
                let p: &mut Box<dyn PollableBase> = p;
                if (ready_events & PollReady::READABLE) != 0i32 {
                    p.handle_read();
                }
                if (ready_events & PollReady::WRITABLE) != 0i32 {
                    let new_mode = p.handle_write();
                    if new_mode != PollMode::NO_CHANGE {
                        write_mode = Some(new_mode);
                    }
                }
            }
            if let Some(new_mode) = write_mode {
                pollworker_do_update_mode(w, fd, new_mode);
            }
            if (ready_events & PollReady::ERROR) != 0i32 {
                let err_opt = w.fd_to_pollable_.get_mut(&fd);
                if let Some(p) = err_opt {
                    let p: &mut Box<dyn PollableBase> = p;
                    p.handle_error();
                }
            }
        }

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
            let opt = w.fd_to_pollable_.get_mut(&fd);
            if let Some(p) = opt {
                let p: &mut Box<dyn PollableBase> = p;
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
            let opt = w.fd_to_pollable_.get(&fd);
            if let Some(p) = opt {
                let p: &Box<dyn PollableBase> = p;
                if p.is_closed() {
                    closed_fds.push(fd);
                }
            }
            j += 1;
        }
        let mut n: usize = 0;
        while n < closed_fds.len() {
            let fd = closed_fds[n];
            let proxy_opt = w.fd_to_pollable_.get_mut(&fd);
            if let Some(p) = proxy_opt {
                let p: &mut Box<dyn PollableBase> = p;
                // Remove from epoll if still registered.
                if w.mode_.contains_key(&fd) {
                    w.poll_.Remove(fd);
                }
                // Invoke the close callback before erasing the map entry
                // so cleanup hooks run.
                p.close();
                w.fd_to_pollable_.remove(&fd);
                w.mode_.remove(&fd);
            }
            n += 1;
        }
    }

    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[poll_loop] Exited while loop (stop_=true), starting cleanup".to_string());
    // Shutdown cleanup — unregister all remaining pollables. Only the
    // keys matter here, so the proxies are never touched.
    let rest = pollworker_snapshot_fds(w);
    let mut k: usize = 0;
    while k < rest.len() {
        let fd = rest[k];
        if w.mode_.contains_key(&fd) {
            w.poll_.Remove(fd);
        }
        k += 1;
    }
    w.fd_to_pollable_.clear();
    w.mode_.clear();
    w.pending_remove_.clear();
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[poll_loop] Cleanup complete, poll_loop exiting".to_string());
}

fn pollworker_process_commands(self_: &mut PollThreadWorker) {
    loop {
        let result = self_.receiver_.try_recv();
        if result.is_err() {
            // Empty or disconnected -- either way, stop draining.
            break;
        }
        let cmd = result.unwrap();
        match cmd {
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

fn job_ready(job: &Arc<dyn Job>) -> bool {
    let job_ptr: *const dyn Job = Arc::as_ptr(job);
    let job_mut: *mut dyn Job = job_ptr as *mut dyn Job;
    unsafe { (*job_mut).Ready() }
}

fn job_spawn_work(job: &Arc<dyn Job>) {
    let owned = job.clone();
    Fiber::create_run(move || {
        let job_ptr: *const dyn Job = Arc::as_ptr(&owned);
        let job_mut: *mut dyn Job = job_ptr as *mut dyn Job;
        unsafe { (*job_mut).Work(); }
    });
}

fn pollworker_trigger_job(w: &mut PollThreadWorker) {
    let jobs_exec = core::mem::take(&mut w.jobs_);
    for job in jobs_exec.iter() {
        if job_ready(job) {
            // Ready jobs ran (or are running) — do NOT re-add them.
            job_spawn_work(job);
        } else {
            // Not ready yet — check again on the next pass.
            w.jobs_.insert(job.clone());
        }
    }
}

fn pollworker_do_add_pollable(w: &mut PollThreadWorker, poll: PollableProxy) {
    let fd = pollable_proxy_fd(&poll);
    let poll_mode = pollable_proxy_mode(&poll);

    // The pollable can close between CmdAddPollable being enqueued and
    // processed (teardown racing an accept/connect registration): fd is
    // then -1 and registering would abort inside Epoll::Add. A closed
    // pollable can never produce events — drop it.
    if fd < 0 {
        return;
    }
    if w.fd_to_pollable_.contains_key(&fd) {
        return;
    }
    w.fd_to_pollable_.insert(fd, poll);
    w.mode_.insert(fd, poll_mode);
    // Add fails (-1) on the EBADF teardown race — drop the dead
    // pollable again.
    if w.poll_.Add(fd, poll_mode) != 0 {
        w.fd_to_pollable_.remove(&fd);
        w.mode_.remove(&fd);
    }
}

fn pollworker_do_remove_pollable(w: &mut PollThreadWorker, fd: i32) {
    if !w.fd_to_pollable_.contains_key(&fd) {
        return;
    }
    // Deferred: actual removal happens after epoll_wait.
    w.pending_remove_.insert(fd);
}

fn pollworker_do_close_pollable(w: &mut PollThreadWorker, fd: i32) {
    w.pending_remove_.remove(&fd);
    if !w.fd_to_pollable_.contains_key(&fd) {
        return;
    }
    if w.mode_.contains_key(&fd) {
        w.poll_.Remove(fd);
    }
    // Virtual close through the proxy (arrow kernel: unwrap would copy
    // the move-only Box).
    pollworker_close_proxy_of(w, fd);
    w.fd_to_pollable_.remove(&fd);
    w.mode_.remove(&fd);
}

fn pollworker_do_update_mode(w: &mut PollThreadWorker, fd: i32, new_mode: i32) {
    if !w.fd_to_pollable_.contains_key(&fd) {
        return;
    }
    let mode_opt = w.mode_.get(&fd);
    if mode_opt.is_none() {
        return;
    }
    let old_mode = *mode_opt.unwrap();
    w.mode_.insert(fd, new_mode);
    if new_mode != old_mode {
        w.poll_.Update(fd, new_mode, old_mode);
    }
}

fn pollworker_do_add_job(w: &mut PollThreadWorker, job: Arc<dyn Job>) {
    w.jobs_.insert(job);
}

fn pollworker_do_remove_job(w: &mut PollThreadWorker, job: Arc<dyn Job>) {
    w.jobs_.erase(job);
}

fn pollworker_process_pending_removals(w: &mut PollThreadWorker) {
    // take-to-Vec kernel: the HashSet rejects the rusty::iter shim.
    let remove_fds = pollworker_take_removals(w);
    let mut i: usize = 0;
    while i < remove_fds.len() {
        let fd = remove_fds[i];
        if w.fd_to_pollable_.contains_key(&fd) {
            // fd not reused (still in the mode map) => unregister.
            if w.mode_.contains_key(&fd) {
                w.poll_.Remove(fd);
            }
            w.fd_to_pollable_.remove(&fd);
            w.mode_.remove(&fd);
        }
        i += 1;
    }
}

// MEASURED allow (clippy::borrowed_box).  `&Box<dyn PollableBase>` is the
// annotation the emitter needs: `PollableProxy` IS `rusty::Box<PollableBase>`
// in C++, and the emitter cannot perform Rust's `Box<T>` -> `&T` auto-deref in
// a binding.  Narrowing to `&dyn PollableBase` emits
//     const PollableBase& b = p;   // p is rusty::Box<PollableBase>
// and the module fails to compile -- 3 errors, "no viable conversion from
// 'const ::srpc::PollableProxy' (aka 'const rusty::Box<PollableBase>') to
// 'const PollableBase'" (R/M/obj-BB1.log).  The C++ ABI contract wins.
#[allow(clippy::borrowed_box)]
fn pollable_proxy_fd(p: &PollableProxy) -> i32 {
    let b: &Box<dyn PollableBase> = p;
    b.fd()
}

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

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn pollable_proxy_mode(p: &PollableProxy) -> i32 {
    let b: &Box<dyn PollableBase> = p;
    b.poll_mode()
}

fn pollworker_close_proxy_of(w: &mut PollThreadWorker, fd: i32) {
    // The map port's non-const get() returns Option<V&>; the &mut-typed
    // Box binding keeps the mutable overload selected so close()'s
    // non-const dispatch compiles.
    let proxy_opt = w.fd_to_pollable_.get_mut(&fd);
    if let Some(p) = proxy_opt {
        let p: &mut Box<dyn PollableBase> = p;
        p.close();
    }
}

fn pollworker_update_mode(w: &mut PollThreadWorker, poll: &mut dyn Pollable, new_mode: i32) {
    pollworker_do_update_mode(w, poll.fd(), new_mode);
}

fn thread_id_to_u64(tid: rusty::thread::ThreadId) -> u64 {
    unsafe { core::mem::transmute::<rusty::thread::ThreadId, u64>(tid) }
}

fn pollthread_create() -> Arc<PollThread> {
    let (sender, receiver) = rusty::sync::mpsc::channel::<PollCommand>();
    let seed = PollThread {
        sender_: sender,
        join_handle_: PollJoinSlot::new(None),
        poll_thread_id_bits_: rusty::sync::atomic::AtomicU64::new(0),
        shutdown_called_: rusty::sync::atomic::AtomicBool::new(false),
    };
    let arc: Arc<PollThread> = Arc::new(seed);
    // rusty atomic ops are const, so a const* suffices through the Arc.
    let thread_id_address = (&arc.poll_thread_id_bits_ as *const rusty::sync::atomic::AtomicU64) as usize;
    // One-argument spawn.  The production `rusty::thread::spawn` is variadic
    // (`auto spawn(F&& func, Args&&... args)`), so both `spawn(f, rx)` and
    // `spawn(f_capturing_rx)` lower to a valid call; the single-callable form
    // is the one Rust can model, because Rust has no variadic functions and
    // the canonical client already spells `spawn(move || { ... })`.
    let handle = rusty::thread::spawn(move || {
        let tid = rusty::thread::current_id();
        let thread_id_ptr = thread_id_address as *const rusty::sync::atomic::AtomicU64;
        unsafe { (*thread_id_ptr).store(thread_id_to_u64(tid), rusty::sync::atomic::Ordering::Release) };
        // Raw TLS pointer (not a re-borrow) so fibers on this thread can
        // reach the worker while the borrow_mut guard is held.
        let worker: Rc<RefCell<PollThreadWorker>> = PollThreadWorker::create(receiver);
        let mut guard: RefMut<PollThreadWorker> = worker.borrow_mut();
        unsafe { g_current_poll_worker = &raw mut *guard };
        guard.poll_loop();
        unsafe { g_current_poll_worker = core::ptr::null_mut() };
    });
    {
        let mut slot = arc.join_handle_.lock().unwrap();
        *slot = Some(handle);
    }
    arc
}

fn pollthread_drop(pt: &PollThread) {
    let tid: i64 = current_thread_gettid();
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), format!("[PollThread::~PollThread] Destructor called from TID={}", tid as i32));
    pt.shutdown();
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::~PollThread] Destructor complete".to_string());
}

fn fiber_yield_invoke(y: &mut fiber_yield_t) {
    reactor_verify(!y.task_.is_null());
    unsafe { fiber_engine_yield(&mut (*y.task_).fib_); }
}

/// The one C -> C++ reentry point.  C linkage and the raw void-pointer cast are
/// both authored here so the generated symbol remains the C engine's callback.
///
/// # Safety
///
/// `arg` must be the `*mut fiber_task_t` that `fiber_engine_start` handed to
/// the C fiber engine for this fiber, and the pointee must still be alive --
/// i.e. this may only be called by the engine, on that fiber's own stack,
/// before `fiber_task_t` is destroyed.  It is never called from Rust or C++.
#[no_mangle]
pub unsafe extern "C" fn fiber_task_entry_thunk(arg: *mut core::ffi::c_void) {
    let task: *mut fiber_task_t = arg as *mut fiber_task_t;
    unsafe { fiber_task_body_invoke(&mut (*task).fn_, &mut (*task).yield_); }
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
    reactor_verify(!f.is_empty());
    (*f)(y);
}

#[cfg_attr(any(), cpp_namespace(::janus))]
// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
pub fn quorum_event_make(n_total: i32, quorum: i32) -> Arc<QuorumEvent> {
    let sp = Arc::new(QuorumEvent {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: rusty::thread::current_id(),
        state_: EventState::new(),
        prunable_: Cell::new(true),
        self_: Weak::<QuorumEvent>::new(),
        n_voted_yes_: Cell::new(0),
        n_voted_no_: Cell::new(0),
        xids_: RefCell::new(HashMap::new()),
        n_total_: n_total,
        quorum_: quorum,
        policy_: Cell::new(QuorumPolicy::DEFAULT),
        committed_seen_: Cell::new(false),
        num_leader_: Cell::new(0),
        n_leader_yes_: Cell::new(0),
        n_leader_no_: Cell::new(0),
        highest_term_: Cell::new(0),
        timeouted_: Cell::new(false),
        leader_id_: Cell::new(0),
        par_id_: Cell::new(-1),
        id_: Cell::new(u64::MAX),
        finalize_event_: create_sp_int_event(n_total),
    });
    event_state_seed(&sp.state_);
    sp
}

#[cfg_attr(any(), cpp_namespace(::janus))]
pub fn create_sp_quorum_event(n_total: i32, quorum: i32) -> Arc<QuorumEvent> {
    reactor_setup_sp_event::<QuorumEvent>(quorum_event_make(n_total, quorum))
}

#[cfg_attr(any(), cpp_namespace(::janus))]
fn quorum_collect_dangling(qe: *const QuorumEvent) -> QuorumDanglingVec {
    let mut v: QuorumDanglingVec = Default::default();
    let guard = unsafe { (*qe).xids_.borrow_mut() };
    for it in (*guard).iter() {
        // SAFETY: std::make_pair has no caller-side precondition; this checked
        // foreign call preserves the historical std::pair callback ABI.
        v.push(unsafe { cpp_std::make_pair(*it.0, *it.1) });
    }
    v
}

#[cfg_attr(any(), cpp_namespace(::janus))]
fn quorum_event_finalize(qe: &QuorumEvent, timeout: u64,
                         mut finalize_func: QuorumFinalizeFn) {
    let qe_ptr: *const QuorumEvent = qe as *const QuorumEvent;
    Fiber::create_run(move || {
        let final_ev = unsafe { (*qe_ptr).finalize_event_.clone() }; // comment A
        let mut dangling_rpc: QuorumDanglingVec = quorum_collect_dangling(qe_ptr);
        (*final_ev).wait_timeout(timeout);
        // A: by the time this fires, the quorum event could have been
        // freed. Avoid touching qe_ptr or its members after this line.
        if final_ev.status_.get() == EventStatus::TIMEOUT {
            // Didn't receive all RPC replies.
            let dr: &mut QuorumDanglingVec = &mut dangling_rpc;
            let _ret = finalize_func(dr);
            // Drain guard: a TIMEOUT'd event is never evicted by the
            // reactor loop (extract takes READY, retain drops DONE), so
            // a registered finalize_event_ would otherwise linger in the
            // queues forever at broadcast rate. Mark it DONE here (we
            // run on the owner thread) so the next pass evicts and
            // prune can free it.
            final_ev.status_.set(EventStatus::DONE);
        }
    });
}

// Reads/clears the reactor's shared slow_ flag (matches the former
// QuorumEvent::is_slow / Event::is_slow); the param is unused — the
// flag is reactor-global.
#[cfg_attr(any(), cpp_namespace(::janus))]
fn quorum_event_is_slow(_qe: &QuorumEvent) -> bool {
    let r = Reactor::get_reactor();
    let result: bool = r.slow_.get();
    r.slow_.set(false);
    result
}
