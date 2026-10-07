//! Canonical Rust source for the historical `srpc.reactor` provider.
//!
//! The crate view is `#[path = "../reactor/reactor.rs"] pub mod reactor;` in the
//! generated `src/lib.rs`, which points straight back at this source of truth.
//!
//! Per-thread state is spelled with Rust's `thread_local!` macro, which is
//! real in BOTH lanes: rustc gets the std macro (per-thread by construction),
//! and the transpiler lowers each declaration to a C++
//! `inline thread_local rusty::LocalKey<T>` whose closure-only `.with()`
//! accessor the access sites already use.  This retired the old
//! `#[cfg_attr(any(), thread_local)]` + `static mut` model, under which the
//! same statics were silently process-global under rustc and any
//! multi-threaded use raced.  Native generated-C++ race, teardown, layout,
//! and symbol gates remain mandatory.
//!
//! Stackless wakeups use a private owner-thread ingress.  Wakers retain only
//! thread-safe heap tickets/queues; the Reactor pointer never crosses threads,
//! and each poll borrows a Context built from an owned standard Waker.
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

use std::cell::{Cell, RefCell, RefMut};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::rc::Rc;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll, Wake, Waker};
use std::sync::{Arc, Weak};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64};

use crate::basetypes::Time;
use crate::epoll_wrapper::{PollMode, SrpcEpollBackend};
use crate::misc::Job;
use crate::pollable_proxy::{PollableBase, PollableProxy};
use crate::logging::{log_line, Log};
use crate::threading as _;
use crate::debugging::verify_at;

pub type SrcFileCStr = &'static str;
// Explicit standard identities let the C++ emitter prove imported callback
// aliases despite this module's thread_local! declarations.
pub type EventTestFn = ::core::option::Option<
    ::std::boxed::Box<dyn ::core::ops::Fn(::core::primitive::i32) -> ::core::primitive::bool>,
>;
pub type FiberFn = ::core::option::Option<::std::boxed::Box<dyn ::core::ops::FnMut()>>;
pub type FiberTaskFn = Option<Box<dyn FnMut(&mut fiber_yield_t)>>;
pub type StacklessPollFn = Option<Box<dyn FnMut(&mut Context<'_>) -> bool>>;
pub type TaskVoid = Pin<Box<dyn Future<Output = ()>>>;
pub type PollCmdReceiver = std::sync::mpsc::Receiver<PollCommand>;
pub type FdSet = HashSet<i32>;
pub type PollJoinSlot = std::sync::Mutex<Option<std::thread::JoinHandle<()>>>;
// The tuple alias keeps the historical std::pair callback element profile.
pub type QuorumDangling = (u16, i64);
pub type QuorumDanglingVec = Vec<QuorumDangling>;
pub type QuorumFinalizeFn = Option<Box<dyn FnMut(&mut QuorumDanglingVec) -> bool>>;
pub type StacklessProfileCountU64 = std::sync::atomic::AtomicU64;
pub type StacklessProfileCountUsize = std::sync::atomic::AtomicUsize;
// rustc models C `char` as an i8 on the supported Unix targets, while the
// production C++ declaration must retain the distinct built-in `char` type.
// `rust-type-map.toml` maps this established facade name to C++ `char`.
type LegacyCChar = i8;

/// Native x86-64 fiber register layout, shared with the C/assembly engine.
#[cfg(target_arch = "x86_64")]
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct srpc_fiber_ctx {
    pub rsp: *mut core::ffi::c_void,
    pub rip: *mut core::ffi::c_void,
    pub rbx: usize,
    pub rbp: usize,
    pub r12: usize,
    pub r13: usize,
    pub r14: usize,
    pub r15: usize,
}

/// Native AArch64 register layout from reactor/srpc_fiber.h.
#[cfg(target_arch = "aarch64")]
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct srpc_fiber_ctx {
    pub sp: *mut core::ffi::c_void,
    pub pc: *mut core::ffi::c_void,
    pub x19: usize,
    pub x20: usize,
    pub x21: usize,
    pub x22: usize,
    pub x23: usize,
    pub x24: usize,
    pub x25: usize,
    pub x26: usize,
    pub x27: usize,
    pub x28: usize,
    pub fp: usize,
}

/// Native fiber state shared with reactor/srpc_fiber.h.
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct srpc_fiber {
    pub caller_ctx: srpc_fiber_ctx,
    pub fiber_ctx: srpc_fiber_ctx,
    pub stack_mapping: *mut core::ffi::c_void,
    pub stack_mapping_bytes: usize,
    pub state: i32,
    pub entry_fn: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub entry_arg: *mut core::ffi::c_void,
}

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
    verify_at(value, file!(), line!());
}

// NOT named `log_line`: the imported `srpc::logging::log_line` lands in the
// same C++ namespace `srpc`, so a same-named local wrapper joins its overload
// set and the forwarding call below resolves back to ITSELF.
fn reactor_log_line(level: i32, line: i32, file: *const i8, message: String) {
    // The production logger consumes the message synchronously and retains no
    // borrow; the owned Rust value therefore has exactly the required extent.
    unsafe { log_line(level, line, file, &message) };
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

thread_local! {
    pub static sp_reactor_th_: RefCell<Option<Rc<Reactor>>> =
        const { RefCell::new(Option::<Rc<Reactor>>::None) };
    pub static sp_disk_reactor_th_: RefCell<Option<Rc<Reactor>>> =
        const { RefCell::new(Option::<Rc<Reactor>>::None) };
}
thread_local! {
    pub static sp_running_fiber_th_: RefCell<Option<Rc<Fiber>>> =
        const { RefCell::new(Option::<Rc<Fiber>>::None) };
}

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
    pub wp_fiber_: RefCell<std::rc::Weak<Fiber>>,
}

impl EventState {
    pub fn new() -> Self {
        Self {
            __debug_creator: 0,
            test_: RefCell::new(Default::default()),
            wakeup_time_: Cell::new(0),
            rcd_wait_: Cell::new(false),
            wait_place_: RefCell::new(String::new()),
            wp_fiber_: RefCell::new(std::rc::Weak::new()),
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
    fn core_owner_thread(&self) -> std::thread::ThreadId;
    fn core_state(&self) -> &EventState;
    fn core_state_mut(&mut self) -> &mut EventState;
    fn core_self(&self) -> &Weak<dyn EventPollable>;
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable>;
    fn core_is_composite(&self) -> bool;
    // Every event's readiness changes only through something that tests it
    // (S4 of docs/dev/lion-runtime-plan.md): its own methods (`set`,
    // `vote_*`, or a direct `test()` after a field write); for a TimeoutEvent,
    // the reactor's deadline map; for a WaitAny or WaitAll, a child's test
    // (event_parents_notify); for a predicate IntEvent, its publisher, which
    // calls set() or, from another thread, pings the owner (event_ping), whose
    // drain tests it.  So every event waited on its owner thread wakes on
    // change: its WAIT->READY edge queues it on the owner's ready queue, and
    // run_loop never re-tests it.  A predicate must be installed before
    // `wait`, and a predicate over state another thread publishes needs that
    // publisher to ping.
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
    pub owner_thread_: std::thread::ThreadId,
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
}

fn boxevent_make<Type: Clone + Default + 'static>() -> Arc<BoxEvent<Type>> {
    let sp: Arc<BoxEvent<Type>> = Arc::new(BoxEvent::<Type> {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: std::thread::current().id(),
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
    pub owner_thread_: std::thread::ThreadId,
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
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
    if guard.is_some() {
        return guard.as_ref().unwrap()(ev.value_.get());
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
    pub owner_thread_: std::thread::ThreadId,
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
}

#[repr(C)]
pub struct TimeoutEvent {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: std::thread::ThreadId,
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
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
    pub owner_thread_: std::thread::ThreadId,
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { true }
}

#[repr(C)]
pub struct WaitAll {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: std::thread::ThreadId,
    pub state_: EventState,
    pub prunable_: Cell<bool>,
    pub self_: Weak<dyn EventPollable>,
    pub events_: RefCell<Vec<Arc<dyn EventPollable>>>,
}

impl WaitAll {
    pub fn add_event(&self, x: Arc<dyn EventPollable>) {
        // The child tells this parent when it becomes ready (S4 step 4).
        event_parent_link::<()>(&x, &self.self_);
        // Bind the guard, then deref — chaining `.borrow_mut().push(x)`
        // mis-lowers to push(Vec::from_iter(x)). See §8.33.
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
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
    pub _pin: std::marker::PhantomPinned,
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
            _pin: std::marker::PhantomPinned {},
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

thread_local! {
    pub static g_fiber_global_id: Cell<u64> = const { Cell::new(0) };
}

fn fiber_next_global_id() -> u64 {
    g_fiber_global_id.with(|id| {
        let r = id.get();
        id.set(r + 1u64);
        r
    })
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
    pub _pin: std::marker::PhantomPinned,
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
            _pin: std::marker::PhantomPinned {},
        }
    }

    pub fn current_fiber() -> Option<Rc<Fiber>> {
        fiber_current_fiber()
    }

    pub fn create_run<Func>(func: Func) -> Rc<Fiber>
    where
        Func: FnMut() + 'static,
    {
        Fiber::create_run_impl(Some(Box::new(func)), "", 0i64)
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
    pub poll_once: StacklessPollFn,
}

const STACKLESS_UNREGISTERED_SLOT: usize = usize::MAX;

// The wake handle of a PollThread's driver task (S3 of
// docs/dev/lion-runtime-plan.md).  A PollThread runs a Lion runtime, and one
// Lion task on it, the driver, does the owner-side work that run_loop does on
// a thread with no loop: commands and jobs, pings, the ready queue, expired
// deadlines.  Each source of such work wakes the driver through this handle
// on its own empty->non-empty edge: a queued command, the ping that made the
// ping ingress non-empty, the first wake queued on the stackless ingress, an
// event queued on an empty ready queue, a deadline earlier than the one the
// driver sleeps until, and a pending write for a registered descriptor.  Any
// thread may wake it; only the poll thread drains.
//
// `pending` turns a burst of wakes into one Lion wake: only the wake that sets
// it wakes the waker.  The driver clears it before each drain, and after the
// drain publishes its waker and reads it again before it sleeps.  A wake that
// lands before the publish is seen by that read; one that lands after it finds
// the new waker.  Both sides take the `waker` mutex, which orders them.
struct PollDriverWake {
    pending: AtomicBool,
    // The driver task's Lion waker; None before its first poll and after it
    // has finished.
    waker: std::sync::Mutex<Option<Waker>>,
}

// Wake the driver unless a wake is already pending.  Any thread.
fn poll_driver_wake(wake: &PollDriverWake) {
    if wake.pending.swap(true, std::sync::atomic::Ordering::AcqRel) {
        return;
    }
    // Cloned under the lock, woken after it is released: a Lion wake from
    // another thread takes the runtime's own queue lock and signals its
    // eventfd.
    let waker: Option<Waker> = {
        let guard = wake.waker.lock().unwrap();
        (*guard).clone()
    };
    if let Some(waker) = waker {
        waker.wake();
    }
}

// The driver bound to `slot`, if any, woken as by poll_driver_wake.  For the
// ingresses other threads reach (pings, stackless wakes).
fn poll_driver_wake_bound(slot: &std::sync::Mutex<Option<Arc<PollDriverWake>>>) {
    let bound: Option<Arc<PollDriverWake>> = {
        let guard = slot.lock().unwrap();
        (*guard).clone()
    };
    if let Some(wake) = bound {
        poll_driver_wake(&wake);
    }
}

struct StacklessWakeTicket {
    slot: std::sync::atomic::AtomicUsize,
    enqueued: std::sync::atomic::AtomicBool,
}

struct StacklessWakeIngress {
    accepting: std::sync::atomic::AtomicBool,
    pending: std::sync::Mutex<VecDeque<Arc<StacklessWakeTicket>>>,
    // The PollThread driver of the owner thread, if it has one (S3): the
    // first wake queued after a drain wakes it.  On a PollThread every task
    // spawned through reactor_spawn_stackless_task_* runs on Lion instead, so
    // this serves only pollers registered directly with
    // Reactor::register_stackless_poller there.
    driver: std::sync::Mutex<Option<Arc<PollDriverWake>>>,
}

struct StacklessWakeTarget {
    ingress: Arc<StacklessWakeIngress>,
    ticket: Arc<StacklessWakeTicket>,
}

impl Wake for StacklessWakeTarget {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        stackless_wake_request::<()>(&self.ingress, &self.ticket);
    }
}

struct StacklessWakeBinding {
    ticket: Arc<StacklessWakeTicket>,
    waker: Waker,
}

struct StacklessWakeOwner {
    reactor_key: usize,
    ingress: Option<Arc<StacklessWakeIngress>>,
    bindings: Vec<Option<Box<StacklessWakeBinding>>>,
}

struct StacklessResultTaskState<T, OnReady> {
    on_ready: RefCell<Option<OnReady>>,
    task: RefCell<Pin<Box<dyn Future<Output = T>>>>,
}

struct StacklessVoidTaskState {
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
    teardown_tasks: std::sync::atomic::AtomicU64,
    admitted_completions: std::sync::atomic::AtomicU64,
    pending_wakes: std::sync::atomic::AtomicU64,
    rejected_spawns: std::sync::atomic::AtomicU64,
}

// Deliberately the same shape as `g_stackless_profile`, which the incumbent
// object proves carries no owned strong symbol (it is absent from the 300-entry
// manifest).  Atomics give interior mutability, so the binding need not be mut.
static g_stackless_cancel: StacklessCancelCounters = StacklessCancelCounters {
    teardown_tasks: std::sync::atomic::AtomicU64::new(0u64),
    admitted_completions: std::sync::atomic::AtomicU64::new(0u64),
    pending_wakes: std::sync::atomic::AtomicU64::new(0u64),
    rejected_spawns: std::sync::atomic::AtomicU64::new(0u64),
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
        teardown_tasks: g_stackless_cancel.teardown_tasks.load(std::sync::atomic::Ordering::Relaxed),
        admitted_completions: g_stackless_cancel.admitted_completions.load(std::sync::atomic::Ordering::Relaxed),
        pending_wakes: g_stackless_cancel.pending_wakes.load(std::sync::atomic::Ordering::Relaxed),
        rejected_spawns: g_stackless_cancel.rejected_spawns.load(std::sync::atomic::Ordering::Relaxed),
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
    thread_local! {
        static OWNERS: Cell<*mut Vec<StacklessWakeOwner>> = const { Cell::new(core::ptr::null_mut()) };
    }
    OWNERS.with(|slot| slot.as_ptr())
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
    if !ingress.accepting.load(std::sync::atomic::Ordering::Acquire) {
        return;
    }
    if ticket.enqueued.swap(true, std::sync::atomic::Ordering::AcqRel) {
        return;
    }
    let first: bool = {
        let mut pending = ingress.pending.lock().unwrap();
        if ingress.accepting.load(std::sync::atomic::Ordering::Acquire) {
            let was_empty: bool = (*pending).is_empty();
            (*pending).push_back(ticket.clone());
            was_empty
        } else {
            ticket.enqueued.store(false, std::sync::atomic::Ordering::Release);
            false
        }
    };
    // The empty->non-empty edge wakes the owner's PollThread driver, if it
    // has one; on a thread with no loop nothing is bound and the owner's next
    // run_loop pass takes the wake, as before.
    if first {
        poll_driver_wake_bound(&ingress.driver);
    }
}

fn stackless_wake_ingress<WakeDomain>(reactor: &Reactor) -> Arc<StacklessWakeIngress> {
    reactor_verify(std::thread::current().id() == reactor.thread_id_.get());
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
        accepting: std::sync::atomic::AtomicBool::new(true),
        pending: std::sync::Mutex::new(VecDeque::<Arc<StacklessWakeTicket>>::new()),
        driver: std::sync::Mutex::new(poll_driver_wake_of(reactor)),
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

fn stackless_wake_make_binding(ingress: Arc<StacklessWakeIngress>) -> Box<StacklessWakeBinding> {
    let ticket = Arc::new(StacklessWakeTicket {
        slot: std::sync::atomic::AtomicUsize::new(STACKLESS_UNREGISTERED_SLOT),
        enqueued: std::sync::atomic::AtomicBool::new(false),
    });
    let waker = Waker::from(Arc::new(StacklessWakeTarget {
        ingress,
        ticket: ticket.clone(),
    }));
    Box::new(StacklessWakeBinding { ticket, waker })
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
                binding.ticket.slot.store(idx, std::sync::atomic::Ordering::Release);
                owners[i].bindings[idx] = Some(binding);
                return;
            }
            i += 1usize;
        }
    }
    reactor_verify(false);
}

fn stackless_wake_waker<WakeDomain>(reactor: &Reactor, idx: usize) -> Waker {
    let key = stackless_wake_reactor_key::<WakeDomain>(reactor);
    unsafe {
        let owners = &mut *stackless_wake_owners_ptr::<WakeDomain>();
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                reactor_verify(idx < owners[i].bindings.len());
                let binding = owners[i].bindings[idx].as_mut().unwrap();
                return binding.waker.clone();
            }
            i += 1usize;
        }
    }
    reactor_verify(false);
    std::process::abort();
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
                        std::sync::atomic::Ordering::Release,
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
        ticket.enqueued.store(false, std::sync::atomic::Ordering::Release);
        let idx = ticket.slot.load(std::sync::atomic::Ordering::Acquire);
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
                            std::sync::atomic::Ordering::Release,
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
        ingress.accepting.store(false, std::sync::atomic::Ordering::Release);
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
                ticket.enqueued.store(false, std::sync::atomic::Ordering::Release);
                drained += 1u64;
            }
        }
        if drained > 0u64 {
            g_stackless_cancel.pending_wakes.fetch_add(drained, std::sync::atomic::Ordering::Relaxed);
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

// ---------------------------------------------------------------------------
// Owner-side event wake state (S4 of docs/dev/lion-runtime-plan.md)
// ---------------------------------------------------------------------------
//
// run_loop used to find work by re-testing every waiting event on every pass
// and by scanning every timed wait for an expired deadline.  Events now reach
// it through this per-thread state instead, each one when it changes:
//
// * The ready queue (steps 1-2).  An event waited on its owner thread joins
//   no scanned queue.  event_test_impl pushes it here on its WAIT->READY edge,
//   and run_loop moves this queue into its dispatch list on every pass, so
//   run_loop's cost for such events is O(ready), not O(waiting).  The queue
//   holds strong references, so a ready event stays alive until its waiter
//   has been dispatched.
//
// * The deadline map (step 3).  Every timed wait, and every TimeoutEvent from
//   its creation, has an entry keyed by its deadline in Time::now(true)
//   microseconds.  check_timeout pops the expired prefix in deadline order,
//   equal deadlines in insertion order, so run_loop's timer cost is
//   O(expired) and a pass reads the clock only while a deadline is pending.
//   At a timed wait's deadline the rule is the one the linear scan of
//   timeout_events_ had: READY if the event is ready, else TIMEOUT.  Entries
//   are deleted lazily (see EventDeadline).  event_next_deadline_us gives the
//   earliest deadline to a driver that has to sleep until it (S3).
//
// * Parent links (step 4).  A WaitAny or WaitAll child cannot tell its
//   parents anything through `dyn EventPollable`, and the pinned event
//   layouts have no room for a list, so the links live here, keyed by the
//   child's address (event_address).  create_sp_waitany,
//   create_sp_waitall_from and WaitAll::add_event record them; a test() that
//   finds the child ready tests the parents (event_parents_notify), whose
//   WAIT->READY edge then queues them.  A composite holds its children
//   strongly, so a child's address cannot be reused while any parent it lists
//   is alive; entries whose parents all died are pruned lazily.
//
// * Pings (step 5).  An IntEvent predicate may read state that other threads
//   publish; FiberChannel's reads its frame queue and closed latch, filled by
//   transport callbacks on the poll thread, an in-memory sender's thread or
//   any closer's thread.  Such a publisher holds an EventPing ticket and
//   pings it after publishing (event_ping).  The ticket crosses to the owner
//   through a mutex-guarded ingress, in the shape of the stackless wake
//   ingress, with an "already queued" flag so repeated pings before a drain
//   queue it once.  Each pass, the owner takes the pinged tickets, looks up
//   the event each one has armed (event_ping_arm), and tests only those; a
//   ready one takes its ordinary WAIT->READY edge onto the ready queue.
//
// Resumption stays deferred throughout: an edge or a deadline only marks the
// event, and the owner's next drain resumes the waiter through the one
// dispatch block in run_loop, with the same DONE de-dup, registry, PAUSED,
// READY->DONE and sticky-TIMEOUT checks.
//
// One state per thread, owned by that thread's TLS Reactor: reactor_tls_get
// opens it when it creates the Reactor, and Reactor::drop closes it.
// `event_wake_owner_th_` names the owning Reactor, so a disk reactor or a
// directly constructed Reactor on the same thread never drains it.  Only
// trivially destructible values live in TLS, for the reason given at
// stackless_wake_owners_slot: C++ destroys a thread-local container before a
// TLS Reactor that was created ahead of it, and Reactor teardown can still
// reach event_test_impl (a cancelled task's destructor may set an event).
// After the state is closed an edge queues nothing and a wait registers no
// deadline.
//
// Only the owner thread touches the state; the one exception is the ping
// ingress, which exists to be reached from other threads.  An edge taken on
// a foreign thread sets READY and queues nothing.  An untimed waiter is then
// not woken.  A timed waiter is woken at its deadline, because the deadline
// rule finds the event READY and dispatches it.  (Before S4 the per-pass
// scan found such an event by racing on its status Cell, and check_timeout
// took READY entries on every pass.)  Events are owner-thread-only; a
// foreign publisher pings, or goes through another ingress queue.
//
// None of this enters the exact strong-symbol census: the thread-locals lower
// to `inline thread_local`, the state structs are private aggregates with no
// methods, the helpers are generic, and the open, drain and close steps live
// inside functions that already exist.

// One entry of the deadline map.  `clock` marks a TimeoutEvent's own entry:
// that event's readiness is a clock comparison, so reaching the deadline is
// what makes it ready, and the entry tests it -- INIT->DONE, or WAIT->READY
// through the ready queue.  Every other entry is a timed wait, and at its
// deadline the waiter resumes READY if the event is ready, else TIMEOUT.
//
// Deletion is lazy.  A wait that ends first leaves its entry behind, and the
// entry is dropped unserved when it is popped, because its event is gone, is
// no longer waiting, or now waits with a different deadline
// (event_wait_impl records every wait's deadline in wakeup_time_, 0 for an
// untimed wait).  `event` is weak, so an entry never keeps an event alive.
// event_deadline_push sweeps stale entries whenever their number passes a
// threshold, so a long timeout on a wait that ended early costs O(1)
// amortized instead of a map entry until the timeout.
struct EventDeadline {
    deadline: u64,
    clock: bool,
    event: Weak<dyn EventPollable>,
}

struct EventWakeState {
    // Self-notifying events whose WAIT->READY edge was taken on the owner
    // thread, in edge order.
    ready: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    // Pending deadlines.  Each key's entries are in insertion order.
    deadlines: RefCell<BTreeMap<u64, Vec<EventDeadline>>>,
    // Entries in `deadlines`, counting stale ones not yet dropped.
    deadline_entries: Cell<usize>,
    // When `deadline_entries` passes this, event_deadline_push sweeps.
    deadline_sweep_at: Cell<usize>,
    // The smallest key in `deadlines`, or u64::MAX when it is empty.
    next_deadline: Cell<u64>,
    // Each composite child's parents, keyed by the child's event_address.
    parents: RefCell<HashMap<usize, EventParentList>>,
    // Entries in `parents`; 0 lets event_test_impl skip the lookup.
    parent_keys: Cell<usize>,
    // When `parent_keys` passes this, event_parent_link sweeps.
    parents_sweep_at: Cell<usize>,
    // Tickets pinged from any thread since the last drain.
    pings: Arc<EventPingIngress>,
    // The event each armed ticket re-tests, keyed by the ticket's address.
    armed: RefCell<HashMap<usize, Weak<dyn EventPollable>>>,
}

// One composite child's parents.  Weak, so a link never keeps a parent alive.
// Dead links are pruned when the list passes `prune_at`, which is then re-armed
// at twice the live length, so a long-lived child shared by many short-lived
// parents costs O(1) amortized per link.
struct EventParentList {
    parents: Vec<Weak<dyn EventPollable>>,
    prune_at: usize,
}

thread_local! {
    static event_wake_state_th_: Cell<*mut EventWakeState> =
        const { Cell::new(core::ptr::null_mut()) };
    static event_wake_owner_th_: Cell<usize> = const { Cell::new(0usize) };
}

// Queue an event on this thread's ready queue.  Called only from
// event_test_impl's WAIT->READY edge, and only on the event's owner thread.
fn event_ready_enqueue<W: EventCore>(ev: &W) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return;
    }
    let self_ref: Option<Arc<dyn EventPollable>> = ev.core_self().upgrade();
    if self_ref.is_none() {
        return;
    }
    // The state is heap storage owned by this thread's TLS Reactor, and the
    // queue is only ever borrowed for one push or one drain, never across user
    // code.  The guard is typed: the emitter needs RefMut to lower `->`
    // through it.
    let first: bool = unsafe {
        let mut queue_guard: RefMut<VecDeque<Arc<dyn EventPollable>>> = (*state_ptr).ready.borrow_mut();
        let was_empty: bool = queue_guard.is_empty();
        queue_guard.push_back(self_ref.unwrap());
        was_empty
    };
    // On a PollThread the driver drains this queue (S3).  An edge taken
    // outside its poll, in a transport task or a stackless task, wakes it on
    // the empty->non-empty edge.
    if first {
        poll_driver_wake_owner();
    }
}

// Register a deadline for `ev` with this thread's deadline map.  `clock` marks
// a TimeoutEvent's own entry (see EventDeadline).  Owner thread only.
fn event_deadline_push<W: EventCore>(ev: &W, deadline: u64, clock: bool) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    let entry = EventDeadline {
        deadline,
        clock,
        event: ev.core_self().clone(),
    };
    {
        let mut map_guard: RefMut<BTreeMap<u64, Vec<EventDeadline>>> = state.deadlines.borrow_mut();
        map_guard.entry(deadline).or_default().push(entry);
    }
    state.deadline_entries.set(state.deadline_entries.get() + 1usize);
    if deadline < state.next_deadline.get() {
        state.next_deadline.set(deadline);
    }
    if state.deadline_entries.get() > state.deadline_sweep_at.get() {
        event_deadline_sweep::<()>(state);
    }
    // A PollThread's driver sleeps until the earliest deadline it saw (S3);
    // an earlier one pushed outside its poll wakes it to sleep less.
    poll_driver_deadline_added(deadline);
}

// Whether a deadline entry can still do something when it is popped: its event
// is alive and, for a timed wait, still waits on exactly this deadline (it may
// already be READY, which the deadline rule hands over).  A TimeoutEvent's own
// entry stays live until it fires.  Reads statuses only.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn event_deadline_is_live<WakeDomain>(entry: &EventDeadline) -> bool {
    let upgraded: Option<Arc<dyn EventPollable>> = entry.event.upgrade();
    if upgraded.is_none() {
        return false;
    }
    if entry.clock {
        return true;
    }
    let ev: Arc<dyn EventPollable> = upgraded.unwrap();
    let status: EventStatus = (*ev).status();
    (*ev).wakeup_time() == entry.deadline
        && (status == EventStatus::WAIT || status == EventStatus::READY)
}

// Drop every stale entry (see EventDeadline) and re-arm the sweep threshold at
// twice the live count.  Reads statuses only; it tests nothing, so no event
// code runs while the map is borrowed.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`: the tag keeps this helper a template, which
// is what keeps it out of the exact strong-symbol census.
#[allow(clippy::extra_unused_type_parameters)]
fn event_deadline_sweep<WakeDomain>(state: &EventWakeState) {
    let mut map_guard: RefMut<BTreeMap<u64, Vec<EventDeadline>>> = state.deadlines.borrow_mut();
    let mut keys: Vec<u64> = Vec::new();
    {
        let ks = map_guard.keys();
        for key in ks {
            keys.push(*key);
        }
    }
    let mut live: usize = 0usize;
    let mut next: u64 = u64::MAX;
    for key in keys {
        // Pruned in place; an emptied key is removed through
        // event_deadline_remove_key.
        let emptied: bool = {
            let slot: &mut Vec<EventDeadline> = map_guard.get_mut(&key).unwrap();
            slot.retain(move |entry: &EventDeadline| -> bool { event_deadline_is_live::<WakeDomain>(entry) });
            live += slot.len();
            slot.is_empty()
        };
        if emptied {
            let _husk: Vec<EventDeadline> = event_deadline_remove_key::<WakeDomain>(&mut map_guard, key);
        } else if key < next {
            next = key;
        }
    }
    state.deadline_entries.set(live);
    state.next_deadline.set(next);
    state.deadline_sweep_at.set(live * 2usize + 64usize);
}

// Remove `key`, which the caller found present, and return its entries.  A
// plain BTreeMap::remove: the btree port's C++ remove used to copy the value
// out and never destroy the original, so this took the entries out first and
// removed only an empty Vec.  rusty-cpp 400cb4d1 (plan T7) made the port's
// reads relocate, and the workaround is gone.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn event_deadline_remove_key<WakeDomain>(
    map_guard: &mut RefMut<BTreeMap<u64, Vec<EventDeadline>>>,
    key: u64,
) -> Vec<EventDeadline> {
    map_guard.remove(&key).unwrap()
}

// The earliest pending deadline of `reactor`'s deadline map, in
// Time::now(true) microseconds.  None when no deadline is pending, or when
// `reactor` does not own this thread's wake state.  It is a lower bound: an
// entry deleted lazily counts until it is popped, so a driver that sleeps
// until this instant may wake to find nothing due and simply asks again.
// Deliberately private.  check_timeout uses it for its clock-free fast path;
// the only other caller is the Lion driver task of a PollThread (S3), which
// sleeps until it (poll_driver_arm_timer).
fn event_next_deadline_us<WakeDomain>(reactor: &Reactor) -> Option<u64> {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null()
        || event_wake_owner_th_.with(|owner| owner.get()) != stackless_wake_reactor_key::<WakeDomain>(reactor)
    {
        return None;
    }
    let next: u64 = unsafe { (*state_ptr).next_deadline.get() };
    if next == u64::MAX {
        return None;
    }
    Some(next)
}

// Pop every entry whose deadline has passed, in deadline order.  The clock is
// read only when a deadline is pending.  The map is not borrowed once this
// returns, so serving the entries may push new deadlines.
fn event_deadline_take_expired<WakeDomain>(reactor: &Reactor) -> Vec<EventDeadline> {
    let mut expired: Vec<EventDeadline> = Vec::new();
    let next: Option<u64> = event_next_deadline_us::<WakeDomain>(reactor);
    if next.is_none() {
        return expired;
    }
    let time_now: u64 = Time::now(true);
    if time_now < next.unwrap() {
        return expired;
    }
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    let state: &EventWakeState = unsafe { &*state_ptr };
    let mut remaining_next: u64 = u64::MAX;
    {
        let mut map_guard: RefMut<BTreeMap<u64, Vec<EventDeadline>>> = state.deadlines.borrow_mut();
        loop {
            let head: Option<u64> = map_guard.keys().next().copied();
            if head.is_none() {
                break;
            }
            let key: u64 = head.unwrap();
            if key > time_now {
                remaining_next = key;
                break;
            }
            // `mut` keeps the C++ binding non-const, so each entry moves out.
            let mut entries: Vec<EventDeadline> = event_deadline_remove_key::<WakeDomain>(&mut map_guard, key);
            for entry in entries {
                expired.push(entry);
            }
        }
    }
    state.next_deadline.set(remaining_next);
    state.deadline_entries.set(state.deadline_entries.get() - expired.len());
    expired
}

// The address that keys an event in `parents`: its data pointer, the same
// whichever handle reaches it.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn event_address<WakeDomain>(ev: &Arc<dyn EventPollable>) -> usize {
    let ptr: *const dyn EventPollable = Arc::as_ptr(ev);
    ptr as *const u8 as usize
}

// Record that `parent` (a WaitAny or WaitAll) waits on `child`.  Owner thread
// only.  Called after reactor_setup_sp_event: before it, the parent has no
// self weak-link, and setup's Arc::get_mut must see no other reference.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn event_parent_link<WakeDomain>(child: &Arc<dyn EventPollable>, parent: &Weak<dyn EventPollable>) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    let key: usize = event_address::<WakeDomain>(child);
    {
        let mut map_guard: RefMut<HashMap<usize, EventParentList>> = state.parents.borrow_mut();
        let list: &mut EventParentList = map_guard.entry(key).or_insert(EventParentList {
            parents: Vec::new(),
            prune_at: 8usize,
        });
        list.parents.push(parent.clone());
        if list.parents.len() > list.prune_at {
            list.parents.retain(move |p: &Weak<dyn EventPollable>| -> bool { p.strong_count() > 0usize });
            list.prune_at = list.parents.len() * 2usize + 8usize;
        }
        state.parent_keys.set(map_guard.len());
    }
    if state.parent_keys.get() > state.parents_sweep_at.get() {
        event_parent_sweep::<WakeDomain>(state);
    }
}

// Drop every child entry whose parents have all died, and re-arm the sweep
// threshold at twice the live count.  Tests nothing.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn event_parent_sweep<WakeDomain>(state: &EventWakeState) {
    let mut map_guard: RefMut<HashMap<usize, EventParentList>> = state.parents.borrow_mut();
    let mut keys: Vec<usize> = Vec::new();
    {
        let ks = map_guard.keys();
        for key in ks {
            keys.push(*key);
        }
    }
    for key in keys {
        // `mut` keeps the C++ binding non-const, so the list moves back in.
        let mut list: EventParentList = map_guard.remove(&key).unwrap();
        list.parents.retain(move |p: &Weak<dyn EventPollable>| -> bool { p.strong_count() > 0usize });
        if !list.parents.is_empty() {
            list.prune_at = list.parents.len() * 2usize + 8usize;
            map_guard.insert(key, list);
        }
    }
    state.parent_keys.set(map_guard.len());
    state.parents_sweep_at.set(map_guard.len() * 2usize + 64usize);
}

// `ev` was just tested and found ready: test each parent that can still use
// it.  A parent that is waiting takes its WAIT->READY edge, which queues it
// for the owner's drain; a parent nobody waits on yet (INIT) moves to DONE and
// tells its own parents in turn, which is how nested composites propagate.
// A parent already DONE, READY or TIMEOUT has nothing to gain, and is left
// alone, so a satisfied composite is never moved back to INIT here.  The
// list is copied out first: a parent's test() re-enters this function.
// Owner thread only; costs one thread-local read while no composite exists.
fn event_parents_notify<W: EventCore>(ev: &W) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    if state.parent_keys.get() == 0usize {
        return;
    }
    // The links live on the owner thread, like the ready queue.
    if std::thread::current().id() != ev.core_owner_thread() {
        return;
    }
    let self_ref: Option<Arc<dyn EventPollable>> = ev.core_self().upgrade();
    if self_ref.is_none() {
        return;
    }
    let key: usize = event_address::<()>(&self_ref.unwrap());
    let mut parents: Vec<Weak<dyn EventPollable>> = Vec::new();
    {
        let map_guard: RefMut<HashMap<usize, EventParentList>> = state.parents.borrow_mut();
        let found: Option<&EventParentList> = map_guard.get(&key);
        if let Some(list) = found {
            let list: &EventParentList = list;
            for p in list.parents.iter() {
                parents.push(p.clone());
            }
        }
    }
    for p in parents.iter() {
        let upgraded: Option<Arc<dyn EventPollable>> = p.upgrade();
        if let Some(parent) = upgraded {
            let parent: Arc<dyn EventPollable> = parent;
            let status: EventStatus = (*parent).status();
            if status == EventStatus::WAIT || status == EventStatus::INIT {
                (*parent).test();
            }
        }
    }
}

// A cross-thread readiness ping for one predicate event at a time (S4 step 5).
// Built by event_ping_new on any thread and shared with the publishers, which
// call event_ping after publishing.  The owner thread arms it with the event
// its fiber is about to wait on (event_ping_arm), which also binds it to that
// thread's ingress, and disarms it when the wait is over (event_ping_disarm).
// Only atomics and a mutex: the event itself is !Send and never leaves the
// owner, which maps the ticket back to it through its `armed` table.
pub struct EventPing {
    // The ingress of the owner thread that last armed this ticket.
    ingress: std::sync::Mutex<Option<Arc<EventPingIngress>>>,
    // Set by the ping that queues the ticket, cleared by the owner's drain.
    queued: AtomicBool,
}

// The owner side of the pings.  `accepting` goes false when the owner's wake
// state closes, which turns later pings into no-ops.  `signaled` lets the
// owner skip the mutex on a pass with nothing pinged.  `driver` is the owner
// thread's PollThread driver while one runs there (S3): the ping that makes
// the ingress non-empty wakes it.
struct EventPingIngress {
    accepting: AtomicBool,
    signaled: AtomicBool,
    pending: std::sync::Mutex<Vec<Arc<EventPing>>>,
    driver: std::sync::Mutex<Option<Arc<PollDriverWake>>>,
}

// The ticket's address, which keys the owner's `armed` table.  Bound through
// a typed pointer: the emitter lowers `ptr as usize` only from a named one.
// MEASURED allow — see the `extra_unused_type_parameters` note on
// `stackless_wake_owners_slot`.
#[allow(clippy::extra_unused_type_parameters)]
fn event_ping_key<WakeDomain>(ping: &Arc<EventPing>) -> usize {
    let ptr: *const EventPing = Arc::as_ptr(ping);
    ptr as usize
}

// A fresh, unarmed ticket.  Generic for the same reason as
// stackless_cancel_report: a template adds no strong symbol.
pub fn event_ping_new<WakeDomain>() -> Arc<EventPing> {
    Arc::new(EventPing {
        ingress: std::sync::Mutex::new(None),
        queued: AtomicBool::new(false),
    })
}

// Tell the owner that the armed event may have become ready.  Call it after
// publishing the state the event's predicate reads, from any thread.  It
// never tests the event and never resumes a waiter: the owner re-tests the
// armed event on its next run_loop pass.  A ticket already queued and not yet
// drained is not queued again; the drain clears the flag before it tests, so
// a publish that lands after the test re-queues the ticket.  Returns true when
// this ping made the owner's ingress non-empty.  On that edge it also wakes
// the owner's PollThread driver, if the owner runs one (S3); a thread with no
// loop notices on its next run_loop pass.
pub fn event_ping<WakeDomain>(ping: &Arc<EventPing>) -> bool {
    let bound: Option<Arc<EventPingIngress>> = {
        let guard = ping.ingress.lock().unwrap();
        (*guard).clone()
    };
    if bound.is_none() {
        // Never armed: nothing waits on it, and the arming fiber rechecks the
        // published state after arming.
        return false;
    }
    let ingress: Arc<EventPingIngress> = bound.unwrap();
    if !ingress.accepting.load(std::sync::atomic::Ordering::Acquire) {
        return false;
    }
    if ping.queued.swap(true, std::sync::atomic::Ordering::AcqRel) {
        return false;
    }
    // A statement block, not a block expression with an early return: the
    // guard is released at its closing brace, before the driver is woken.
    let mut was_empty: bool = false;
    {
        let mut pending = ingress.pending.lock().unwrap();
        if ingress.accepting.load(std::sync::atomic::Ordering::Acquire) {
            was_empty = (*pending).is_empty();
            (*pending).push(ping.clone());
            ingress.signaled.store(true, std::sync::atomic::Ordering::Release);
        } else {
            ping.queued.store(false, std::sync::atomic::Ordering::Release);
        }
    }
    // Woken after the ingress lock is released, on the edge only: a ping
    // that finds tickets queued knows an earlier one already woke the driver.
    if was_empty {
        poll_driver_wake_bound(&ingress.driver);
    }
    was_empty
}

// Arm `ping` with `ev`: until disarmed, each drained ping of it re-tests `ev`.
// Owner thread only, before the wait, and before the arming code rechecks the
// state the predicate reads; a publish that lands in between is then caught
// either by that recheck or by the ping.
pub fn event_ping_arm<Ev: EventPollable + 'static>(ping: &Arc<EventPing>, ev: &Arc<Ev>) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    {
        let mut guard = ping.ingress.lock().unwrap();
        *guard = Some(state.pings.clone());
    }
    let base: Arc<dyn EventPollable> = ev.clone();
    let key: usize = event_ping_key::<()>(ping);
    let mut armed_guard: RefMut<HashMap<usize, Weak<dyn EventPollable>>> = state.armed.borrow_mut();
    armed_guard.insert(key, Arc::downgrade(&base));
}

// Disarm `ping`.  Owner thread only, once its wait is over.  A ping still
// queued is dropped unserved by the drain.
pub fn event_ping_disarm<WakeDomain>(ping: &Arc<EventPing>) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    let key: usize = event_ping_key::<WakeDomain>(ping);
    let mut armed_guard: RefMut<HashMap<usize, Weak<dyn EventPollable>>> = state.armed.borrow_mut();
    armed_guard.remove(&key);
}

// Test the event armed on each ticket pinged since the last drain.  Owner
// reactor only.  One atomic load when nothing was pinged.  Returns whether any
// ticket was drained.
fn event_ping_drain<WakeDomain>(reactor: &Reactor) -> bool {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null()
        || event_wake_owner_th_.with(|owner| owner.get()) != stackless_wake_reactor_key::<WakeDomain>(reactor)
    {
        return false;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    if !state.pings.signaled.load(std::sync::atomic::Ordering::Acquire) {
        return false;
    }
    let pinged: Vec<Arc<EventPing>> = {
        let mut pending = state.pings.pending.lock().unwrap();
        state.pings.signaled.store(false, std::sync::atomic::Ordering::Release);
        core::mem::take(&mut *pending)
    };
    for ping in pinged.iter() {
        // Clear the flag before the test (AcqRel: the publish that set it is
        // visible to the test), so a publish after the test queues again.
        ping.queued.swap(false, std::sync::atomic::Ordering::AcqRel);
        let key: usize = event_ping_key::<WakeDomain>(ping);
        let armed: Option<Weak<dyn EventPollable>> = {
            let armed_guard: RefMut<HashMap<usize, Weak<dyn EventPollable>>> = state.armed.borrow_mut();
            armed_guard.get(&key).cloned()
        };
        if let Some(weak) = armed {
            let upgraded: Option<Arc<dyn EventPollable>> = weak.upgrade();
            if let Some(ev) = upgraded {
                let ev: Arc<dyn EventPollable> = ev;
                (*ev).test();
            }
        }
    }
    !pinged.is_empty()
}

// Counters over this thread's event wake state, for tests and diagnostics.
// A plain aggregate with no methods, like StacklessCancelReport, so it
// contributes no symbol.
pub struct EventWakeReport {
    // Events on the ready queue, waiting for the owner's next drain.
    pub ready_queued: usize,
    // Deadline entries whose event can still be served (see
    // event_deadline_is_live): a timed wait that has not ended, or a
    // TimeoutEvent that has not fired.
    pub live_deadlines: usize,
    // All deadline entries, counting ended waits whose entries are deleted
    // lazily and have not been dropped yet.
    pub deadline_entries: usize,
    // Composite children with a parent list, counting lists whose parents
    // have all died and are not swept yet.
    pub composite_children: usize,
    // Parent links over all those lists, dead ones included.
    pub parent_links: usize,
    // Pings armed with an event (see EventPing).
    pub armed_pings: usize,
}

// Generic for the same reason as stackless_cancel_report: a template adds no
// strong symbol.  Reads this thread's state whichever Reactor owns it; all
// zero when the thread has none.
pub fn event_wake_report<WakeDomain>() -> EventWakeReport {
    let mut report = EventWakeReport {
        ready_queued: 0usize,
        live_deadlines: 0usize,
        deadline_entries: 0usize,
        composite_children: 0usize,
        parent_links: 0usize,
        armed_pings: 0usize,
    };
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if state_ptr.is_null() {
        return report;
    }
    let state: &EventWakeState = unsafe { &*state_ptr };
    {
        let queue_guard: RefMut<VecDeque<Arc<dyn EventPollable>>> = state.ready.borrow_mut();
        report.ready_queued = queue_guard.len();
    }
    report.deadline_entries = state.deadline_entries.get();
    {
        let map_guard: RefMut<BTreeMap<u64, Vec<EventDeadline>>> = state.deadlines.borrow_mut();
        let vs = map_guard.values();
        for entries in vs {
            for entry in entries.iter() {
                if event_deadline_is_live::<WakeDomain>(entry) {
                    report.live_deadlines += 1usize;
                }
            }
        }
    }
    let parents_guard: RefMut<HashMap<usize, EventParentList>> = state.parents.borrow_mut();
    report.composite_children = parents_guard.len();
    let lists = parents_guard.values();
    for list in lists {
        report.parent_links += list.parents.len();
    }
    let armed_guard: RefMut<HashMap<usize, Weak<dyn EventPollable>>> = state.armed.borrow_mut();
    report.armed_pings = armed_guard.len();
    report
}

thread_local! {
    pub static reactor_clients_th_: RefCell<HashMap<String, Vec<PollableProxy>>> =
        RefCell::new(HashMap::<String, Vec<PollableProxy>>::new());
    pub static reactor_prune_hwm_th_: Cell<usize> = const { Cell::new(64usize) };
}

#[repr(C)]
pub struct Reactor {
    pub server_id_: Cell<i32>,
    pub all_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub waiting_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub composite_events_: RefCell<VecDeque<Arc<dyn EventPollable>>>,
    pub fibers_: RefCell<BTreeMap<usize, Rc<Fiber>>>,
    pub available_fibers_: RefCell<Vec<Rc<Fiber>>>,
    pub looping_: Cell<bool>,
    pub slow_: Cell<bool>,
    pub slow_count_: Cell<i32>,
    pub trying_count_: Cell<i32>,
    pub thread_id_: Cell<std::thread::ThreadId>,
    pub n_created_fibers_: Cell<i64>,
    pub n_busy_fibers_: Cell<i64>,
    pub n_active_fibers_: Cell<i64>,
    pub n_active_fibers_2_: Cell<i64>,
    pub n_idle_fibers_: Cell<i64>,
    // --- srpc's async runtime (the stackless task executor) -----------------
    // These three fields ARE the executor's state: a task table, a free list,
    // and a ready queue.  `run_loop` drains the ready queue via
    // `process_stackless_tasks()` each pass; waking a task pushes its index
    // back onto it. Standard pinned Futures are polled with a borrowed
    // Context, and each Waker owns its wake target. On a PollThread the
    // spawn functions hand pending tasks to Lion instead, and the driver
    // task pumps `run_loop` for what remains here (S3). See
    // docs/async-runtime.md.
    pub stackless_tasks_: RefCell<Vec<StacklessTaskEntry>>,
    pub free_stackless_task_slots_: RefCell<Vec<usize>>,
    pub ready_stackless_tasks_: RefCell<VecDeque<usize>>,
    pub _pin: std::marker::PhantomPinned,
}

impl Reactor {
    pub fn new() -> Reactor {
        Reactor {
            server_id_: Default::default(),
            all_events_: Default::default(),
            waiting_events_: Default::default(),
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
            thread_id_: Cell::new(std::thread::current().id()),
            n_created_fibers_: Default::default(),
            n_busy_fibers_: Default::default(),
            n_active_fibers_: Default::default(),
            n_active_fibers_2_: Default::default(),
            n_idle_fibers_: Default::default(),
            stackless_tasks_: Default::default(),
            free_stackless_task_slots_: Default::default(),
            ready_stackless_tasks_: Default::default(),
            _pin: std::marker::PhantomPinned {},
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
        reactor_verify(std::thread::current().id() == self.thread_id_.get());
        self.looping_.set(infinite);
        loop {
            let mut found_ready_events = true;
            while found_ready_events {
                found_ready_events = false;
                if self.process_stackless_tasks() {
                    found_ready_events = true;
                }
                let mut ready_events: VecDeque<Arc<dyn EventPollable>> = Default::default();
                // Pinged predicate events are re-tested here; a ready one
                // queues itself on the ready queue drained below.
                if event_ping_drain::<()>(self) {
                    found_ready_events = true;
                }
                // The per-pass scans below serve only a wait taken on a thread
                // other than the event's owner (see event_wait_impl): no event
                // type is scanned when waited on its owner thread any more.
                // Both queues are empty otherwise, and cost one borrow and one
                // length check each per pass.
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
                    // Evict both terminal states. TIMEOUT is sticky: only
                    // check_timeout sets it, it has already handed the event
                    // to the dispatcher on the pass that set it, and
                    // event_test_impl never moves a TIMEOUT event again. Kept
                    // here it was re-tested on every pass for as long as the
                    // reactor lived. Eviction leaves the status untouched.
                    waiting_guard.retain(move |ev: &Arc<dyn EventPollable>| -> bool {
                        let status = (*ev).status();
                        status != EventStatus::DONE && status != EventStatus::TIMEOUT
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
                    // Same terminal-state eviction as the waiting queue.
                    composite_guard.retain(move |ev: &Arc<dyn EventPollable>| -> bool {
                        let status = (*ev).status();
                        status != EventStatus::DONE && status != EventStatus::TIMEOUT
                    });
                }
                if do_check_timeout {
                    let before = ready_events.len();
                    self.check_timeout(&mut ready_events);
                    if ready_events.len() > before {
                        found_ready_events = true;
                    }
                }
                // Self-notifying events arrive through the ready queue, pushed
                // by their WAIT->READY edge; the scans above never see them.
                // Drained last so that any edge taken before dispatch, even one
                // taken inside a scan's predicate, is served on this pass.
                // Edges taken during dispatch land in the queue again and are
                // served on the next pass, which runs because dispatch implies
                // found_ready_events.  A timed event can also arrive from
                // check_timeout, which hands expired timers over in deadline
                // order; the DONE de-dup below serves it once.  An entry that
                // is no longer READY is dropped: it was already dispatched, or
                // its event was re-armed, and its next edge queues it again.
                // The queue is borrowed only to read its length and to take it
                // whole, so no event destructor or dispatch runs under a
                // borrow and an edge taken there can still push.  An empty
                // queue costs one thread-local read and one length check per
                // pass: create_run drains on every call, so this sits on the
                // fiber-RPC path.
                let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
                if !state_ptr.is_null() {
                    let pending: usize = {
                        // Typed for the emitter, as in event_ready_enqueue.
                        let queue_guard: RefMut<VecDeque<Arc<dyn EventPollable>>> =
                            unsafe { (*state_ptr).ready.borrow_mut() };
                        queue_guard.len()
                    };
                    if pending > 0usize
                        && event_wake_owner_th_.with(|owner| owner.get()) == stackless_wake_reactor_key::<()>(self)
                    {
                        let mut drained: VecDeque<Arc<dyn EventPollable>> = {
                            let mut queue_guard: RefMut<VecDeque<Arc<dyn EventPollable>>> =
                                unsafe { (*state_ptr).ready.borrow_mut() };
                            core::mem::take(&mut *queue_guard)
                        };
                        let n_before = ready_events.len();
                        move_matching(&mut drained, &mut ready_events, move |ev: &Arc<dyn EventPollable>| -> bool {
                            (*ev).status() == EventStatus::READY
                        });
                        if ready_events.len() > n_before {
                            found_ready_events = true;
                        }
                    }
                }
                // Dispatch ready events. `continue` restructured as nested
                // ifs (the DSL has no continue); the Arc is cloned out of
                // the deque so no reference is held across continue_fiber.
                // An event can be listed twice (the ready queue and its
                // deadline both hand it over), so only a status the hand-over
                // set is dispatched: a READY event becomes DONE on its first
                // dispatch, and a later entry skips it.  WAIT and INIT are
                // skipped too.  They mean the first dispatch resumed a waiter
                // that re-armed the event and waits on it again, a new wait
                // this entry must not end.
                {
                    let mut i: usize = 0usize;
                    while i < ready_events.len() {
                        let ev = ready_events[i].clone();
                        i += 1usize;
                        let handed_over: EventStatus = (*ev).status();
                        if handed_over == EventStatus::READY || handed_over == EventStatus::TIMEOUT {
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
        if guard.len() < reactor_prune_hwm_th_.with(|hwm| hwm.get()) {
            return;
        }
        guard.retain(move |e: &Arc<dyn EventPollable>| -> bool {
            Arc::strong_count(e) > 1usize || !(*e).prunable()
        });
        reactor_prune_hwm_th_.with(|hwm| hwm.set(guard.len() * 2usize + 64usize));
    }
    pub fn create_run_fiber(&self, func: Option<Box<dyn FnMut()>>) -> Rc<Fiber> {
        reactor_create_run_fiber_impl(self, func)
    }
    pub fn continue_fiber(&self, fiber: &Rc<Fiber>) {
        // Save current running fiber for nesting support.
        // `(*…)` is load-bearing for the C++ lane: without it the emitter
        // clones the Ref guard itself (a deleted constructor) instead of
        // auto-dereffing to the Option the way rustc does.
        let old_fiber: Option<Rc<Fiber>> =
            sp_running_fiber_th_.with(|slot| (*slot.borrow()).clone());
        sp_running_fiber_th_.with(|slot| {
            *slot.borrow_mut() = Some(fiber.clone());
        });
        sp_running_fiber_th_.with(|slot| {
            let guard = slot.borrow();
            let running: &Rc<Fiber> = (*guard).as_ref().unwrap();
            reactor_verify(!running.finished());
        });
        self.n_active_fibers_.set(self.n_active_fibers_.get() + 1i64);
        if fiber.status_.get() == FiberStatus::INIT {
            fiber.run();
        } else {
            // Don't hold a borrow across continue_(): the fiber may call
            // create_run() (RefCell double-borrow crash during restart).
            fiber.continue_();
        }
        {
            // The finished check happens under the borrow; recycle() runs
            // after it is released, so a recycle path that re-enters the
            // running-fiber slot can never double-borrow.
            let finished_fiber: Option<Rc<Fiber>> = sp_running_fiber_th_.with(|slot| {
                let guard = slot.borrow();
                let running: &Rc<Fiber> = (*guard).as_ref().unwrap();
                if running.finished() {
                    Some(running.clone())
                } else {
                    None
                }
            });
            if let Some(mut fiber_ref) = finished_fiber {
                self.recycle(&mut fiber_ref);
            }
        }
        sp_running_fiber_th_.with(|slot| {
            *slot.borrow_mut() = old_fiber;
        });
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
            let empty_fn: Option<Box<dyn FnMut()>> = Default::default();
            *fiber.func_.borrow_mut() = empty_fn;
            self.n_idle_fibers_.set(self.n_idle_fibers_.get() + 1i64);
            self.available_fibers_.borrow_mut().push(fiber.clone());
        }
        self.n_busy_fibers_.set(self.n_busy_fibers_.get() - 1i64);
        self.fibers_.borrow_mut().remove(&fiber_registry_key(fiber));
    }

    pub fn enqueue_stackless_task(&self, idx: usize) {
        reactor_verify(std::thread::current().id() == self.thread_id_.get());
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

    pub fn register_stackless_poller(&self, poller: StacklessPollFn) -> usize {
        let ingress = stackless_wake_ingress::<()>(self);
        if !ingress.accepting.load(std::sync::atomic::Ordering::Acquire) {
            // Reactor teardown has started.  Destroy the rejected Task-bearing
            // closure without publishing a slot or a Context binding.  Dropping
            // it here destroys the completion callback and its captures on the
            // owner thread, which is what releases a cancellation-safe waiter.
            drop(poller);
            // Refusing a spawn is a cancellation, so it is reported, never
            // silent: the caller believes it has scheduled work that will now
            // never run, and anything waiting on that work must be told.
            g_stackless_cancel.rejected_spawns.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
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
        let binding = stackless_wake_make_binding(ingress);
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
        reactor_verify(std::thread::current().id() == self.thread_id_.get());
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
                let mut poll_fn: StacklessPollFn = Default::default();
                let mut runnable = false;
                {
                    let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                    if idx < tasks_guard.len() {
                        (*tasks_guard)[idx].queued = false;
                        if (*tasks_guard)[idx].active && (*tasks_guard)[idx].poll_once.is_some() {
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
                        // before releasing its owned Waker binding.
                        stackless_wake_close::<()>(self, idx);
                        let mut tasks_guard = self.stackless_tasks_.borrow_mut();
                        if idx < tasks_guard.len() {
                            stackless_profile_note_poll_ready();
                            (*tasks_guard)[idx].active = false;
                            (*tasks_guard)[idx].queued = false;
                            let empty_fn: StacklessPollFn = Default::default();
                            (*tasks_guard)[idx].poll_once = empty_fn;
                        }
                        drop(tasks_guard);
                        // Task/coroutine destruction may run arbitrary awaiter
                        // destructors that re-enter registration.  Do not
                        // publish this index for reuse until both the old Task
                        // and its owned Waker binding are gone.
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

    // Serve every expired deadline of this reactor (S4 step 3 of
    // docs/dev/lion-runtime-plan.md).  The entries come from the thread's
    // deadline map in deadline order; see EventDeadline for the two kinds and
    // for lazy deletion.  A timed wait that reached its deadline resumes
    // READY if its event is ready, else TIMEOUT -- the rule the linear scan of
    // timeout_events_ applied, now applied through test(), so a ready event
    // takes its ordinary WAIT->READY edge.  An event that is already READY is
    // handed over too: that is how a timed wait whose event was made ready
    // without an owner-thread test() (a foreign-thread set, or a direct field
    // write) still completes, at its deadline.  Only the entry that sets
    // TIMEOUT hands that event over, once: dispatch de-duplicates READY
    // events through DONE, but TIMEOUT is sticky and is not de-duplicated.
    // (The timeout_events_ queue that scan read was retired in S7b.)
    pub fn check_timeout(&self, ready_events: &mut VecDeque<Arc<dyn EventPollable>>) {
        let expired: Vec<EventDeadline> = event_deadline_take_expired::<()>(self);
        for entry in expired.iter() {
            let upgraded: Option<Arc<dyn EventPollable>> = entry.event.upgrade();
            if let Some(ev) = upgraded {
                let ev: Arc<dyn EventPollable> = ev;
                if entry.clock {
                    // A TimeoutEvent became ready.  Its test() is the edge:
                    // INIT->DONE, or WAIT->READY, which also queues it.  Hand
                    // a woken waiter over here as well, so that timers that
                    // expire on one pass resume in deadline order.  Any other
                    // status has nothing left to take: its own timed wait
                    // (fiber_sleep's) may have been served first on this pass.
                    let before: EventStatus = (*ev).status();
                    if before == EventStatus::INIT || before == EventStatus::WAIT {
                        (*ev).test();
                        if before == EventStatus::WAIT && (*ev).status() == EventStatus::READY {
                            ready_events.push_back(ev.clone());
                        }
                    }
                } else if (*ev).wakeup_time() == entry.deadline {
                    let status: EventStatus = (*ev).status();
                    if status == EventStatus::WAIT {
                        if !(*ev).test() {
                            (*ev).set_status(EventStatus::TIMEOUT);
                        }
                        ready_events.push_back(ev.clone());
                    } else if status == EventStatus::READY {
                        ready_events.push_back(ev.clone());
                    }
                }
            }
        }
    }
}

impl Drop for Reactor {
    fn drop(&mut self) {
        reactor_verify(std::thread::current().id() == self.thread_id_.get());
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), format!("[Reactor::~Reactor] Starting destruction, all_events_.len()={}, fibers_.size()={}",
                  self.all_events_.borrow().len(), self.fibers_.borrow().len()));
        // Close this thread's event wake state if this Reactor owns it, before
        // any teardown step can set an event.  Its waiters die with this
        // Reactor, and no later reactor may resume them.  The slot is cleared
        // first, so anything the state's destructor reaches finds no state.
        if event_wake_owner_th_.with(|owner| owner.get()) == stackless_wake_reactor_key::<()>(self) {
            let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
            event_wake_state_th_.with(|slot| slot.set(core::ptr::null_mut()));
            event_wake_owner_th_.with(|owner| owner.set(0usize));
            if !state_ptr.is_null() {
                // Refuse later pings, then release what is queued, as
                // stackless_wake_shutdown_begin does for wakes.  A publisher
                // may still hold a ticket bound to this ingress; it now
                // pings nothing.
                {
                    let state: &EventWakeState = unsafe { &*state_ptr };
                    state.pings.accepting.store(false, std::sync::atomic::Ordering::Release);
                    let drained: Vec<Arc<EventPing>> = {
                        let mut pending = state.pings.pending.lock().unwrap();
                        core::mem::take(&mut *pending)
                    };
                    for ping in drained.iter() {
                        ping.queued.store(false, std::sync::atomic::Ordering::Release);
                    }
                }
                // Allocated by Box::into_raw in reactor_tls_get; the slot no
                // longer names it, so this is the only owner.
                drop(unsafe { Box::from_raw(state_ptr) });
            }
        }
        // Reject new foreign wakes first. Destroy every Task-bearing closure
        // while its owned Waker binding still exists, then retire the
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
            g_stackless_cancel.teardown_tasks.fetch_add(outstanding, std::sync::atomic::Ordering::Relaxed);
            g_stackless_cancel.admitted_completions.fetch_add(admitted, std::sync::atomic::Ordering::Relaxed);
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

// Poll once immediately, register a pending future, and deliver its completed
// value through on_ready. See docs/async-runtime.md for the wake protocol.
//
// Two executors serve this (S3 of docs/dev/lion-runtime-plan.md).  On a
// PollThread, while its driver runs, a task still pending after the first
// poll becomes a Lion `spawn_local` task (stackless_lion_spawn_with_result).
// Everywhere else -- a thread with no loop, and a PollThread once its driver
// has stopped -- it registers with this Reactor's own stackless executor,
// which run_loop pumps, as before.  S7 revisits the split.
pub fn reactor_spawn_stackless_task_with_result<T: 'static, OnReady>(self_: &Reactor, mut task: Pin<Box<dyn Future<Output = T>>>, mut on_ready: OnReady)
where
    OnReady: FnMut(T) + 'static,
{
    reactor_verify(std::thread::current().id() == self_.thread_id_.get());
    if poll_driver_accepts_spawn(self_) {
        stackless_lion_spawn_with_result(task, on_ready);
        return;
    }
    let ingress = stackless_wake_ingress::<()>(self_);
    let mut early_binding = stackless_wake_make_binding(ingress);
    let early_ticket = early_binding.ticket.clone();
    let mut ectx = Context::from_waker(&early_binding.waker);
    if let Poll::Ready(value) = task.as_mut().poll(&mut ectx) {
        on_ready(value);
        return;
    }

    let ts = StacklessResultTaskState {
        on_ready: RefCell::<Option<OnReady>>::new(Some(on_ready)),
        task: RefCell::<Pin<Box<dyn Future<Output = T>>>>::new(task),
    };
    let state: Arc<StacklessResultTaskState<T, OnReady>> = Arc::new(ts);
    let completion_ticket = early_ticket.clone();
    let poller: StacklessPollFn = Some(Box::new(move |ctx: &mut Context<'_>| -> bool {
        // Release the task borrow before invoking user completion code.
        let poll_result = {
            let mut task_guard = state.task.borrow_mut();
            (*task_guard).as_mut().poll(ctx)
        };
        if let Poll::Ready(value) = poll_result {
            completion_ticket.slot.store(
                STACKLESS_UNREGISTERED_SLOT,
                std::sync::atomic::Ordering::Release,
            );
            let cb: Option<OnReady> = {
                let mut cbguard = state.on_ready.borrow_mut();
                (*cbguard).take()
            };
            if let Some(mut f) = cb {
                f(value);
            }
            true
        } else {
            false
        }
    }));
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
    early_ticket.slot.store(idx, std::sync::atomic::Ordering::Release);
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
    let sp: Arc<TimeoutEvent> = reactor_setup_sp_event::<TimeoutEvent>(timeout_event_make(wait_us));
    // A TimeoutEvent is ready once `Time::now(true) > wakeup_time_`, which
    // first holds at wakeup_time_ + 1.  The deadline map tests it then (S4
    // step 3), whether or not anything waits on it yet: that test is the edge
    // that resumes its own waiter and tells a waiting WaitAny/WaitAll parent.
    // Registered here rather than in wait() so that a timer created ahead of
    // its wait, or waited only as a composite's child, still fires on time.
    event_deadline_push::<TimeoutEvent>(&*sp, sp.wakeup_time_ + 1u64, true);
    sp
}

pub fn create_sp_never_event() -> Arc<NeverEvent> {
    reactor_setup_sp_event::<NeverEvent>(never_event_make())
}

pub fn create_sp_waitany(a: Arc<dyn EventPollable>, b: Arc<dyn EventPollable>) -> Arc<WaitAny> {
    let sp: Arc<WaitAny> = reactor_setup_sp_event::<WaitAny>(waitany_make(a, b));
    // Each child tells this parent when it becomes ready (S4 step 4).  Linked
    // here, not in waitany_make: setup must see the only reference.
    for child in sp.events_.iter() {
        event_parent_link::<()>(child, &sp.self_);
    }
    sp
}

pub fn create_sp_waitall() -> Arc<WaitAll> {
    reactor_setup_sp_event::<WaitAll>(waitall_make())
}

pub fn create_sp_waitall_from(evs: &Vec<Arc<dyn EventPollable>>) -> Arc<WaitAll> {
    let sp: Arc<WaitAll> = reactor_setup_sp_event::<WaitAll>(waitall_make_from(evs));
    // Each child tells this parent when it becomes ready (S4 step 4).  Linked
    // here, not in waitall_make_from: setup must see the only reference.
    for child in evs.iter() {
        event_parent_link::<()>(child, &sp.self_);
    }
    sp
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

// True on a thread whose PollThread is running: inside its Lion driver, a
// job, a fiber the driver resumes, or one of its runtime's tasks.
pub fn pollworker_is_on_poll_thread() -> bool {
    poll_driver_th_.with(|driver| !driver.get().is_null())
}

// One OS thread running one Lion runtime (S3 of docs/dev/lion-runtime-plan.md).
// The thread builds the runtime itself (a Lion Runtime is !Send) over
// SrpcEpollBackend and runs the driver task (PollDriverTask) until a Shutdown
// command stops it.  Commands still travel over `sender_`; every method that
// sends also wakes the driver, so nothing polls the channel.  A command sent
// on `sender_` directly waits for the next wake.
#[repr(C)]
pub struct PollThread {
    pub sender_: std::sync::mpsc::Sender<PollCommand>,
    pub join_handle_: PollJoinSlot,
    // Kernel thread ID, with zero meaning the worker has not started.
    // This avoids inspecting the private representation of std ThreadId.
    pub poll_thread_id_bits_: AtomicU64,
    pub shutdown_called_: AtomicBool,
    /// Number of removal commands accepted by the worker's command queue.
    remove_count_: AtomicI32,
    // The driver's wake handle, shared with the poll thread.
    driver_: Arc<PollDriverWake>,
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
        if self.shutdown_called_.swap(true, std::sync::atomic::Ordering::AcqRel) {
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
        poll_driver_wake(&self.driver_);
        reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread::shutdown] CmdShutdown sent".to_string());
        // Thread-safe read of the poll thread's id.
        let poll_tid = self.poll_thread_id_bits_.load(std::sync::atomic::Ordering::Acquire);
        if current_thread_gettid() as u64 == poll_tid {
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
                let _joined = (*guard).take().unwrap().join();
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
        poll_driver_wake(&self.driver_);
    }

    /// Unregister the caller's current descriptor asynchronously.
    /// The caller must keep that descriptor owned until command processing
    /// completes, for example by retaining it through worker shutdown.
    pub fn remove_fd(&self, fd: i32) {
        if self.sender_.send(PollCommand::RemovePollable { fd }).is_ok() {
            self.remove_count_.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            poll_driver_wake(&self.driver_);
        }
    }

    /// Ask the worker to unregister and close its currently owned descriptor.
    /// The caller must identify a live registration and must not independently
    /// close or replace its descriptor while this command is pending.
    pub fn request_close(&self, fd: i32) {
        // Err == the poll worker exited; it already closed everything it owned.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::ClosePollable { fd });
        poll_driver_wake(&self.driver_);
    }

    /// Change the caller's current registration asynchronously. Its descriptor
    /// must remain owned until this command has been processed.
    pub fn update_mode(&self, fd: i32, new_mode: i32) {
        let result = self.sender_.send(PollCommand::UpdateMode { fd, new_mode });
        if result.is_err() {
            reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), "PollThread::update_mode: send failed! Channel disconnected?".to_string());
        }
        poll_driver_wake(&self.driver_);
    }

    pub fn add(&self, job: Arc<dyn Job>) {
        // Err == the poll worker exited; there is no loop left to run the job.
        let _dropped_when_worker_gone =
            self.sender_.send(PollCommand::AddJob { job });
        poll_driver_wake(&self.driver_);
    }

    /// Whether the calling thread is this PollThread's own thread while its
    /// Lion runtime runs: inside its driver, a fiber the driver resumes, a job,
    /// or one of the runtime's tasks.  State that must live on the runtime (a
    /// Lion `AsyncFd`, a `spawn_local` task) can then be created in place
    /// rather than through a job.
    pub fn is_current_thread(&self) -> bool {
        poll_driver_is_current(&self.driver_)
    }

    /// Count accepted remove requests, including requests for an absent fd.
    /// Requests sent after worker shutdown are rejected and do not count.
    pub fn get_remove_count(&self) -> i32 {
        self.remove_count_.load(std::sync::atomic::Ordering::Relaxed)
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
    COMMITTED_SHORT = 3,
    ALWAYS_READY = 4,
}

#[cfg_attr(any(), cpp_namespace(::janus))]
#[repr(C)]
pub struct QuorumEvent {
    pub status_: Cell<EventStatus>,
    pub owner_thread_: std::thread::ThreadId,
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
        self.n_voted_yes_.get() >= self.quorum_
    }
    pub fn no(&self) -> bool {
        if self.policy_.get() == QuorumPolicy::ALL_NO {
            return self.n_voted_no_.get() == self.n_total_;
        }
        reactor_verify(self.n_total_ >= self.quorum_);
        self.n_voted_no_.get() > (self.n_total_ - self.quorum_)
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
    // A QuorumEvent has no child events. It used to report itself composite
    // only so that run_loop would put it on the scanned composite queue; it
    // now wakes on change instead (see the event wake state).
    pub fn is_composite_event(&self) -> bool {
        false
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

#[cfg_attr(any(), cpp_inherit)]
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
    fn core_owner_thread(&self) -> std::thread::ThreadId { self.owner_thread_ }
    fn core_state(&self) -> &EventState { &self.state_ }
    fn core_state_mut(&mut self) -> &mut EventState { &mut self.state_ }
    fn core_self(&self) -> &Weak<dyn EventPollable> { &self.self_ }
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable> { &mut self.self_ }
    fn core_is_composite(&self) -> bool { false }
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
    // lowered to `return *this->q_;` when this was first measured and lowers
    // to `return (rusty::detail::deref_if_pointer_like(this->q_));` under
    // rusty-cpp 3e1d9505 -- a dereference either way, which is the point.
    // The C++ ABI contract wins; scoped to this one item.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it returns `this->q_` -- the handle -- where `const QuorumEvent&` is declared, dropping the deref_if_pointer_like unwrap the accessor lowers to today (2 emitted lines in srpc.reactor.cppm).
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
    reactor_verify(sp_reactor_th_.with(|slot| slot.borrow().is_some()));
    // `.clone()` binds a *value* Rc (not a reference).  The field access is
    // spelled without an explicit `(*…)`: Rust auto-derefs the Rc, and the
    // emitter lowers the bare receiver to a direct `(*reactor_th).thread_id_`,
    // so it reaches through the Rc either way.  (Spelling the deref in Rust
    // instead lowers to the generic `deref_if_pointer_like(reactor_th)` —
    // equivalent, and what this file used to emit.)
    let reactor_th = sp_reactor_th_.with(|slot| slot.borrow().as_ref().unwrap().clone());
    reactor_verify(reactor_th.thread_id_.get() == std::thread::current().id());
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
        // An event waited on its owner thread wakes on change:
        // event_test_impl queues it on its WAIT->READY edge, so it joins no
        // scanned queue and run_loop never re-tests it.  An event waited on
        // another thread -- possible only from C++, since events are !Send --
        // cannot reach this thread's ready queue, so it is still found by
        // run_loop's per-pass scan of waiting_events_.  The same condition
        // decides the edge's enqueue in event_test_impl.
        let wakes_on_change: bool = std::thread::current().id() == ev.core_owner_thread();
        // Inline `borrow_mut().push_back(…)`: the RefMut temporary releases at
        // the end of each statement — before the yield below — so the reactor
        // loop can re-borrow these queues while this fiber sleeps.  With the
        // receiver spelled bare these lower to
        // `(*reactor_rc).waiting_events_.borrow_mut()->push_back(…)` — the
        // guard is still a per-statement temporary.
        if !wakes_on_change {
            reactor_rc.waiting_events_.borrow_mut().push_back(ev.core_self().upgrade().unwrap());
        }

        // A composite waited off its owner thread cannot hear from its
        // children, whose links live on the owner's thread, so it still joins
        // the scanned composite queue.  On the owner thread the children
        // test it (event_parents_notify) and it joins no scanned queue.
        if ev.core_is_composite() && !wakes_on_change {
            reactor_rc.composite_events_.borrow_mut().push_back(ev.core_self().upgrade().unwrap());
        }

        // A timed wait registers its deadline with this thread's deadline
        // map, which check_timeout drains in deadline order (S4 step 3).
        // wakeup_time_ names the deadline of this wait, and 0 for an untimed
        // one: an entry left by an earlier wait of the same event no longer
        // matches it, and is dropped unserved (see EventDeadline).
        if timeout > 0 {
            let now = Time::now(true);
            ev.core_state().wakeup_time_.set(now + timeout);
            event_deadline_push(ev, now + timeout, false);
        } else {
            ev.core_state().wakeup_time_.set(0u64);
        }

        // Transpiled Weak has no implicit Rc→Weak conversion; use the static
        // Rc::downgrade(rc) factory (mirrors std::rc::Rc::downgrade). `fiber` is
        // cloned (a refcount bump) so the factory consumes the temporary and the
        // original `fiber` stays live for the checks below.
        *ev.core_state().wp_fiber_.borrow_mut() = Rc::<Fiber>::downgrade(&fiber);
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
            let on_owner: bool = std::thread::current().id() == ev.core_owner_thread();
            if on_owner {
                // Owner-thread-only: upgrading the weak fiber ref mutates a plain
                // (non-atomic) Rc strong count; doing this from a foreign thread
                // races the owner's own Rc<Fiber> clones and corrupts the count.
                // The upgraded handle is used only for this liveness assertion.
                let option_fiber = ev.core_state().wp_fiber_.borrow().upgrade();
                reactor_verify(option_fiber.is_some());
                reactor_verify(ev.core_status().get() != EventStatus::DEBUG);
            }
            ev.core_status().set(EventStatus::READY);
            // The WAIT->READY edge of an event that wakes on change: queue it
            // once for the owner's next drain (see event_ready_enqueue).  This
            // is reached from set(), vote_*(), a direct test(), a deadline, a
            // child's test and a drained ping alike.  It only queues; the
            // waiter resumes when run_loop drains, never here.
            if on_owner {
                event_ready_enqueue(ev);
            }
        } else if ev.core_status().get() == EventStatus::READY {
            reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "event status ready, triggered?".to_string());
        } else if ev.core_status().get() == EventStatus::DONE
            || ev.core_status().get() == EventStatus::TIMEOUT
        {
            // do nothing
        } else {
            reactor_verify(false);
        }
        // A composite waiting on this event learns of it here (S4 step 4):
        // on the child's INIT->DONE edge from set(), a direct test(), or a
        // child timer's deadline.
        event_parents_notify(ev);
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
        *g2 = Rc::<Fiber>::downgrade(&rc_fiber);
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
        owner_thread_: std::thread::current().id(),
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
        owner_thread_: std::thread::current().id(),
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
        owner_thread_: std::thread::current().id(),
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
        owner_thread_: std::thread::current().id(),
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
        owner_thread_: std::thread::current().id(),
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
        owner_thread_: std::thread::current().id(),
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
    if f.as_ref().unwrap()(sie.value_) {
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
    g.is_some()
}

fn fiber_fn_invoke(f: *const RefCell<FiberFn>) {
    // borrow_mut: rusty::Function::operator() is non-const.
    let mut g = unsafe { (*f).borrow_mut() };
    g.as_mut().unwrap()();
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
    let mut task: FiberTaskFn = Some(Box::new(move |yy: &mut fiber_yield_t| {
        unsafe {
            // The initial callback must run before fiber_install_task stores
            // its Box. This also proves no RefCell borrow spans engine start.
            {
                let tguard = (*self_ptr).fiber_task_.borrow();
                reactor_verify((*tguard).is_none());
            }
            fiber_run_wrapper(&*self_ptr, &raw mut *yy);
        }
    }));
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
    static ENABLED_STATE: std::sync::atomic::AtomicUsize =
        std::sync::atomic::AtomicUsize::new(0);
    loop {
        let observed = ENABLED_STATE.load(std::sync::atomic::Ordering::Acquire);
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
                    std::sync::atomic::Ordering::AcqRel,
                    std::sync::atomic::Ordering::Acquire,
                )
                .is_ok()
        {
            let enabled = stackless_profile_env();
            ENABLED_STATE.store(
                if enabled { 3 } else { 2 },
                std::sync::atomic::Ordering::Release,
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
    reg_calls: std::sync::atomic::AtomicU64::new(0u64),
    reg_scan_steps: std::sync::atomic::AtomicU64::new(0u64),
    reg_reuse: std::sync::atomic::AtomicU64::new(0u64),
    reg_new: std::sync::atomic::AtomicU64::new(0u64),
    poll_calls: std::sync::atomic::AtomicU64::new(0u64),
    poll_ready: std::sync::atomic::AtomicU64::new(0u64),
    enqueue_calls: std::sync::atomic::AtomicU64::new(0u64),
    max_slots: std::sync::atomic::AtomicUsize::new(0usize),
};

fn stackless_profile_update_max_slots(slots: usize) {
    g_stackless_profile.max_slots.fetch_max(slots, std::sync::atomic::Ordering::Relaxed);
}

fn stackless_profile_report_periodic() {
    if !stackless_profile_enabled() {
        return;
    }
    thread_local! {
        static last_report_us: Cell<u64> = const { Cell::new(0) };
    }
    let now_us: u64 = Time::now(true);
    let last = last_report_us.with(|stamp| stamp.get());
    if last == 0u64 {
        last_report_us.with(|stamp| stamp.set(now_us));
        return;
    }
    if now_us - last < 1000000u64 {
        return;
    }
    last_report_us.with(|stamp| stamp.set(now_us));

    let reg_calls: u64 = g_stackless_profile.reg_calls.load(std::sync::atomic::Ordering::Relaxed);
    let reg_scans: u64 = g_stackless_profile.reg_scan_steps.load(std::sync::atomic::Ordering::Relaxed);
    let reg_reuse: u64 = g_stackless_profile.reg_reuse.load(std::sync::atomic::Ordering::Relaxed);
    let reg_new: u64 = g_stackless_profile.reg_new.load(std::sync::atomic::Ordering::Relaxed);
    let poll_calls: u64 = g_stackless_profile.poll_calls.load(std::sync::atomic::Ordering::Relaxed);
    let poll_ready: u64 = g_stackless_profile.poll_ready.load(std::sync::atomic::Ordering::Relaxed);
    let enqueue_calls: u64 = g_stackless_profile.enqueue_calls.load(std::sync::atomic::Ordering::Relaxed);
    let max_slots: usize = g_stackless_profile.max_slots.load(std::sync::atomic::Ordering::Relaxed);

    let mut avg_scan: f64 = 0.0f64;
    if reg_calls > 0u64 {
        avg_scan = (reg_scans as f64) / (reg_calls as f64);
    }
    reactor_log_line(Log::INFO, 0i32, core::ptr::null(), format!("[async-prof] reg_calls={} avg_scan={:.2} reuse={} new={} max_slots={} poll_calls={} poll_ready={} enqueue_calls={}",
        reg_calls, avg_scan, reg_reuse, reg_new, max_slots, poll_calls, poll_ready, enqueue_calls));
}

fn stackless_profile_note_enqueue() {
    if stackless_profile_enabled() {
        g_stackless_profile.enqueue_calls.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
    }
}

fn reactor_poll_one(r: &Reactor, idx: usize, poll_fn: *mut StacklessPollFn) -> bool {
    if stackless_profile_enabled() {
        g_stackless_profile.poll_calls.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
    }
    // The Waker owns its wake target; the Context is borrowed for this poll.
    let waker = stackless_wake_waker::<()>(r, idx);
    let mut context = Context::from_waker(&waker);
    unsafe { (*poll_fn).as_mut().unwrap()(&mut context) }
}

fn stackless_profile_note_poll_ready() {
    if stackless_profile_enabled() {
        g_stackless_profile.poll_ready.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
    }
}

fn stackless_profile_report_periodic_shim() {
    stackless_profile_report_periodic();
}

fn stackless_profile_note_register(scanned: usize, reuse: bool, slots_now: usize) {
    if !stackless_profile_enabled() {
        return;
    }
    g_stackless_profile.reg_calls.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
    g_stackless_profile.reg_scan_steps.fetch_add(scanned as u64, std::sync::atomic::Ordering::Relaxed);
    if reuse {
        g_stackless_profile.reg_reuse.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
    } else {
        g_stackless_profile.reg_new.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
        stackless_profile_update_max_slots(slots_now);
    }
}

fn fiber_current_fiber() -> Option<Rc<Fiber>> {
    // Explicit deref: see continue_fiber's old_fiber note.
    sp_running_fiber_th_.with(|slot| (*slot.borrow()).clone())
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
    sp_reactor_th_.with(|slot| {
        let mut guard = slot.borrow_mut();
        if guard.is_none() {
            reactor_log_create(false);
            let r = reactor_make();
            r.thread_id_.set(std::thread::current().id());
            // Open this thread's event wake state for the new TLS Reactor.  A
            // state can still be open here only if an earlier TLS Reactor's
            // slot was cleared while another Rc kept that reactor alive.  It
            // is handed over rather than freed, so no event is dropped while
            // this slot is borrowed; its entries name the earlier reactor's
            // fibers, which the new owner's registry check skips.  The first
            // deadline and parent sweeps run at 64 entries (see
            // event_deadline_sweep and event_parent_sweep).
            if event_wake_state_th_.with(|slot| slot.get()).is_null() {
                let fresh: Box<EventWakeState> = Box::new(EventWakeState {
                    ready: RefCell::new(VecDeque::<Arc<dyn EventPollable>>::new()),
                    deadlines: RefCell::new(BTreeMap::<u64, Vec<EventDeadline>>::new()),
                    deadline_entries: Cell::new(0usize),
                    deadline_sweep_at: Cell::new(64usize),
                    next_deadline: Cell::new(u64::MAX),
                    parents: RefCell::new(HashMap::<usize, EventParentList>::new()),
                    parent_keys: Cell::new(0usize),
                    parents_sweep_at: Cell::new(64usize),
                    pings: Arc::new(EventPingIngress {
                        accepting: AtomicBool::new(true),
                        signaled: AtomicBool::new(false),
                        pending: std::sync::Mutex::new(Vec::<Arc<EventPing>>::new()),
                        driver: std::sync::Mutex::new(None),
                    }),
                    armed: RefCell::new(HashMap::<usize, Weak<dyn EventPollable>>::new()),
                });
                event_wake_state_th_.with(|slot| slot.set(Box::into_raw(fresh)));
            }
            let reactor_ptr: *const Reactor = Rc::<Reactor>::as_ptr(&r);
            event_wake_owner_th_.with(|owner| owner.set(reactor_ptr as usize));
            *guard = Some(r);
        }
        guard.as_ref().unwrap().clone()
    })
}

fn reactor_tls_get_disk() -> Rc<Reactor> {
    sp_disk_reactor_th_.with(|slot| {
        let mut guard = slot.borrow_mut();
        if guard.is_none() {
            reactor_log_create(true);
            let r = reactor_make();
            r.thread_id_.set(std::thread::current().id());
            *guard = Some(r);
        }
        guard.as_ref().unwrap().clone()
    })
}

fn reactor_tls_save_running() -> Option<Rc<Fiber>> {
    // Explicit deref: see continue_fiber's old_fiber note.
    sp_running_fiber_th_.with(|slot| (*slot.borrow()).clone())
}

fn reactor_tls_restore_running(old_fiber: Option<Rc<Fiber>>) {
    sp_running_fiber_th_.with(|slot| {
        *slot.borrow_mut() = old_fiber;
    });
}

fn reactor_tls_set_running(fiber: &Rc<Fiber>) {
    sp_running_fiber_th_.with(|slot| {
        *slot.borrow_mut() = Some(fiber.clone());
    });
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
pub fn reactor_spawn_stackless_task_impl(self_: &Reactor, mut task: TaskVoid) {
    reactor_verify(std::thread::current().id() == self_.thread_id_.get());
    // The same executor split as reactor_spawn_stackless_task_with_result.
    if poll_driver_accepts_spawn(self_) {
        stackless_lion_spawn_void(task);
        return;
    }
    let ingress = stackless_wake_ingress::<()>(self_);
    let mut early_binding = stackless_wake_make_binding(ingress);
    let early_ticket = early_binding.ticket.clone();
    let mut ectx = Context::from_waker(&early_binding.waker);
    if task.as_mut().poll(&mut ectx).is_ready() {
        return;
    }

    let ts = StacklessVoidTaskState {
        task: RefCell::<TaskVoid>::new(task),
    };
    let state: Arc<StacklessVoidTaskState> = Arc::new(ts);
    let completion_ticket = early_ticket.clone();
    let poller: StacklessPollFn = Some(Box::new(move |ctx: &mut Context<'_>| -> bool {
        // Scoped so the task borrow is released before the ready-path store.
        let ready: bool = {
            let mut tguard = state.task.borrow_mut();
            (*tguard).as_mut().poll(ctx).is_ready()
        };
        if !ready {
            return false;
        }
        completion_ticket.slot.store(
            STACKLESS_UNREGISTERED_SLOT,
            std::sync::atomic::Ordering::Release,
        );
        true
    }));
    let idx = self_.register_stackless_poller(poller);
    if idx == STACKLESS_UNREGISTERED_SLOT {
        // See reactor_spawn_stackless_task_with_result: the rejected poller and
        // its Task are already destroyed and the cancellation is already
        // recorded.  A suspended Task<void> destroyed here never resumes its
        // continuation, so this must not look like a successful spawn.
        return;
    }
    early_ticket.slot.store(idx, std::sync::atomic::Ordering::Release);
    let ingress_ready = stackless_wake_take_pending::<()>(self_);
    for ready_idx in ingress_ready {
        self_.enqueue_stackless_task(ready_idx);
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

#[allow(unsafe_code)]
fn job_identity(job: &Arc<dyn Job>) -> usize {
    Arc::as_ptr(job) as *const () as usize
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

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn pollable_proxy_mode(p: &PollableProxy) -> i32 {
    let b: &Box<dyn PollableBase> = p;
    b.poll_mode()
}

fn pollthread_create() -> Arc<PollThread> {
    let (sender, receiver) = std::sync::mpsc::channel::<PollCommand>();
    let wake: Arc<PollDriverWake> = Arc::new(PollDriverWake {
        pending: AtomicBool::new(false),
        waker: std::sync::Mutex::new(None),
    });
    let seed = PollThread {
        sender_: sender,
        join_handle_: PollJoinSlot::new(None),
        poll_thread_id_bits_: std::sync::atomic::AtomicU64::new(0),
        shutdown_called_: std::sync::atomic::AtomicBool::new(false),
        remove_count_: AtomicI32::new(0),
        driver_: wake.clone(),
    };
    let arc: Arc<PollThread> = Arc::new(seed);
    // rusty atomic ops are const, so a const* suffices through the Arc.
    let thread_id_address = (&arc.poll_thread_id_bits_ as *const std::sync::atomic::AtomicU64) as usize;
    let handle = crate::threading::spawn_abort_on_panic(move || {
        let tid = current_thread_gettid() as u64;
        let thread_id_ptr = thread_id_address as *const std::sync::atomic::AtomicU64;
        unsafe { (*thread_id_ptr).store(tid, std::sync::atomic::Ordering::Release) };
        pollthread_run(receiver, wake);
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

// ---------------------------------------------------------------------------
// The PollThread driver (S3 of docs/dev/lion-runtime-plan.md)
// ---------------------------------------------------------------------------
//
// A PollThread is one OS thread running one Lion runtime over
// SrpcEpollBackend.  The thread builds the runtime itself, because a Lion
// Runtime is !Send, and blocks in `block_on` on the JoinHandle of the driver
// task until a Shutdown command ends it.  The root future is only that join;
// all of SRPC's work runs in spawned local tasks, which are what Lion's
// scheduler (and its liveness argument) is about:
//
// * The driver (PollDriverTask) does the owner-side work that run_loop does
//   on a thread with no loop.  Woken through PollDriverWake, it drains the
//   command channel, applies deferred removals, runs ready jobs, and calls
//   run_loop(false, true) -- the
//   same drain a thread with no loop runs -- which serves pings, the ready
//   queue and expired deadlines and resumes fibers.  Then it sleeps on a Lion
//   timer until event_next_deadline_us, rounded up to whole milliseconds
//   (Lion's clock), or until woken.  Resumption stays in that drain, never in
//   set(); Fiber::create_run and continue_fiber stay synchronous.
//
// * Each pollable registered with add_proxy gets a task (PollFdTask) that
//   waits on its descriptor through Lion's AsyncFd and calls handle_read and
//   handle_write, as the epoll loop's dispatch did.  This was S3's interim
//   adapter for TCP.  Since S5 the TCP connections and listeners run their
//   own reader, writer and accept tasks (srpc.tcp_channel), and no production
//   pollable remains; S7b kept the adapter because add_proxy is the public
//   extension API for custom pollables, documented in both books.
//
// * Stackless tasks spawned on the thread run as Lion spawn_local tasks (see
//   reactor_spawn_stackless_task_with_result).
//
// What is polled rather than woken: Job::Ready has no wake, so while a job
// waits to become ready the driver re-checks it every millisecond, the rate
// of the old loop.  With no waiting job, an idle PollThread parks until a
// wake, a timer, or Lion's 100 ms idle bound.
//
// Every task on the thread aborts the process if a poll unwinds
// (PollTaskUnwindAbort), which is what the thread's catch-all
// (spawn_abort_on_panic) did before Lion's tasks started catching panics.
//
// The epoll loop this replaced (PollThreadWorker and its pollworker_*
// helpers) was deleted in S7b; the comments below still name the rules it had
// where the driver keeps them.

// The driver of this thread's PollThread, while it runs; null elsewhere.
thread_local! {
    static poll_driver_th_: Cell<*const PollDriver> = const { Cell::new(core::ptr::null()) };
}

// The driver's state.  Owned by the poll thread (Rc), reached from the driver
// task, the transport tasks and, through poll_driver_th_, from the wake hooks
// on this thread.
struct PollDriver {
    wake: Arc<PollDriverWake>,
    receiver: PollCmdReceiver,
    // Jobs waiting to run, in submission order, one entry per job (identity
    // is the Arc address).  The epoll worker's job set ran them in address
    // order; callers rely on submission order (a close job queued before a
    // later job runs first).
    jobs: RefCell<Vec<Arc<dyn Job>>>,
    // One registration per descriptor, as fd_to_pollable_ was.
    fds: RefCell<HashMap<i32, Rc<PollFdEntry>>>,
    // RemovePollable is applied after the command batch that carried it, as
    // it was after each epoll pass.
    pending_remove: RefCell<FdSet>,
    stop: Cell<bool>,
    // Whether stackless spawns go to Lion; false once shutdown has begun.
    accepting: Cell<bool>,
    // Whether the driver task is being polled.  Its own drain covers what the
    // owner-side hooks would wake it for, so they skip the wake then.
    running: Cell<bool>,
    // The Lion timer the driver sleeps on, and the event deadline it was
    // armed for (u64::MAX when none).
    timer: RefCell<Option<lion_reactor::ResourceId>>,
    armed_us: Cell<u64>,
}

// One registered pollable.  The transport task and the driver share it; both
// run on the poll thread and never at once.  `async_fd` is dropped before
// `proxy`: the proxy's descriptor lease keeps the fd open until Lion has
// deregistered it (AsyncFd requires the fd to outlive it).
struct PollFdEntry {
    fd: i32,
    proxy: RefCell<Option<PollableProxy>>,
    async_fd: RefCell<Option<lion_reactor::AsyncFd>>,
    mode: Cell<i32>,
    // The transport task's waker, for mode changes, pending writes and
    // retirement.
    waker: RefCell<Option<Waker>>,
}

// Aborts the process when dropped while still armed, i.e. when a task's poll
// unwinds.  Every poll disarms it before returning.  Public so that tasks
// other modules spawn on the poll thread (S5's TCP transport) abort alike.
pub struct PollTaskUnwindAbort {
    pub armed: bool,
}

impl Drop for PollTaskUnwindAbort {
    fn drop(&mut self) {
        if self.armed {
            reactor_log_line(Log::FATAL, 0i32, core::ptr::null(), "[PollThread] a task on the poll thread panicked; aborting, as the poll thread always has".to_string());
            std::process::abort();
        }
    }
}

// The body of a PollThread's OS thread.
fn pollthread_run(receiver: PollCmdReceiver, wake: Arc<PollDriverWake>) {
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread] starting its Lion runtime".to_string());
    let backend = SrpcEpollBackend::new();
    reactor_verify(backend.is_ok());
    let os_backend: Box<dyn lion_reactor::OsBackend> = Box::new(backend.unwrap());
    let built = lion_executor::RuntimeBuilder::new().os_backend(os_backend).build();
    reactor_verify(built.is_ok());
    let runtime: lion_executor::Runtime = built.unwrap();
    // This thread's Reactor and its event wake state, which the driver drains.
    let reactor: Rc<Reactor> = Reactor::get_reactor();
    let driver: Rc<PollDriver> = Rc::new(PollDriver {
        wake,
        receiver,
        jobs: RefCell::new(Vec::new()),
        fds: RefCell::new(HashMap::<i32, Rc<PollFdEntry>>::new()),
        pending_remove: RefCell::new(FdSet::new()),
        stop: Cell::new(false),
        accepting: Cell::new(true),
        running: Cell::new(false),
        timer: RefCell::new(None),
        armed_us: Cell::new(u64::MAX),
    });
    poll_driver_bind(&driver, &reactor);
    let driver_task = PollDriverTask { driver: driver.clone() };
    let joined = runtime.block_on(runtime.handle().spawn_local(driver_task));
    // Not cancelled (nothing aborts it) and not panicked (its poll aborts).
    reactor_verify(joined.is_ok());
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread] driver stopped, tearing down".to_string());
    // From here on the thread has no loop: stackless spawns and wake hooks go
    // back to the plain Reactor, which this thread's exit tears down.
    poll_driver_unbind(&driver, &reactor);
    // Unregister every pollable without closing it, as the epoll loop's
    // shutdown cleanup did, while the runtime can still deregister them.
    poll_driver_retire_all(&driver);
    // Dropping the runtime drops every task left on it, here on its thread:
    // stackless tasks (counted as cancelled, see StacklessLionTask) and the
    // finished transport tasks.
    drop(runtime);
    drop(driver);
    reactor_log_line(Log::DEBUG, 0i32, core::ptr::null(), "[PollThread] poll thread exiting".to_string());
}

// Make `driver` this thread's driver: the TLS slot the owner-side hooks read,
// and the ping and stackless ingresses other threads reach.
fn poll_driver_bind(driver: &Rc<PollDriver>, reactor: &Reactor) {
    poll_driver_th_.with(|slot| slot.set(Rc::as_ptr(driver)));
    poll_driver_bind_ingresses(reactor, Some(driver.wake.clone()));
}

fn poll_driver_unbind(driver: &PollDriver, reactor: &Reactor) {
    driver.accepting.set(false);
    poll_driver_th_.with(|slot| slot.set(core::ptr::null()));
    poll_driver_bind_ingresses(reactor, None);
    // A late wake has no task to wake.
    let mut slot = driver.wake.waker.lock().unwrap();
    *slot = None;
}

// Point the owner-thread ingresses of `reactor` at `wake` (or at nothing).
fn poll_driver_bind_ingresses(reactor: &Reactor, wake: Option<Arc<PollDriverWake>>) {
    let state_ptr: *mut EventWakeState = event_wake_state_th_.with(|slot| slot.get());
    if !state_ptr.is_null()
        && event_wake_owner_th_.with(|owner| owner.get()) == stackless_wake_reactor_key::<()>(reactor)
    {
        let state: &EventWakeState = unsafe { &*state_ptr };
        let mut guard = state.pings.driver.lock().unwrap();
        *guard = wake.clone();
    }
    let owners_ptr = stackless_wake_owners_existing_ptr::<()>();
    if owners_ptr.is_null() {
        return;
    }
    let key = stackless_wake_reactor_key::<()>(reactor);
    let mut ingress: Option<Arc<StacklessWakeIngress>> = None;
    unsafe {
        let owners = &mut *owners_ptr;
        let mut i: usize = 0usize;
        while i < owners.len() {
            if owners[i].reactor_key == key {
                ingress = owners[i].ingress.as_ref().cloned();
                break;
            }
            i += 1usize;
        }
    }
    if let Some(ingress) = ingress {
        let mut guard = ingress.driver.lock().unwrap();
        *guard = wake;
    }
}

// The wake handle a new stackless ingress of `reactor` binds to: this
// thread's driver, if `reactor` is the Reactor it drains.
fn poll_driver_wake_of(reactor: &Reactor) -> Option<Arc<PollDriverWake>> {
    let local: *const PollDriver = poll_driver_th_.with(|slot| slot.get());
    if local.is_null()
        || event_wake_owner_th_.with(|owner| owner.get()) != stackless_wake_reactor_key::<()>(reactor)
    {
        return None;
    }
    let driver: &PollDriver = unsafe { &*local };
    Some(driver.wake.clone())
}

// Whether a stackless spawn on `reactor` goes to this thread's Lion runtime.
fn poll_driver_accepts_spawn(reactor: &Reactor) -> bool {
    let local: *const PollDriver = poll_driver_th_.with(|slot| slot.get());
    if local.is_null() {
        return false;
    }
    let driver: &PollDriver = unsafe { &*local };
    driver.accepting.get()
        && event_wake_owner_th_.with(|owner| owner.get()) == stackless_wake_reactor_key::<()>(reactor)
}

// Wake this thread's driver from an owner-side edge (the ready queue), unless
// the driver is draining, which serves the edge itself.
fn poll_driver_wake_owner() {
    let local: *const PollDriver = poll_driver_th_.with(|slot| slot.get());
    if local.is_null() {
        return;
    }
    let driver: &PollDriver = unsafe { &*local };
    if !driver.running.get() {
        poll_driver_wake(&driver.wake);
    }
}

// A deadline was pushed on this thread: wake the driver if it sleeps past it.
fn poll_driver_deadline_added(deadline: u64) {
    let local: *const PollDriver = poll_driver_th_.with(|slot| slot.get());
    if local.is_null() {
        return;
    }
    let driver: &PollDriver = unsafe { &*local };
    if !driver.running.get() && deadline < driver.armed_us.get() {
        poll_driver_wake(&driver.wake);
    }
}

// Whether this thread runs the driver that owns `wake`, and that driver still
// accepts work on its runtime.
fn poll_driver_is_current(wake: &Arc<PollDriverWake>) -> bool {
    let local: *const PollDriver = poll_driver_th_.with(|slot| slot.get());
    if local.is_null() {
        return false;
    }
    let driver: &PollDriver = unsafe { &*local };
    driver.accepting.get() && Arc::ptr_eq(&driver.wake, wake)
}

fn poll_fd_entry_wake(entry: &PollFdEntry) {
    let waker: Option<Waker> = {
        let guard = entry.waker.borrow();
        (*guard).clone()
    };
    if let Some(waker) = waker {
        waker.wake();
    }
}

// The driver task.  Unpin, so poll can reach its fields through get_mut.
struct PollDriverTask {
    driver: Rc<PollDriver>,
}

impl Future for PollDriverTask {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut PollDriverTask = self.get_mut();
        let mut unwind = PollTaskUnwindAbort { armed: true };
        this.driver.running.set(true);
        let result: Poll<()> = poll_driver_poll(&this.driver, cx);
        this.driver.running.set(false);
        unwind.armed = false;
        result
    }
}

// One poll of the driver: drain until no wake is pending, then sleep.
fn poll_driver_poll(driver: &Rc<PollDriver>, cx: &mut Context<'_>) -> Poll<()> {
    let reactor: Rc<Reactor> = Reactor::get_reactor();
    loop {
        // Cleared before the drain, so a wake that lands during it is seen
        // below (see PollDriverWake).  A swap, not a store: reading the flag
        // a wake set acquires the work that wake published.
        driver.wake.pending.swap(false, std::sync::atomic::Ordering::AcqRel);
        poll_driver_process_commands(driver);
        poll_driver_apply_removals(driver);
        poll_driver_trigger_jobs(driver);
        (*reactor).run_loop(false, true);
        if driver.stop.get() {
            poll_driver_disarm_timer(driver);
            return Poll::Ready(());
        }
        {
            let mut slot = driver.wake.waker.lock().unwrap();
            *slot = Some(cx.waker().clone());
        }
        if driver.wake.pending.load(std::sync::atomic::Ordering::Acquire) {
            continue;
        }
        if poll_driver_arm_timer(driver, &reactor, cx) {
            return Poll::Pending;
        }
        // The next deadline passed during this drain: serve it now.
    }
}

// Sleep on a Lion timer until the earliest of the next event deadline and,
// while a job waits to become ready, the next job re-check.  Returns false
// when that instant has already passed; the caller drains again.
fn poll_driver_arm_timer(driver: &PollDriver, reactor: &Reactor, cx: &mut Context<'_>) -> bool {
    // Job::Ready has no wake; a waiting job is re-checked this often, the
    // rate of the old poll loop.
    let job_recheck_us: u64 = 1000u64;
    let mut target: u64 = u64::MAX;
    let next: Option<u64> = event_next_deadline_us::<()>(reactor);
    if let Some(deadline) = next {
        target = deadline;
    }
    driver.armed_us.set(target);
    let jobs_waiting: bool = {
        let jobs_guard = driver.jobs.borrow();
        !(*jobs_guard).is_empty()
    };
    if target == u64::MAX && !jobs_waiting {
        poll_driver_disarm_timer(driver);
        return true;
    }
    let now_us: u64 = Time::now(true);
    if jobs_waiting && now_us + job_recheck_us < target {
        target = now_us + job_recheck_us;
    }
    if target <= now_us {
        poll_driver_disarm_timer(driver);
        return false;
    }
    // Lion's clock counts whole milliseconds from its own origin, so the
    // wait is converted as a duration, rounded up.  Lion truncates its clock,
    // so the timer can fire up to a millisecond before `target`; the drain
    // then finds nothing due and the driver sleeps again for the rest.
    // div_ceil lowers to rusty::div_ceil.
    let wait_ms: u64 = (target - now_us).div_ceil(1000u64);
    poll_driver_disarm_timer(driver);
    let deadline: lion_reactor::Instant = lion_reactor::Instant::now() + lion_reactor::Duration::from_millis(wait_ms);
    let waker: lion_reactor::Waker = lion_reactor::Waker::from_std(cx.waker().clone());
    let registered = lion_reactor::ReactorHandle::new().register_timer(deadline, waker);
    match registered {
        lion_reactor::IoResult::Ok(rid) => {
            let mut timer_guard = driver.timer.borrow_mut();
            *timer_guard = Some(rid);
        }
        lion_reactor::IoResult::Err(_) => {
            reactor_verify(false);
        }
    }
    true
}

// Drop the driver's timer, if any.  Deregistration is deferred inside Lion,
// which re-uses the slot when the next timer has the same deadline.
fn poll_driver_disarm_timer(driver: &PollDriver) {
    let previous: Option<lion_reactor::ResourceId> = {
        let mut timer_guard = driver.timer.borrow_mut();
        (*timer_guard).take()
    };
    if let Some(rid) = previous {
        lion_reactor::ReactorHandle::new().deregister_timer(rid);
    }
}

// Drain the command channel, as pollworker_process_commands did each pass.
fn poll_driver_process_commands(driver: &Rc<PollDriver>) {
    loop {
        let result = driver.receiver.try_recv();
        if result.is_err() {
            // Empty or disconnected -- either way, stop draining.
            break;
        }
        let cmd = result.unwrap();
        match cmd {
            PollCommand::AddPollable { pollable } => {
                poll_driver_add(driver, pollable);
            }
            PollCommand::RemovePollable { fd } => {
                let known: bool = {
                    let fds_guard = driver.fds.borrow();
                    (*fds_guard).contains_key(&fd)
                };
                if known {
                    let mut remove_guard = driver.pending_remove.borrow_mut();
                    (*remove_guard).insert(fd);
                }
            }
            PollCommand::ClosePollable { fd } => {
                poll_driver_close(driver, fd);
            }
            PollCommand::UpdateMode { fd, new_mode } => {
                poll_driver_update_mode(driver, fd, new_mode);
            }
            PollCommand::AddJob { job } => {
                poll_driver_add_job(driver, job);
            }
            PollCommand::RemoveJob { job } => {
                let key: usize = job_identity(&job);
                let mut jobs_guard = driver.jobs.borrow_mut();
                (*jobs_guard).retain(move |queued: &Arc<dyn Job>| -> bool { job_identity(queued) != key });
            }
            PollCommand::Shutdown => {
                driver.stop.set(true);
            }
        }
    }
}

// Admit a registration with the rules of pollworker_do_add_pollable: a closed
// or fd-less proxy is dropped; a second registration for a live one is
// dropped; a closed one is retired first.  The new registration gets an
// AsyncFd and its transport task.
// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn poll_driver_add(driver: &Rc<PollDriver>, poll: PollableProxy) {
    let fd = pollable_proxy_fd(&poll);
    let poll_mode = pollable_proxy_mode(&poll);
    let poll_ref: &Box<dyn PollableBase> = &poll;
    if fd < 0 || poll_ref.is_closed() {
        return;
    }
    let existing: Option<Rc<PollFdEntry>> = {
        let fds_guard = driver.fds.borrow();
        (*fds_guard).get(&fd).cloned()
    };
    if let Some(old) = existing {
        let old: Rc<PollFdEntry> = old;
        if !poll_fd_entry_is_closed(&old) {
            return;
        }
        poll_driver_close(driver, fd);
    }
    // A failed registration drops the proxy, which releases its lease.
    let registered = lion_reactor::AsyncFd::new(fd);
    if registered.is_err() {
        return;
    }
    let entry: Rc<PollFdEntry> = Rc::new(PollFdEntry {
        fd,
        proxy: RefCell::new(Some(poll)),
        async_fd: RefCell::new(Some(registered.unwrap())),
        mode: Cell::new(poll_mode),
        waker: RefCell::new(None),
    });
    {
        let mut fds_guard = driver.fds.borrow_mut();
        (*fds_guard).insert(fd, entry.clone());
    }
    // Detached: retirement ends the task by emptying its entry and waking it,
    // and the runtime's drop takes whatever is left at shutdown.
    let task = PollFdTask { driver: driver.clone(), entry };
    let handle: lion_executor::JoinHandle<()> = lion_executor::spawn_local(task);
    drop(handle);
}

// ClosePollable: retire the registration and close its pollable, as
// pollworker_do_close_pollable did, cancelling a queued removal of it.
fn poll_driver_close(driver: &PollDriver, fd: i32) {
    {
        let mut remove_guard = driver.pending_remove.borrow_mut();
        (*remove_guard).remove(&fd);
    }
    let retired: Option<Rc<PollFdEntry>> = {
        let mut fds_guard = driver.fds.borrow_mut();
        (*fds_guard).remove(&fd)
    };
    if let Some(entry) = retired {
        let entry: Rc<PollFdEntry> = entry;
        poll_fd_entry_retire(&entry, true);
    }
}

// UpdateMode: record the mode and let the transport task act on it.
fn poll_driver_update_mode(driver: &PollDriver, fd: i32, new_mode: i32) {
    let entry: Option<Rc<PollFdEntry>> = {
        let fds_guard = driver.fds.borrow();
        (*fds_guard).get(&fd).cloned()
    };
    if let Some(entry) = entry {
        let entry: Rc<PollFdEntry> = entry;
        entry.mode.set(new_mode);
        poll_fd_entry_wake(&entry);
    }
}

// The removals of the command batch just drained.  Unregistered, not closed,
// as pollworker_process_pending_removals did.
fn poll_driver_apply_removals(driver: &PollDriver) {
    // The HashSet port has no drain(); take the set and copy the fds out.
    let taken: FdSet = {
        let mut remove_guard = driver.pending_remove.borrow_mut();
        core::mem::take(&mut *remove_guard)
    };
    let mut fds: Vec<i32> = Vec::new();
    for fd in taken.iter() {
        fds.push(*fd);
    }
    for fd in fds.iter() {
        let retired: Option<Rc<PollFdEntry>> = {
            let mut fds_guard = driver.fds.borrow_mut();
            (*fds_guard).remove(fd)
        };
        if let Some(entry) = retired {
            let entry: Rc<PollFdEntry> = entry;
            poll_fd_entry_retire(&entry, false);
        }
    }
}

// Queue a job unless the same job (by identity) is already queued.  The scan
// is over the jobs waiting on this thread, normally none or a few.
fn poll_driver_add_job(driver: &PollDriver, job: Arc<dyn Job>) {
    let key: usize = job_identity(&job);
    let mut jobs_guard = driver.jobs.borrow_mut();
    let mut i: usize = 0usize;
    while i < (*jobs_guard).len() {
        if job_identity(&(*jobs_guard)[i]) == key {
            return;
        }
        i += 1usize;
    }
    (*jobs_guard).push(job);
}

// Run every ready job in a fiber, in submission order, and keep the rest in
// order, as pollworker_trigger_job did apart from the order.  A job still
// waiting keeps the driver's 1 ms re-check armed.
fn poll_driver_trigger_jobs(driver: &PollDriver) {
    let jobs_exec: Vec<Arc<dyn Job>> = {
        let mut jobs_guard = driver.jobs.borrow_mut();
        core::mem::take(&mut *jobs_guard)
    };
    for job in jobs_exec.iter() {
        if job_ready(job) {
            // Ready jobs ran (or are running) -- do NOT re-add them.
            job_spawn_work(job);
        } else {
            let mut jobs_guard = driver.jobs.borrow_mut();
            (*jobs_guard).push(job.clone());
        }
    }
}

// Shutdown: unregister every pollable without closing it, and drop the jobs
// and the timer.  Runs with the runtime still alive.
fn poll_driver_retire_all(driver: &PollDriver) {
    let taken: HashMap<i32, Rc<PollFdEntry>> = {
        let mut fds_guard = driver.fds.borrow_mut();
        core::mem::take(&mut *fds_guard)
    };
    let mut entries: Vec<Rc<PollFdEntry>> = Vec::new();
    for entry in taken.values() {
        entries.push(entry.clone());
    }
    drop(taken);
    for entry in entries.iter() {
        poll_fd_entry_retire(entry, false);
    }
    {
        let mut remove_guard = driver.pending_remove.borrow_mut();
        (*remove_guard).clear();
    }
    {
        let mut jobs_guard = driver.jobs.borrow_mut();
        (*jobs_guard).clear();
    }
    poll_driver_disarm_timer(driver);
}

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn poll_fd_entry_is_closed(entry: &PollFdEntry) -> bool {
    let guard = entry.proxy.borrow();
    match (*guard).as_ref() {
        Some(p) => {
            let p: &Box<dyn PollableBase> = p;
            p.is_closed()
        }
        None => true,
    }
}

// Retire a registration: deregister its descriptor from Lion while the
// proxy's lease still holds it open, optionally close the pollable (the
// epoll loop's order: unregister, then close, then release the lease), and
// wake the transport task, which then finds the entry empty and finishes.
fn poll_fd_entry_retire(entry: &PollFdEntry, close: bool) {
    let async_fd: Option<lion_reactor::AsyncFd> = {
        let mut fd_guard = entry.async_fd.borrow_mut();
        (*fd_guard).take()
    };
    drop(async_fd);
    let proxy: Option<PollableProxy> = {
        let mut proxy_guard = entry.proxy.borrow_mut();
        (*proxy_guard).take()
    };
    if let Some(mut poll) = proxy {
        if close {
            let poll_ref: &mut Box<dyn PollableBase> = &mut poll;
            poll_ref.close();
        }
        drop(poll);
    }
    let waker: Option<Waker> = {
        let mut waker_guard = entry.waker.borrow_mut();
        (*waker_guard).take()
    };
    if let Some(waker) = waker {
        waker.wake();
    }
}

// The task of one add_proxy registration (the pollable adapter).  Unpin, so
// poll can reach its fields through get_mut.
struct PollFdTask {
    driver: Rc<PollDriver>,
    entry: Rc<PollFdEntry>,
}

impl Future for PollFdTask {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut PollFdTask = self.get_mut();
        let mut unwind = PollTaskUnwindAbort { armed: true };
        let result: Poll<()> = poll_fd_task_poll(&this.driver, &this.entry, cx);
        unwind.armed = false;
        result
    }
}

// One poll of a transport task.  It keeps the epoll loop's edge-triggered
// dispatch: one handle_read per read edge, and handle_write while the mode
// asks for writes and the socket takes them.
//
// Readiness is Lion's AsyncFd flag per direction, which only a park sets and
// only an observed WouldBlock clears.  A pollable drains internally and does
// not report EAGAIN, so the adapter maps its results onto the flags, under
// the EPOLLET contract the epoll loop gave pollables:
// * A read edge is consumed before handle_read runs.  A pollable must read
//   until EAGAIN or a short read (accept until EAGAIN), so data that arrives
//   after its read raises a new edge.
// * The write flag is consumed only when handle_write returns NO_CHANGE,
//   which a pollable returns only after send(2) hit EAGAIN (TCP's retired
//   pollable did).  Any other mode means the socket is still writable, so
//   the flag stays set and the next pending write goes out at once.
// ERR and HUP wake both directions in Lion, so a failed or hung-up socket is
// seen by handle_read (recv fails or reads EOF) rather than handle_error,
// which the adapter does not call.
// The pending-write latch is read here after every handle_read (a fast
// handler's reply is written in the same poll) and whenever the task is woken
// for another reason.  Nothing wakes the task for the latch alone: a pollable
// that wants write interest from another thread asks for it with
// PollThread::update_mode.  (The epoll loop swept every registration's latch
// each pass; S3's notify_pending_write replaced that sweep for TCP, and went
// with TCP's pollable surface in S7b.)  A pollable found closed is retired and
// closed, as that loop's closed sweep did.
fn poll_fd_task_poll(driver: &PollDriver, entry: &Rc<PollFdEntry>, cx: &mut Context<'_>) -> Poll<()> {
    // Publish the waker before reading any state it may be woken for.
    {
        let mut waker_guard = entry.waker.borrow_mut();
        *waker_guard = Some(cx.waker().clone());
    }
    let retired: bool = {
        let proxy_guard = entry.proxy.borrow();
        (*proxy_guard).is_none()
    };
    if retired {
        let mut waker_guard = entry.waker.borrow_mut();
        *waker_guard = None;
        return Poll::Ready(());
    }
    if (entry.mode.get() & PollMode::READ) != 0 && poll_fd_take_ready(entry, cx, false) {
        poll_fd_entry_handle_read(entry);
    }
    if poll_fd_entry_is_closed(entry) {
        poll_fd_task_retire(driver, entry);
        return Poll::Ready(());
    }
    if poll_fd_entry_latched(entry) {
        entry.mode.set(PollMode::READ | PollMode::WRITE);
    }
    if (entry.mode.get() & PollMode::WRITE) != 0 && poll_fd_is_ready(entry, cx, true) {
        let new_mode: i32 = poll_fd_entry_handle_write(entry);
        if new_mode == PollMode::NO_CHANGE {
            let _consumed: bool = poll_fd_take_ready(entry, cx, true);
        } else {
            entry.mode.set(new_mode);
        }
    }
    if poll_fd_entry_is_closed(entry) {
        poll_fd_task_retire(driver, entry);
        return Poll::Ready(());
    }
    // Wait for the directions the mode asks for.  A direction consumed above
    // registers the waker now; one still flagged (the mode changed under the
    // task) is served on another poll.
    let mut again: bool = false;
    if (entry.mode.get() & PollMode::READ) != 0 && poll_fd_is_ready(entry, cx, false) {
        again = true;
    }
    if (entry.mode.get() & PollMode::WRITE) != 0 && poll_fd_is_ready(entry, cx, true) {
        again = true;
    }
    if again {
        cx.waker().wake_by_ref();
    }
    Poll::Pending
}

// Whether the registration's descriptor is ready in one direction.  When it is
// not, `cx`'s waker is registered for that direction's next edge.
fn poll_fd_is_ready(entry: &PollFdEntry, cx: &mut Context<'_>, write: bool) -> bool {
    let fd_guard = entry.async_fd.borrow();
    if (*fd_guard).is_none() {
        return false;
    }
    let async_fd: &lion_reactor::AsyncFd = (*fd_guard).as_ref().unwrap();
    lion_fd_poll_ready(async_fd, cx, write)
}

// Consume one direction's readiness (see lion_fd_consume_ready).
fn poll_fd_take_ready(entry: &PollFdEntry, cx: &mut Context<'_>, write: bool) -> bool {
    let fd_guard = entry.async_fd.borrow();
    if (*fd_guard).is_none() {
        return false;
    }
    let async_fd: &lion_reactor::AsyncFd = (*fd_guard).as_ref().unwrap();
    lion_fd_consume_ready(async_fd, cx, write)
}

/// Whether `async_fd` may be ready in one direction (read, or write when
/// `write`).  When it is not, `cx`'s waker is registered for that
/// direction's next edge and false is returned.  Call on the poll thread that
/// registered the descriptor.
///
/// A true result obliges the caller to act: perform the operation, and when
/// it reports EAGAIN, call `lion_fd_consume_ready` in the same poll; or wake
/// its own task before returning Pending.  Otherwise no waker is registered
/// and nothing wakes the task (the AsyncFd protocol of U8).
pub fn lion_fd_poll_ready(async_fd: &lion_reactor::AsyncFd, cx: &mut Context<'_>, write: bool) -> bool {
    let polled = if write {
        async_fd.poll_write_ready(cx)
    } else {
        async_fd.poll_read_ready(cx)
    };
    match polled {
        Poll::Ready(Ok(_ready)) => true,
        Poll::Ready(Err(_misuse)) => {
            // Only an AsyncFd polled off its reactor's thread fails here.
            reactor_verify(false);
            false
        }
        Poll::Pending => false,
    }
}

/// Consume one direction's readiness: true if it was ready, and the flag is
/// then clear until the next edge.  The guard's try_io is the only way to
/// clear it, and clears only on WouldBlock, which the empty operation reports.
///
/// Call it only right after the caller's own operation on the descriptor
/// returned EAGAIN, in the same poll: no park lies between that EAGAIN and
/// the clear, so the clear consumes exactly the readiness the EAGAIN
/// disproved (U8 invariant 2), and the next transition raises a new edge.
/// (The pollable adapter also calls it before handle_read, whose pollables
/// drain to EAGAIN or a short read under EPOLLET rules.)
pub fn lion_fd_consume_ready(async_fd: &lion_reactor::AsyncFd, cx: &mut Context<'_>, write: bool) -> bool {
    let polled = if write {
        async_fd.poll_write_ready(cx)
    } else {
        async_fd.poll_read_ready(cx)
    };
    match polled {
        Poll::Ready(Ok(ready)) => {
            let _cleared = ready.try_io(|_fd: lion_reactor::RawFd| -> std::io::Result<()> {
                Err(std::io::Error::from(std::io::ErrorKind::WouldBlock))
            });
            true
        }
        Poll::Ready(Err(_misuse)) => {
            reactor_verify(false);
            false
        }
        Poll::Pending => false,
    }
}

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn poll_fd_entry_handle_read(entry: &PollFdEntry) {
    // Held across the callout: only this task and the driver touch the proxy,
    // and the driver never runs inside a transport task's poll.
    let mut proxy_guard = entry.proxy.borrow_mut();
    if let Some(p) = (*proxy_guard).as_mut() {
        let p: &mut Box<dyn PollableBase> = p;
        p.handle_read();
    }
}

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn poll_fd_entry_handle_write(entry: &PollFdEntry) -> i32 {
    let mut proxy_guard = entry.proxy.borrow_mut();
    if let Some(p) = (*proxy_guard).as_mut() {
        let p: &mut Box<dyn PollableBase> = p;
        return p.handle_write();
    }
    PollMode::NO_CHANGE
}

// MEASURED allow — see the `borrowed_box` note on `pollable_proxy_fd`.
#[allow(clippy::borrowed_box)]
fn poll_fd_entry_latched(entry: &PollFdEntry) -> bool {
    let proxy_guard = entry.proxy.borrow();
    if let Some(p) = (*proxy_guard).as_ref() {
        let p: &Box<dyn PollableBase> = p;
        return p.check_pending_write_update();
    }
    false
}

// A transport task found its pollable closed: leave the driver's map (unless
// a replacement already took the descriptor) with any queued removal, and
// retire it with a close, as the epoll loop's closed sweep did.
fn poll_fd_task_retire(driver: &PollDriver, entry: &Rc<PollFdEntry>) {
    let current: Option<Rc<PollFdEntry>> = {
        let fds_guard = driver.fds.borrow();
        (*fds_guard).get(&entry.fd).cloned()
    };
    if let Some(current) = current {
        let current: Rc<PollFdEntry> = current;
        if Rc::ptr_eq(&current, entry) {
            {
                let mut fds_guard = driver.fds.borrow_mut();
                (*fds_guard).remove(&entry.fd);
            }
            let mut remove_guard = driver.pending_remove.borrow_mut();
            (*remove_guard).remove(&entry.fd);
        }
    }
    {
        // The task finishes in this poll; nothing needs to wake it.
        let mut waker_guard = entry.waker.borrow_mut();
        *waker_guard = None;
    }
    poll_fd_entry_retire(entry, true);
}

// ---------------------------------------------------------------------------
// Stackless tasks on a PollThread's Lion runtime (S3)
// ---------------------------------------------------------------------------
//
// The first poll runs inside the spawn call, as on the Reactor's own
// executor, so a task that completes at once delivers inline and never
// becomes a Lion task.  The waker of that first poll is a forwarder
// (StacklessLionWake): the future may keep it, so it wakes whatever Lion task
// the future became, from any thread.  Each later poll re-points it at that
// poll's Lion waker before polling.
//
// Teardown owes waiters an error (W2) here too: a task the runtime drops
// before it completes -- when its PollThread shuts down -- is counted in
// g_stackless_cancel.teardown_tasks and logged at ERROR, and its future and
// completion callback are destroyed then, on the poll thread.

struct StacklessLionWake {
    target: std::sync::Mutex<Option<Waker>>,
}

impl Wake for StacklessLionWake {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        let target: Option<Waker> = {
            let guard = self.target.lock().unwrap();
            (*guard).clone()
        };
        if let Some(waker) = target {
            waker.wake();
        }
    }
}

fn stackless_lion_forward_to(forward: &StacklessLionWake, waker: Option<Waker>) {
    let mut guard = forward.target.lock().unwrap();
    *guard = waker;
}

// A task with a completion callback.  `on_ready` is boxed so the task is
// Unpin whatever the callback captures.
struct StacklessLionTask<T, OnReady> {
    task: Pin<Box<dyn Future<Output = T>>>,
    on_ready: Option<Box<OnReady>>,
    forward: Arc<StacklessLionWake>,
    done: bool,
}

impl<T: 'static, OnReady: FnMut(T) + 'static> Future for StacklessLionTask<T, OnReady> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut StacklessLionTask<T, OnReady> = self.get_mut();
        let mut unwind = PollTaskUnwindAbort { armed: true };
        stackless_lion_forward_to(&this.forward, Some(cx.waker().clone()));
        let polled: Poll<T> = this.task.as_mut().poll(cx);
        let mut result: Poll<()> = Poll::Pending;
        if let Poll::Ready(value) = polled {
            this.done = true;
            let callback: Option<Box<OnReady>> = this.on_ready.take();
            if let Some(mut f) = callback {
                (*f)(value);
            }
            result = Poll::Ready(());
        }
        unwind.armed = false;
        result
    }
}

impl<T, OnReady> Drop for StacklessLionTask<T, OnReady> {
    fn drop(&mut self) {
        stackless_lion_forward_to(&self.forward, None);
        if !self.done {
            stackless_lion_note_cancelled();
        }
    }
}

// A task without a completion value.
struct StacklessLionVoidTask {
    task: TaskVoid,
    forward: Arc<StacklessLionWake>,
    done: bool,
}

impl Future for StacklessLionVoidTask {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut StacklessLionVoidTask = self.get_mut();
        let mut unwind = PollTaskUnwindAbort { armed: true };
        stackless_lion_forward_to(&this.forward, Some(cx.waker().clone()));
        let ready: bool = this.task.as_mut().poll(cx).is_ready();
        if ready {
            this.done = true;
        }
        unwind.armed = false;
        if ready {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }
}

impl Drop for StacklessLionVoidTask {
    fn drop(&mut self) {
        stackless_lion_forward_to(&self.forward, None);
        if !self.done {
            stackless_lion_note_cancelled();
        }
    }
}

fn stackless_lion_note_cancelled() {
    g_stackless_cancel.teardown_tasks.fetch_add(1u64, std::sync::atomic::Ordering::Relaxed);
    reactor_log_line(Log::ERROR, 0i32, core::ptr::null(), "[PollThread] cancelling an outstanding stackless task at shutdown; its callback and captures are destroyed now, so waiters are released with an error instead of blocking forever".to_string());
}

fn stackless_lion_spawn_with_result<T: 'static, OnReady>(mut task: Pin<Box<dyn Future<Output = T>>>, mut on_ready: OnReady)
where
    OnReady: FnMut(T) + 'static,
{
    let forward: Arc<StacklessLionWake> = Arc::new(StacklessLionWake {
        target: std::sync::Mutex::new(None),
    });
    let early_waker: Waker = Waker::from(forward.clone());
    let mut ectx = Context::from_waker(&early_waker);
    if let Poll::Ready(value) = task.as_mut().poll(&mut ectx) {
        on_ready(value);
        return;
    }
    let lion_task = StacklessLionTask {
        task,
        on_ready: Some(Box::new(on_ready)),
        forward,
        done: false,
    };
    // Detached: the runtime owns the task until it completes or is dropped.
    let handle: lion_executor::JoinHandle<()> = lion_executor::spawn_local(lion_task);
    drop(handle);
}

fn stackless_lion_spawn_void(mut task: TaskVoid) {
    let forward: Arc<StacklessLionWake> = Arc::new(StacklessLionWake {
        target: std::sync::Mutex::new(None),
    });
    let early_waker: Waker = Waker::from(forward.clone());
    let mut ectx = Context::from_waker(&early_waker);
    if task.as_mut().poll(&mut ectx).is_ready() {
        return;
    }
    let lion_task = StacklessLionVoidTask {
        task,
        forward,
        done: false,
    };
    let handle: lion_executor::JoinHandle<()> = lion_executor::spawn_local(lion_task);
    drop(handle);
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
    reactor_verify(f.is_some());
    f.as_mut().unwrap()(y);
}

#[cfg_attr(any(), cpp_namespace(::janus))]
// MEASURED allow — see the `arc_with_non_send_sync` note on `never_event_make`.
#[allow(clippy::arc_with_non_send_sync)]
pub fn quorum_event_make(n_total: i32, quorum: i32) -> Arc<QuorumEvent> {
    let sp = Arc::new(QuorumEvent {
        status_: Cell::new(EventStatus::INIT),
        owner_thread_: std::thread::current().id(),
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
        let dangling: QuorumDangling = (*it.0, *it.1);
        v.push(dangling);
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
            let _ret = finalize_func.as_mut().unwrap()(dr);
            // Historical drain guard. This was added when run_loop kept
            // TIMEOUT events in its queues forever; run_loop now evicts
            // them itself. The DONE mark stays because callers can observe
            // finalize_event_'s final status. We run on the owner thread.
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

#[cfg(test)]
#[path = "../tests/helpers/event_wake_state.rs"]
mod event_wake_state_tests;
