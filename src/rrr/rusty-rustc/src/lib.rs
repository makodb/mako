#![deny(unsafe_code)]

//! Rust-only facades for APIs supplied by the rusty-cpp C++ runtime.
//!
//! The `rrr` crate uses this package for direct rustc checking and tests. The
//! rusty-cpp crate emitter recognizes this exact local package identity and
//! omits it from generated C++ because the production definitions already
//! live in the rusty runtime headers.

pub use ::std::boxed::Box;
pub use ::std::cell::{Cell, RefCell, RefMut};
pub use ::std::collections::{VecDeque};
use ::std::marker::PhantomData;
use ::std::ops::{Deref, DerefMut, Index};
pub use ::std::option::Option;
pub use ::std::option::Option::{None, Some};
pub use ::std::rc::Rc;
use ::std::time::Duration;
pub use ::std::vec::Vec;

pub use rusty_cpp_markers::cpp_inherit;

/// Rust-only callback-wrapper spelling for canonical cross-module facades.
pub struct CallbackWrapper<F> {
    inner: Option<::std::sync::Arc<F>>,
}

impl<F> CallbackWrapper<F> {
    pub fn from_callable(callable: F) -> Self {
        Self {
            inner: Some(::std::sync::Arc::new(callable)),
        }
    }

    pub fn has_value(&self) -> bool {
        self.inner.is_some()
    }

    pub fn callable(&self) -> &F {
        self.inner.as_deref().unwrap()
    }
}

impl<F> Clone for CallbackWrapper<F> {
    fn clone(&self) -> Self {
        Self {
            inner: self.inner.clone(),
        }
    }
}

impl<F> Default for CallbackWrapper<F> {
    fn default() -> Self {
        Self { inner: None }
    }
}

/// Opaque rustc-only models of the native pthread types used by the
/// canonical threading wrapper. The checked C++ type map restores the native
/// typedef spellings; canonical Rust only passes pointers to these values.
#[repr(C)]
pub struct PthreadSpinlock {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct PthreadMutex {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct PthreadMutexAttr {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct PthreadCond {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct PthreadCondAttr {
    _opaque: [u8; 0],
}

pub mod sync {
    pub use ::std::sync::{Arc, Weak};

    pub fn downgrade<T>(arc: Arc<T>) -> Weak<T> {
        Arc::downgrade(&arc)
    }

    pub mod atomic {
        pub use ::std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
    }

    pub mod mpsc {
        pub use ::std::sync::mpsc::{channel, Receiver, Sender};
    }
}

pub mod marker {
    pub use ::std::marker::PhantomPinned;
}

pub mod rc {
    pub use ::std::rc::{Rc, Weak};
}

pub mod port {
    pub mod rc {
        pub use ::std::rc::Rc;
    }
}

/// Rustc-only model of `rusty::thread`.
///
/// The production facade (`third-party/rusty-cpp/include/rusty/thread.hpp`)
/// declares `auto spawn(F&& func, Args&&... args)` -- variadic -- and its
/// `JoinHandle<T>` carries BOTH `join()` and `detach()`.  Rust has no variadic
/// functions, so this model is the single-callable form, and every canonical
/// caller spells that form.
///
/// The model type-checks the body and drops it rather than running it, and it
/// deliberately does NOT require `Send`: the runtime's shared state is reached
/// through `rusty::Cell`/`RefCell` handles that are `!Sync` in the rustc model
/// but are the production C++ types the reactor already shares across its own
/// threads.  Demanding `Send` here would reject the canonical client's
/// connection bodies without describing anything true about the emitted C++.
pub mod thread {
    use ::std::marker::PhantomData;

    #[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
    #[repr(transparent)]
    pub struct ThreadId(pub u64);

    impl ThreadId {
        pub fn as_native(self) -> u64 {
            self.0
        }
    }

    /// Model of the runtime's spawned-thread handle.
    ///
    /// Generic in the body's result so `JoinHandle<()>` -- the payload of the
    /// reactor's `PollJoinSlot` -- names the same type production `spawn`
    /// deduces for a void body.  `PhantomData<fn() -> T>` keeps the handle
    /// unconditionally `Send + Sync`, which `PollThread`'s own
    /// `assert_send_sync` requires.
    pub struct JoinHandle<T>(PhantomData<fn() -> T>);

    impl<T> JoinHandle<T> {
        pub fn join(self) {}
        pub fn detach(self) {}
    }

    pub fn current_id() -> ThreadId {
        // The production facade preserves the platform-native id.  Direct
        // rustc needs only stable equality within one execution, so derive a
        // deterministic numeric token from ThreadId's Debug representation.
        let text = ::std::format!("{:?}", ::std::thread::current().id());
        let value = text
            .trim_start_matches("ThreadId(")
            .trim_end_matches(')')
            .parse::<u64>()
            .unwrap_or(0);
        ThreadId(value)
    }

    /// Production C++ resolves this to the runtime's thread spawn.
    pub fn spawn<F, R>(body: F) -> JoinHandle<R>
    where
        F: FnOnce() -> R + 'static,
    {
        drop(body);
        JoinHandle(PhantomData)
    }
}

pub struct Waker {
    // The production `rusty::Waker` stores a copyable `std::function` and its
    // `wake()` member is const.  Model that contract directly so a retained
    // waker may be invoked concurrently without an `FnMut` aliasing hole.
    pub wake_fn: Box<dyn Fn() + Send + Sync>,
}

impl Waker {
    pub fn wake(&self) {
        (self.wake_fn)();
    }
}

pub struct Context {
    pub waker: *mut Waker,
}

/// Rustc-side x86-64 layout model for the plain-C fiber context.
#[repr(C)]
pub struct ReactorFiberContext {
    pub rsp: *mut core::ffi::c_void,
    pub rip: *mut core::ffi::c_void,
    pub rbx: usize,
    pub rbp: usize,
    pub r12: usize,
    pub r13: usize,
    pub r14: usize,
    pub r15: usize,
}

/// Rustc-side layout model for `::srpc_fiber` from `reactor/srpc_fiber.h`.
#[repr(C)]
pub struct ReactorFiberState {
    pub caller_ctx: ReactorFiberContext,
    pub fiber_ctx: ReactorFiberContext,
    pub stack_mapping: *mut core::ffi::c_void,
    pub stack_mapping_bytes: usize,
    pub state: i32,
    pub entry_fn: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub entry_arg: *mut core::ffi::c_void,
}

pub struct Poll<T> {
    pub ready: bool,
    pub value: T,
}

impl<T> Poll<T> {
    pub fn ready_with(value: T) -> Self {
        Self { ready: true, value }
    }

    pub fn is_ready(&self) -> bool {
        self.ready
    }

    pub fn is_pending(&self) -> bool {
        !self.ready
    }
}

impl<T: Default> Poll<T> {
    pub fn pending() -> Self {
        Self {
            ready: false,
            value: T::default(),
        }
    }
}

/// The boxed poll closure a `Task<T>` drives.  Named so the signature reads
/// once here rather than at every use.
pub type TaskPoller<T> = Box<dyn FnMut(&mut Context) -> Poll<T>>;

pub struct Task<T> {
    poller: TaskPoller<T>,
}

/// Rustc-only storage model for the reactor's `std::set<Arc<Job>>` slot.
///
/// The production type map lowers `ReactorJobSet<T>` to `std::set<T>`; this
/// vector-backed model preserves the set's pointer-identity semantics for
/// direct Rust checking without requiring `dyn Job: Ord`.
pub struct ReactorJobSet<T> {
    entries: Vec<T>,
}

pub trait ReactorJobSetKey {
    fn same_identity(&self, other: &Self) -> bool;
}

impl<T: ?Sized> ReactorJobSetKey for ::std::sync::Arc<T> {
    fn same_identity(&self, other: &Self) -> bool {
        ::std::sync::Arc::ptr_eq(self, other)
    }
}

impl<T> Default for ReactorJobSet<T> {
    fn default() -> Self {
        Self { entries: Vec::new() }
    }
}

impl<T: ReactorJobSetKey> ReactorJobSet<T> {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn insert(&mut self, value: T) {
        if !self
            .entries
            .iter()
            .any(|existing| existing.same_identity(&value))
        {
            self.entries.push(value);
        }
    }

    pub fn erase(&mut self, value: T) {
        self.entries
            .retain(|existing| !existing.same_identity(&value));
    }

    pub fn iter(&self) -> ::std::slice::Iter<'_, T> {
        self.entries.iter()
    }
}

impl<T> Task<T> {
    pub fn from_poller<F>(poller: F) -> Self
    where
        F: FnMut(&mut Context) -> Poll<T> + 'static,
    {
        Self {
            poller: Box::new(poller),
        }
    }

    pub fn poll(&mut self, context: &mut Context) -> Poll<T> {
        (self.poller)(context)
    }
}

thread_local! {
    static REACTOR_CURRENT_FIBER: RefCell<Option<Rc<ReactorFiber>>> = const { RefCell::new(None) };
    static REACTOR_SLEEP_CALLS: RefCell<Vec<u64>> = const { RefCell::new(Vec::new()) };
}

/// Rust-only model of the `rrr.reactor` module's `Fiber` class.
///
/// The checked type map restores the existing `rrr::Fiber` spelling for C++;
/// this state exists only for direct rustc tests of `rrr.fiber`.
pub struct ReactorFiber {
    pub id: Cell<u64>,
    yields: Cell<u64>,
}

pub type ReactorIntEvent = rrr::reactor::IntEvent;
pub type ReactorPollThread = rrr::reactor::PollThread;
pub type RustcSocketAddrV4 = ::std::net::SocketAddrV4;
pub type RustcIoErrorKind = ::std::io::ErrorKind;

#[derive(Debug)]
pub struct RustcIoError {
    inner: ::std::io::Error,
}

impl RustcIoError {
    pub fn kind(&self) -> RustcIoErrorKind {
        self.inner.kind()
    }

    pub fn what(&self) -> String {
        self.inner.to_string()
    }
}

impl From<::std::io::Error> for RustcIoError {
    fn from(inner: ::std::io::Error) -> Self {
        Self { inner }
    }
}

#[derive(Debug)]
pub struct RustcTcpStream {
    inner: ::std::net::TcpStream,
}

impl RustcTcpStream {
    pub fn set_nonblocking(&self, value: bool) -> Result<(), RustcIoError> {
        self.inner.set_nonblocking(value).map_err(Into::into)
    }

    pub fn into_owned_fd(self) -> ::std::os::fd::OwnedFd {
        self.inner.into()
    }
}

#[derive(Debug, Default)]
pub struct RustcTcpListener {
    inner: Option<::std::net::TcpListener>,
}

/// Rustc-only borrowed descriptor view for a possibly-unbound TCP listener.
///
/// Production `rusty::net::TcpListener` stores an invalid/default
/// `rusty::os::fd::OwnedFd`, whose borrowed view reports `-1`. Rust's standard
/// `BorrowedFd` cannot represent that state, so the facade uses this tiny view
/// to preserve the production API's pre-bind behavior.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RustcBorrowedFd {
    raw: i32,
}

impl RustcBorrowedFd {
    pub fn as_raw_fd(&self) -> i32 {
        self.raw
    }
}

impl RustcTcpListener {
    pub fn bind(address: RustcSocketAddrV4) -> Result<Self, RustcIoError> {
        ::std::net::TcpListener::bind(address)
            .map(|inner| Self { inner: Some(inner) })
            .map_err(Into::into)
    }

    pub fn set_nonblocking(&self, value: bool) -> Result<(), RustcIoError> {
        self.inner
            .as_ref()
            .unwrap()
            .set_nonblocking(value)
            .map_err(Into::into)
    }

    pub fn local_addr(&self) -> Result<RustcSocketAddrV4, RustcIoError> {
        let address = self
            .inner
            .as_ref()
            .unwrap()
            .local_addr()
            .map_err(RustcIoError::from)?;
        match address {
            ::std::net::SocketAddr::V4(address) => Ok(address),
            _ => Err(RustcIoError::from(::std::io::Error::new(
                ::std::io::ErrorKind::InvalidInput,
                "not IPv4",
            ))),
        }
    }

    pub fn accept(&self) -> Result<(RustcTcpStream, RustcSocketAddrV4), RustcIoError> {
        let (stream, address) = self
            .inner
            .as_ref()
            .unwrap()
            .accept()
            .map_err(RustcIoError::from)?;
        match address {
            ::std::net::SocketAddr::V4(address) => Ok((RustcTcpStream { inner: stream }, address)),
            _ => Err(RustcIoError::from(::std::io::Error::new(
                ::std::io::ErrorKind::InvalidInput,
                "not IPv4",
            ))),
        }
    }

    pub fn is_bound(&self) -> bool {
        self.inner.is_some()
    }

    pub fn as_raw_fd(&self) -> i32 {
        use ::std::os::fd::AsRawFd;
        self.inner.as_ref().map_or(-1, AsRawFd::as_raw_fd)
    }

    pub fn as_owned_fd(&self) -> RustcBorrowedFd {
        RustcBorrowedFd {
            raw: self.as_raw_fd(),
        }
    }
}

/// Rustc-only ownership model for the production `rusty::os::fd::OwnedFd`.
///
/// The runtime wrapper has an invalid/default state; `std::os::fd::OwnedFd`
/// deliberately does not, so the facade represents that state with `Option`.
#[derive(Debug, Default)]
pub struct RustcOwnedFd {
    inner: Option<::std::os::fd::OwnedFd>,
}

impl RustcOwnedFd {
    /// # Safety
    ///
    /// `fd` must be a live descriptor whose unique ownership is transferred
    /// to the returned value.
    #[allow(unsafe_code)]
    pub unsafe fn from_raw_fd(fd: i32) -> Self {
        use ::std::os::fd::FromRawFd;
        Self {
            // SAFETY: this facade has the same ownership precondition.
            inner: Some(unsafe { ::std::os::fd::OwnedFd::from_raw_fd(fd) }),
        }
    }

    pub fn as_raw_fd(&self) -> i32 {
        use ::std::os::fd::AsRawFd;
        self.inner.as_ref().map_or(-1, AsRawFd::as_raw_fd)
    }

    pub fn is_valid(&self) -> bool {
        self.inner.is_some()
    }
}

impl ReactorFiber {
    /// # Safety
    ///
    /// Reading the reactor's thread-local handle has no caller precondition.
    #[allow(unsafe_code)]
    pub unsafe fn current_fiber() -> Option<Rc<ReactorFiber>> {
        REACTOR_CURRENT_FIBER.with(|slot| slot.borrow().clone())
    }

    /// # Safety
    ///
    /// The receiver must be a live reactor fiber.
    #[allow(unsafe_code)]
    pub unsafe fn yield_(&self) {
        self.yields.set(self.yields.get().wrapping_add(1));
    }

    /// Model of the `rrr::Fiber::create_run_impl` static. Production C++
    /// resolves it to the reactor carrier's own definition, which heap-
    /// allocates the task and schedules it; the model runs nothing so a
    /// direct-rustc check never starts a fiber.
    ///
    /// # Safety
    ///
    /// `file` must be null or a valid NUL-terminated path; `unsafe` otherwise
    /// records the foreign named-module boundary.
    #[allow(unsafe_code)]
    pub unsafe fn create_run_impl<F>(_func: F, _file: *const i8, _line: i64) -> Option<Rc<ReactorFiber>>
    where
        F: FnMut() + 'static,
    {
        None
    }
}

/// Rust-only model of the `rrr.reactor` module's `BoxEvent<T>` template.
///
/// Crate-mode C++ generation maps this type back to the existing
/// `rrr::BoxEvent<T>` definition. The synchronization state exists only so
/// direct rustc tests can exercise one-shot set/wait/get behavior; it is never
/// emitted into production C++.
pub struct ReactorBoxEvent<T> {
    pub is_set_: Cell<bool>,
    value: Mutex<Option<T>>,
    ready: ::std::sync::Condvar,
    not_thread_safe: PhantomData<Rc<()>>,
}

/// Rust-only conversion used by [`ReactorBoxEvent::set`] so canonical source
/// can pass either an owned value or the borrowed value accepted by C++.
pub trait ReactorSetValue<T> {
    fn into_owned(self) -> T;
}

impl<T> ReactorSetValue<T> for T {
    fn into_owned(self) -> T {
        self
    }
}

impl<T> ReactorSetValue<T> for &T
where
    T: Clone,
{
    fn into_owned(self) -> T {
        self.clone()
    }
}

impl<T> ReactorBoxEvent<T> {
    pub fn new() -> ReactorBoxEvent<T> {
        ReactorBoxEvent {
            is_set_: Cell::new(false),
            value: Mutex::new(None),
            ready: ::std::sync::Condvar::new(),
            not_thread_safe: PhantomData,
        }
    }

    pub fn is_ready(&self) -> bool {
        self.is_set_.get()
    }

    pub fn set<V>(&self, value: V)
    where
        V: ReactorSetValue<T>,
    {
        *self.value.lock().unwrap() = Some(value.into_owned());
        self.is_set_.set(true);
        self.ready.notify_all();
    }

    pub fn wait(&self) {
        let mut value = self.value.lock().unwrap();
        while value.is_none() {
            value = self.ready.wait(value).unwrap();
        }
    }

    pub fn wait_timeout(&self, timeout_us: u64) {
        if timeout_us == 0 {
            self.wait();
            return;
        }
        let value = self.value.lock().unwrap();
        if value.is_none() {
            let _guard = self
                .ready
                .wait_timeout(value, Duration::from_micros(timeout_us))
                .unwrap();
        }
    }

    pub fn get(&self) -> T
    where
        T: Clone,
    {
        self.value
            .lock()
            .unwrap()
            .as_ref()
            .expect("BoxEvent value is not set")
            .clone()
    }
}

impl<T> Default for ReactorBoxEvent<T> {
    fn default() -> Self {
        Self::new()
    }
}

/// Rust-only representation of `std::pair<A, B>` used by canonical sources.
pub struct StdPair<A, B> {
    pub first: A,
    pub second: B,
}

impl<A, B> StdPair<A, B> {
    pub fn new(first: A, second: B) -> StdPair<A, B> {
        StdPair { first, second }
    }
}

/// Rust-only facade spelling mapped to the public `std::string` ABI.
pub type LoggingString = std::string;

/// Opaque rustc-only model mapped to libc's `FILE` in generated C++.
#[repr(C)]
pub struct CFile {
    _opaque: [u8; 0],
}

/// Rust-side model of `std::source_location` used by Debugging tests.
pub struct SourceLocation {
    file: &'static str,
    line: u32,
}

impl SourceLocation {
    pub fn current() -> SourceLocation {
        SourceLocation {
            file: file!(),
            line: line!(),
        }
    }

    pub fn file_name(&self) -> &'static str {
        self.file
    }

    pub fn line(&self) -> u32 {
        self.line
    }
}

/// Rust-only spelling for exact `std::vector<T>` ABI mappings.
pub type StdVector<T> = Vec<T>;

/// Rustc-only method facade for the member-shaped `rusty::Arc::get_mut()`
/// C++ API. `std::sync::Arc` exposes the same operation as an associated
/// function, so canonical sources import this trait to retain one spelling in
/// both languages.
pub trait StdArcGetMutExt<T: ?Sized> {
    fn get_mut(&mut self) -> Option<&mut T>;
}

impl<T: ?Sized> StdArcGetMutExt<T> for ::std::sync::Arc<T> {
    fn get_mut(&mut self) -> Option<&mut T> {
        ::std::sync::Arc::get_mut(self)
    }
}

/// Rustc-only spelling for the sparse 32-bit wrapper exported directly by the
/// `rrr.basetypes` C++ module.  The production type map restores the public
/// `rrr::v32` spelling; this local model only supplies the checked Rust API.
#[derive(Clone, Copy, Default, Eq, PartialEq)]
pub struct SerializableV32(i32);

impl SerializableV32 {
    pub const fn new(value: i32) -> Self {
        Self(value)
    }

    pub fn set(&mut self, value: i32) {
        self.0 = value;
    }

    pub const fn get(&self) -> i32 {
        self.0
    }
}

/// Rustc-only spelling for the corresponding sparse 64-bit wrapper.
#[derive(Clone, Copy, Default, Eq, PartialEq)]
pub struct SerializableV64(i64);

impl SerializableV64 {
    pub const fn new(value: i64) -> Self {
        Self(value)
    }

    pub fn set(&mut self, value: i64) {
        self.0 = value;
    }

    pub const fn get(&self) -> i64 {
        self.0
    }
}

/// Rustc-only dispatch facade for the generated C++ `Serialize_` overload
/// namespace. The production type map restores that exact namespace spelling;
/// the Rust body is intentionally inert because canonical unit tests exercise
/// concrete leaf implementations directly.
pub struct SerializableSerializeDispatch;

impl SerializableSerializeDispatch {
    pub fn serialize<T: ?Sized, Archive>(_value: &T, _archive: &mut Archive) {}
}

/// Read-side counterpart of `SerializableSerializeDispatch`.
pub struct SerializableDeserializeDispatch;

impl SerializableDeserializeDispatch {
    pub fn deserialize<T, Archive>(_value: &mut T, _archive: &mut Archive) {}
}

/// Rustc-only model of the move-only zero-argument registry factory. The
/// production type map restores `rusty::Function<SerializableProxy()>`.
pub struct SerializableRegistryFactory {
    callback: Box<dyn Fn() -> SerializableProxy + Send>,
}

impl SerializableRegistryFactory {
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: Fn() -> SerializableProxy + Send + 'static,
    {
        Self {
            callback: Box::new(callback),
        }
    }
}

impl Deref for SerializableRegistryFactory {
    type Target = dyn Fn() -> SerializableProxy + Send;

    fn deref(&self) -> &Self::Target {
        &*self.callback
    }
}

/// Minimal rustc-only model of the runtime mutex.  Production C++ keeps using
/// `rusty::Mutex`; this wrapper exists only so canonical sources can be checked
/// by rustc, including const initialization of process-wide registries.
pub struct Mutex<T>(::std::sync::Mutex<T>);

impl<T> Mutex<T> {
    pub const fn new(value: T) -> Self {
        Self(::std::sync::Mutex::new(value))
    }

    pub fn lock(&self) -> ::std::sync::LockResult<::std::sync::MutexGuard<'_, T>> {
        self.0.lock()
    }
}

/// Facade-only sequence used for the Rusty B-tree set.  The production type
/// map restores the concrete C++ runtime container spelling.
pub struct BTreeSet<T> {
    values: Vec<T>,
}

impl<T> Default for BTreeSet<T> {
    fn default() -> Self {
        Self { values: Vec::new() }
    }
}

impl<T> BTreeSet<T> {
    pub fn len(&self) -> usize {
        self.values.len()
    }

    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }

    pub fn clear(&mut self) {
        self.values.clear();
    }

    pub fn insert(&mut self, value: T) {
        self.values.push(value);
    }

    pub fn iter(&self) -> ::std::slice::Iter<'_, T> {
        self.values.iter()
    }
}

/// Facade-only associative sequence.  It deliberately requires no ordering or
/// hashing bounds, matching the unconstrained C++ templates emitted from the
/// canonical source.
pub struct BTreeMap<K, V> {
    values: Vec<(K, V)>,
}

impl<K, V> Default for BTreeMap<K, V> {
    fn default() -> Self {
        Self { values: Vec::new() }
    }
}

impl<K, V> BTreeMap<K, V> {
    pub const fn new() -> Self {
        Self { values: Vec::new() }
    }

    pub fn len(&self) -> usize {
        self.values.len()
    }

    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }

    pub fn clear(&mut self) {
        self.values.clear();
    }

    pub fn insert(&mut self, key: K, value: V) {
        self.values.push((key, value));
    }

    pub fn get(&self, key: &K) -> Option<&V>
    where
        K: PartialEq,
    {
        self.values
            .iter()
            .find_map(|(candidate, value)| (candidate == key).then_some(value))
    }

    pub fn iter(&self) -> ::std::slice::Iter<'_, (K, V)> {
        self.values.iter()
    }
}

/// Hash-map facade kept distinct from `BTreeMap` so canonical source can carry
/// both otherwise-overlapping Rust trait implementations.
pub struct HashMap<K, V> {
    values: Vec<(K, V)>,
}

impl<K, V> Default for HashMap<K, V> {
    fn default() -> Self {
        Self { values: Vec::new() }
    }
}

impl<K, V> HashMap<K, V> {
    pub const fn new() -> Self {
        Self { values: Vec::new() }
    }

    pub fn len(&self) -> usize {
        self.values.len()
    }

    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }

    pub fn clear(&mut self) {
        self.values.clear();
    }

    pub fn insert(&mut self, key: K, value: V) {
        self.values.push((key, value));
    }

    pub fn insert_callable<C, R>(&mut self, key: K, callback: C)
    where
        C: Fn() -> R + 'static,
        R: 'static,
        V: FromCallable0<C, R>,
    {
        self.values
            .push((key, <V as FromCallable0<C, R>>::from_callable(callback)));
    }

    pub fn get(&self, key: &K) -> Option<&V>
    where
        K: PartialEq,
    {
        self.values
            .iter()
            .find_map(|(candidate, value)| (candidate == key).then_some(value))
    }

    pub fn iter(&self) -> ::std::slice::Iter<'_, (K, V)> {
        self.values.iter()
    }

    pub fn contains_key(&self, key: &K) -> bool
    where
        K: PartialEq,
    {
        self.values.iter().any(|(candidate, _)| candidate == key)
    }

    pub fn remove(&mut self, key: &K)
    where
        K: PartialEq,
    {
        self.values.retain(|(candidate, _)| candidate != key);
    }
}

/// Hash-set facade exposes the `map` field used by the historical encoder.
pub struct HashSet<T> {
    pub map: HashMap<T, ()>,
}

impl<T> Default for HashSet<T> {
    fn default() -> Self {
        Self {
            map: HashMap::new(),
        }
    }
}

impl<T> HashSet<T> {
    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    pub fn clear(&mut self) {
        self.map.clear();
    }

    pub const fn new() -> Self {
        Self {
            map: HashMap::new(),
        }
    }

    pub fn insert(&mut self, value: T) {
        self.map.insert(value, ());
    }

    pub fn contains(&self, value: &T) -> bool
    where
        T: PartialEq,
    {
        self.map.contains_key(value)
    }

    pub fn remove(&mut self, value: &T)
    where
        T: PartialEq,
    {
        self.map.remove(value);
    }
}

/// Distinct rustc-only model for `std::string_view`.
#[derive(Default)]
pub struct SerializableStdStringView {
    bytes: Vec<u8>,
}

impl SerializableStdStringView {
    pub fn size(&self) -> usize {
        self.bytes.len()
    }

    pub fn data(&self) -> *const u8 {
        self.bytes.as_ptr()
    }
}

/// Distinct rustc-only models keep otherwise-aliasing C++ container impls
/// coherent in Rust.  Each is mapped back to its historical STL spelling.
macro_rules! serializable_std_sequence {
    ($name:ident) => {
        pub struct $name<T> {
            values: Vec<T>,
        }

        impl<T> Default for $name<T> {
            fn default() -> Self {
                Self { values: Vec::new() }
            }
        }

        impl<T> $name<T> {
            pub fn size(&self) -> usize {
                self.values.len()
            }

            pub fn clear(&mut self) {
                self.values.clear();
            }

            pub fn reserve(&mut self, additional: usize) {
                self.values.reserve(additional);
            }

            pub fn push_back(&mut self, value: T) {
                self.values.push(value);
            }

            pub fn insert(&mut self, value: T) {
                self.values.push(value);
            }
        }

        impl<T> ::std::ops::Index<usize> for $name<T> {
            type Output = T;

            fn index(&self, index: usize) -> &Self::Output {
                &self.values[index]
            }
        }

        impl<'a, T> IntoIterator for &'a $name<T> {
            type Item = &'a T;
            type IntoIter = ::std::slice::Iter<'a, T>;

            fn into_iter(self) -> Self::IntoIter {
                self.values.iter()
            }
        }
    };
}

serializable_std_sequence!(SerializableStdList);
serializable_std_sequence!(SerializableStdVector);
serializable_std_sequence!(SerializableStdSet);
serializable_std_sequence!(SerializableStdUnorderedSet);

macro_rules! serializable_std_map {
    ($name:ident) => {
        pub struct $name<K, V> {
            values: Vec<StdPair<K, V>>,
        }

        impl<K, V> Default for $name<K, V> {
            fn default() -> Self {
                Self { values: Vec::new() }
            }
        }

        impl<K, V> $name<K, V> {
            pub fn size(&self) -> usize {
                self.values.len()
            }

            pub fn clear(&mut self) {
                self.values.clear();
            }

            pub fn emplace(&mut self, key: K, value: V) {
                self.values.push(StdPair::new(key, value));
            }
        }

        impl<'a, K, V> IntoIterator for &'a $name<K, V> {
            type Item = &'a StdPair<K, V>;
            type IntoIter = ::std::slice::Iter<'a, StdPair<K, V>>;

            fn into_iter(self) -> Self::IntoIter {
                self.values.iter()
            }
        }
    };
}

serializable_std_map!(SerializableStdMap);
serializable_std_map!(SerializableStdUnorderedMap);

/// Type-only rustc stand-ins for compiler-generated trait adapters.
pub struct RustcSinkBaseAdapterRefMut<T>(PhantomData<T>);
pub struct RustcSourceBaseAdapterRefMut<T>(PhantomData<T>);

/// Opaque rustc-only stand-in mapped to C++ `void` at the Serializable C ABI.
pub enum LegacyCVoid {}

/// The production emitter recognizes this call and emits
/// `rusty::make_box<Adapter>(value)`.  The divergent Rust facade lets the call
/// coerce to the local trait-object return type without pretending to model
/// C++'s generated adapter hierarchy.
pub fn make_box<Adapter>(_value: impl Sized) -> ! {
    let _ = core::marker::PhantomData::<Adapter>;
    panic!("rustc-only make_box facade is not executable")
}

/// Declarations for module-local C++ templates supplied by
/// `misc/serializable_support.hpp`.  They preserve structural C++ dispatch
/// while giving direct rustc a fully typed foreign boundary.
pub mod rusty {
    use crate::{Arc, SerializableProxy};

    pub use crate::ReactorPollThread;

    pub mod io {
        pub use ::std::io::Error;
    }

    pub mod ptr {
        /// # Safety
        ///
        /// `pointer.add(offset)` must remain within the same allocation or one
        /// byte past it.
        #[allow(unsafe_code)]
        pub unsafe fn add<T>(pointer: *const T, offset: usize) -> *const T {
            unsafe { pointer.add(offset) }
        }
    }

    pub mod net {
        pub use crate::{
            RustcIoError as Error, RustcSocketAddrV4 as SocketAddrV4,
            RustcTcpListener as TcpListener, RustcTcpStream as TcpStream,
        };

        pub fn socket_addr_v4_from_str(value: &str) -> Result<SocketAddrV4, Error> {
            value.parse::<SocketAddrV4>().map_err(|error| {
                Error::from(::std::io::Error::new(
                    ::std::io::ErrorKind::InvalidInput,
                    error.to_string(),
                ))
            })
        }

        pub fn socket_addr_v4_to_string(value: SocketAddrV4) -> String {
            value.to_string()
        }

        #[repr(C)]
        pub struct InAddr {
            pub s_addr: u32,
        }

        #[repr(C)]
        pub struct SockAddrIn {
            pub sin_addr: InAddr,
            pub sin_port: u16,
        }

        pub fn sockaddr_in_from_socket_addr_v4(value: SocketAddrV4) -> SockAddrIn {
            let octets = value.ip().octets();
            SockAddrIn {
                sin_addr: InAddr {
                    s_addr: u32::from_ne_bytes(octets).to_be(),
                },
                sin_port: value.port().to_be(),
            }
        }
    }

    pub mod os {
        pub mod fd {
            pub type OwnedFd = ::std::os::fd::OwnedFd;
        }
    }

    /// # Safety
    ///
    /// The C++ associated namespace for `T` must provide a compatible
    /// `serialize(const T&, Archive&)` overload which does not retain either
    /// borrowed argument.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_adl_serialize<T, Archive>(_value: &T, _archive: &mut Archive) {}

    /// # Safety
    ///
    /// The C++ associated namespace for `T` must provide a compatible
    /// `deserialize(T&, Archive&)` overload which does not retain either
    /// borrowed argument.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_adl_deserialize<T, Archive>(_value: &mut T, _archive: &mut Archive) {}

    /// # Safety
    ///
    /// If `_length` is nonzero, `_pointer` must address that many initialized
    /// readable bytes for the duration of the call.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_sink_write<Sink: ?Sized>(
        _sink: &mut Sink,
        _pointer: *const u8,
        _length: usize,
    ) {
    }

    /// # Safety
    ///
    /// If `_length` is nonzero, `_pointer` must address that many writable
    /// bytes for the duration of the call.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_source_read<Source: ?Sized>(
        _source: &mut Source,
        _pointer: *mut u8,
        _length: usize,
    ) -> usize {
        0
    }

    /// Rustc-only coercion into the move-only registry callback facade. The
    /// production runtime helper constructs `rusty::Function<R()>` from the
    /// supplied closure.
    ///
    /// # Safety
    ///
    /// The callable must remain valid after it is moved into the returned C++
    /// function wrapper and must return a well-formed owning proxy.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_factory_from_callable<C>(callback: C) -> crate::SerializableRegistryFactory
    where
        C: Fn() -> SerializableProxy + Send + 'static,
    {
        crate::SerializableRegistryFactory::from_callable(callback)
    }

    /// # Safety
    ///
    /// `T` must implement the structural C++ payload save contract for the
    /// supplied archive and may not retain the archive reference.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_payload_save<T, Archive>(_value: &T, _archive: &mut Archive) {}

    /// # Safety
    ///
    /// `T` must implement the structural C++ payload load contract for the
    /// supplied archive and may not retain the archive reference.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_payload_load<T, Archive>(_value: &mut T, _archive: &mut Archive) {}

    /// # Safety
    ///
    /// `T` must provide the structural C++ `kind() const` payload method.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_payload_kind<T>(_value: &T) -> i32 {
        0
    }

    /// # Safety
    ///
    /// The production C++ `T` must be default constructible and safe to own in
    /// `rusty::Arc<T>`.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_arc_default<T>() -> Arc<T> {
        panic!("rustc-only default Arc construction facade is not executable")
    }

    /// # Safety
    ///
    /// The production C++ `T` must be copy constructible and safe to own in
    /// `rusty::Arc<T>`.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_arc_copy<T>(_value: &T) -> Arc<T> {
        panic!("rustc-only copying Arc construction facade is not executable")
    }

    /// # Safety
    ///
    /// `Holder` must be the module-owned `SerializableBase` holder for `T`,
    /// with a constructor that takes ownership of the supplied Arc.
    #[allow(unsafe_code)]
    pub unsafe fn srpc_holder_proxy<Holder, T>(_value: Arc<T>) -> SerializableProxy {
        let _ = core::marker::PhantomData::<Holder>;
        panic!("rustc-only holder proxy construction facade is not executable")
    }
}

/// Rust-side model of helpers supplied by the C++ rusty runtime.
pub mod sys {
    pub mod env {
        /// Return a host name for direct-rustc tests without adding an unsafe
        /// syscall boundary to this compile-time-only facade. Production C++
        /// resolves this path to `rusty::sys::env::hostname()`.
        pub fn hostname() -> String {
            ::std::env::var("HOSTNAME").unwrap_or_default()
        }
    }

    pub mod time {
        pub fn sleep_us(microseconds: u64) {
            ::std::thread::sleep(::std::time::Duration::from_micros(microseconds));
        }

        /// Production C++ resolves this to `rusty::sys::time::clock_monotonic_us()`.
        pub fn clock_monotonic_us() -> u64 {
            use ::std::time::{SystemTime, UNIX_EPOCH};
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_micros() as u64
        }
    }

    pub mod process {
        /// Production C++ resolves this to `rusty::sys::process::getpid()`.
        pub fn getpid() -> i32 {
            ::std::process::id() as i32
        }
    }
}

pub mod panic {
    /// Opaque model of the C++ `std::exception_ptr` payload carried out of a
    /// caught unwind. Production C++ resolves the pair below to
    /// `rusty::panic::catch_unwind` / `rusty::panic::payload_message`.
    pub struct PanicPayload(Option<String>);

    pub fn do_panic(message: crate::std::string) -> ! {
        ::std::panic::panic_any(message.to_rust_string())
    }

    /// Run `body`, converting an unwind into `Err(PanicPayload)`.
    pub fn catch_unwind<F: FnMut()>(body: F) -> Result<(), PanicPayload> {
        let result = ::std::panic::catch_unwind(::std::panic::AssertUnwindSafe(body));
        match result {
            Ok(()) => Ok(()),
            Err(payload) => {
                let message = payload
                    .downcast_ref::<&str>()
                    .map(|text| (*text).to_string())
                    .or_else(|| payload.downcast_ref::<String>().cloned());
                Err(PanicPayload(message))
            }
        }
    }

    /// Recover a typed `std::exception::what()` message; an opaque payload
    /// yields `None`.
    pub fn payload_message(payload: PanicPayload) -> Option<String> {
        payload.0
    }
}

/// Handle-validity and emptiness predicates the runtime types carry but Rust's
/// own owning handles cannot be without. `rusty::Box` / `rusty::Arc` expose
/// `is_valid()` and `rusty::Function` exposes `is_empty()`; a Rust `Box` or
/// `Arc` is never null and a Rust boxed closure is never empty, so the rustc
/// models are constants. Canonical sources still spell the predicate so the
/// generated C++ keeps checking handles that reach it from C++ callers.
pub trait RustyHandleIsValid {
    fn is_valid(&self) -> bool;
}

impl<T: ?Sized> RustyHandleIsValid for Box<T> {
    fn is_valid(&self) -> bool {
        true
    }
}

impl<T: ?Sized> RustyHandleIsValid for ::std::sync::Arc<T> {
    fn is_valid(&self) -> bool {
        true
    }
}

/// `rusty::Box<T>::get()` — the raw pointee pointer the runtime handle
/// exposes. A Rust `Box` is never null, so this is an ordinary reborrow.
pub trait RustyBoxGet<T: ?Sized> {
    fn get(&self) -> *mut T;
}

impl<T: ?Sized> RustyBoxGet<T> for Box<T> {
    fn get(&self) -> *mut T {
        let borrowed: &T = self;
        borrowed as *const T as *mut T
    }
}

pub trait RustyFunctionIsEmpty {
    fn is_empty(&self) -> bool;
}

impl<T: ?Sized> RustyFunctionIsEmpty for Box<T> {
    fn is_empty(&self) -> bool {
        false
    }
}

/// Rustc-only model of the runtime condition variable. Production C++ keeps
/// using `rusty::Condvar`; this wrapper exists only so canonical sources can
/// be checked by rustc.
pub struct Condvar(::std::sync::Condvar);

impl Condvar {
    #[allow(clippy::new_without_default)]
    pub fn new() -> Self {
        Self(::std::sync::Condvar::new())
    }

    pub fn notify_all(&self) {
        self.0.notify_all();
    }

    pub fn wait_while<'a, T, F: FnMut(&mut T) -> bool>(
        &self,
        guard: ::std::sync::MutexGuard<'a, T>,
        condition: F,
    ) -> ::std::sync::LockResult<::std::sync::MutexGuard<'a, T>> {
        self.0.wait_while(guard, condition)
    }
}

/// `rusty::Cell<T>::get()` for a non-`Copy` payload. The runtime `Cell` copies
/// its value out for any copy-constructible `T`; `::std::cell::Cell` restricts
/// the inherent `get` to `Copy`, so canonical sources holding a
/// `Cell<std::string>` need this extension to be checkable by rustc. The
/// inherent method still wins wherever it applies, so `Cell<i32>::get` is
/// untouched.
pub trait RustyCellGet<T> {
    fn get(&self) -> T;
}

impl<T: Clone> RustyCellGet<T> for ::std::cell::Cell<T> {
    #[allow(unsafe_code)]
    fn get(&self) -> T {
        // SAFETY: `Cell` is `!Sync`, so no other thread can observe the cell,
        // and the clone below does not re-enter it.
        unsafe { (*self.as_ptr()).clone() }
    }
}

/// `std::string::c_str()` — the NUL-terminated view the C++ type exposes and
/// canonical sources hand to the C connect ladder. Rust's `String` is not
/// NUL-terminated, so the model returns the buffer pointer: it type-checks the
/// canonical body and keeps the emitted C++ calling the real `c_str()`.
pub trait RustyStdStringCStr {
    fn c_str(&self) -> *const i8;
}

impl RustyStdStringCStr for ::std::string::String {
    fn c_str(&self) -> *const i8 {
        self.as_ptr() as *const i8
    }
}


/// Rust-only declarations for C++ modules imported by canonical rrr sources.
/// The exact local `rusty` facade dependency is omitted from generated C++.
#[allow(clippy::missing_safety_doc)]
pub mod rrr {
    pub mod basetypes {
        pub struct Time;

        impl Time {
            /// # Safety
            ///
            /// Both clock selectors are valid; `unsafe` records the foreign
            /// named-module boundary used by canonical Fiber code.
            #[allow(unsafe_code)]
            pub unsafe fn now(_monotonic: bool) -> u64 {
                use ::std::time::{SystemTime, UNIX_EPOCH};
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_micros() as u64
            }
        }

        #[allow(non_snake_case)]
        pub mod SparseInt {
            #[allow(unsafe_code)]
            pub unsafe fn buf_size(byte0: u8) -> usize {
                crate::sparse_buf_size(byte0)
            }

            #[allow(unsafe_code)]
            pub unsafe fn dump32(value: i32, buffer: *mut u8) -> usize {
                unsafe { crate::sparse_dump32(value, buffer) }
            }

            #[allow(unsafe_code)]
            pub unsafe fn dump64(value: i64, buffer: *mut u8) -> usize {
                unsafe { crate::sparse_dump64(value, buffer) }
            }

            #[allow(unsafe_code)]
            pub unsafe fn load32(buffer: *const u8) -> i32 {
                unsafe { crate::sparse_load32(buffer) }
            }

            #[allow(unsafe_code)]
            pub unsafe fn load64(buffer: *const u8) -> i64 {
                unsafe { crate::sparse_load64(buffer) }
            }
        }
    }

    pub mod debugging {
        #[allow(unsafe_code)]
        pub unsafe fn verify(value: bool) {
            assert!(value);
        }
    }

    pub mod rand {
        pub struct RandomGenerator;

        impl RandomGenerator {
            /// # Safety
            ///
            /// Records the foreign named-module boundary; the production
            /// generator is a pure integer draw with no preconditions.
            #[allow(unsafe_code)]
            pub unsafe fn rand(min: i32, _max: i32) -> i32 {
                min
            }
        }
    }

    /// Compile-time-only namespace model used to retain the private
    /// `rrr.errors` named-module import in canonical callback generation.
    pub mod errors {}

    /// Compile-time-only namespace model used to retain the exact
    /// `rrr.internal_protocol` named-module import in canonical server
    /// generation. The wire constant itself is read through the crate path;
    /// only the provider edge is carried here.
    pub mod internal_protocol {}
    /// `rrr.callback_wrapper` named-module import in canonical client
    /// generation; the wrapper template itself is reached through the
    /// `rusty::CallbackWrapper` facade type.
    pub mod callback_wrapper {}

    pub mod logging {
        /// Rust-side no-op model of the production logging entry point.
        ///
        /// # Safety
        ///
        /// `file` must be null or point to a valid NUL-terminated path for the
        /// duration of the call. The production logger scans any non-null path.
        #[allow(unsafe_code)]
        pub unsafe fn log_line(_level: i32, _line: i32, _file: *const i8, _message: &String) {}
    }

    pub mod reactor {
        use crate::{ReactorBoxEvent, ReactorFiber, REACTOR_CURRENT_FIBER, REACTOR_SLEEP_CALLS};
        use ::std::cell::Cell;
        use ::std::rc::Rc;
        use ::std::sync::{Arc, Mutex};

        pub type Fiber = ReactorFiber;

        /// Rustc-only opaque model of the cross-thread poll command sender.
        pub struct PollThread;

        impl PollThread {
            /// # Safety
            ///
            /// `poll` must be a well-formed owning pollable proxy. The
            /// production method moves it into the worker command queue.
            #[allow(unsafe_code)]
            pub unsafe fn add_proxy<P>(&self, _poll: P) {}

            /// # Safety
            ///
            /// `fd` must identify a pollable registered with this thread.
            #[allow(unsafe_code)]
            pub unsafe fn update_mode(&self, _fd: i32, _new_mode: i32) {}

            /// # Safety
            ///
            /// No caller-side precondition; `unsafe` records the foreign
            /// named-module boundary.
            #[allow(unsafe_code)]
            pub unsafe fn create() -> Arc<PollThread> {
                Arc::new(PollThread)
            }

            /// # Safety
            ///
            /// `job` must be a well-formed owning job handle; the production
            /// method moves it into the worker command queue. The parameter is
            /// generic because the C++ surface takes `rusty::Arc<Job>` and
            /// canonical callers pass a concrete job whose Arc upcasts.
            #[allow(unsafe_code)]
            pub unsafe fn add<J>(&self, _job: Arc<J>) {}

            /// # Safety
            ///
            /// No caller-side precondition; `unsafe` records the foreign
            /// named-module boundary.
            #[allow(unsafe_code)]
            pub unsafe fn shutdown(&self) {}
        }

        /// Rustc-only affinity model. Tests execute outside the poll worker.
        pub fn pollworker_is_on_poll_thread() -> bool {
            false
        }


        /// # Safety
        ///
        /// `func` must be a well-formed owning closure; the production fiber
        /// runtime takes ownership and runs it on a new stack. `file` must be
        /// null or a NUL-terminated static string.
        #[allow(unsafe_code)]
        pub unsafe fn fiber_create_run_impl<F: FnMut()>(
            mut func: F,
            _file: *const i8,
            _line: i64,
        ) {
            func();
        }

        /// Rust-only model of the reactor's fiber-aware integer event.
        pub struct IntEvent {
            value: Mutex<i32>,
            target: i32,
            ready: ::std::sync::Condvar,
        }

        impl IntEvent {
            pub fn set(&self, next: i32) -> i32 {
                let mut value = self.value.lock().unwrap();
                let previous = *value;
                *value = next;
                self.ready.notify_all();
                previous
            }

            pub fn wait(&self) {
                let mut value = self.value.lock().unwrap();
                while *value < self.target {
                    value = self.ready.wait(value).unwrap();
                }
            }
        }

        /// # Safety
        ///
        /// Every integer target is valid; `unsafe` records the foreign module
        /// boundary used by canonical FiberChannel code.
        #[allow(unsafe_code)]
        pub unsafe fn create_sp_int_event(target: i32) -> Arc<IntEvent> {
            Arc::new(IntEvent {
                value: Mutex::new(0),
                target,
                ready: ::std::sync::Condvar::new(),
            })
        }

        /// # Safety
        ///
        /// This facade has no caller-side precondition. `unsafe` records the
        /// foreign named-module boundary at canonical Rust call sites.
        #[allow(unsafe_code)]
        pub unsafe fn create_sp_box_event<T>() -> Arc<ReactorBoxEvent<T>> {
            Arc::new(ReactorBoxEvent::new())
        }

        /// # Safety
        ///
        /// Every microsecond duration is accepted by the production reactor.
        #[allow(unsafe_code)]
        pub unsafe fn fiber_sleep(microseconds: u64) {
            REACTOR_SLEEP_CALLS.with(|calls| calls.borrow_mut().push(microseconds));
        }

        /// Install a test fiber for the duration of `body`.
        pub fn with_test_fiber<R>(id: u64, body: impl FnOnce() -> R) -> (R, u64) {
            let fiber = Rc::new(ReactorFiber {
                id: Cell::new(id),
                yields: Cell::new(0),
            });
            let previous = REACTOR_CURRENT_FIBER.with(|slot| slot.replace(Some(Rc::clone(&fiber))));
            let result = body();
            REACTOR_CURRENT_FIBER.with(|slot| {
                slot.replace(previous);
            });
            (result, fiber.yields.get())
        }

        /// Return and clear durations recorded by the rustc-only sleep model.
        pub fn take_test_sleep_calls() -> Vec<u64> {
            REACTOR_SLEEP_CALLS.with(|calls| core::mem::take(&mut *calls.borrow_mut()))
        }
    }

    pub mod serializable {
        pub use crate::{BinaryReadArchive, BinaryWriteArchive};

        /// # Safety
        ///
        /// `sink` must be non-null, uniquely borrowed, and remain alive and
        /// unmoved for every use of the returned proxy. Production C++
        /// resolves this to `make_sink_proxy_buffer`.
        #[allow(unsafe_code)]
        pub unsafe fn make_sink_proxy_buffer<S, P>(_sink: *mut S) -> P {
            // The rustc facade never runs: the production emitter resolves
            // this to the real `rrr::make_sink_proxy_buffer`.
            unreachable!("rustc-only serializable proxy facade")
        }

        /// # Safety
        ///
        /// Same borrow and lifetime contract as [`make_sink_proxy_buffer`],
        /// for the read side.
        #[allow(unsafe_code)]
        pub unsafe fn make_source_proxy_buffer<S, P>(_source: *mut S) -> P {
            unreachable!("rustc-only serializable proxy facade")
        }
        use crate::{SerializableBase, SerializableProxy, SerializableSharedPtrHolder};

        fn encoded_length_size(value: usize) -> usize {
            if value <= 63 {
                1
            } else if value <= 8_191 {
                2
            } else if value <= 1_048_575 {
                3
            } else if value <= 134_217_727 {
                4
            } else if value <= 17_179_869_183 {
                5
            } else if value <= 2_199_023_255_551 {
                6
            } else if value <= 281_474_976_710_655 {
                7
            } else if value <= 36_028_797_018_963_967 {
                8
            } else {
                9
            }
        }

        fn sparse_size(byte0: u8) -> usize {
            if byte0 & 0x80 == 0 {
                1
            } else if byte0 & 0xc0 == 0x80 {
                2
            } else if byte0 & 0xe0 == 0xc0 {
                3
            } else if byte0 & 0xf0 == 0xe0 {
                4
            } else if byte0 & 0xf8 == 0xf0 {
                5
            } else if byte0 & 0xfc == 0xf8 {
                6
            } else if byte0 & 0xfe == 0xfc {
                7
            } else if byte0 == 0xfe {
                8
            } else {
                9
            }
        }

        #[allow(non_camel_case_types)]
        pub struct Serialize_;

        impl Serialize_ {
            #[allow(clippy::ptr_arg)]
            #[allow(unsafe_code)]
            pub unsafe fn serialize(value: &String, archive: &mut BinaryWriteArchive) {
                let size = encoded_length_size(value.len());
                let bits = value.len() as u64;
                let mut encoded = [0u8; 9];
                if size <= 7 {
                    for (index, byte) in encoded[..size].iter_mut().enumerate() {
                        *byte = (bits >> (8 * (size - 1 - index))) as u8;
                    }
                    encoded[0] &= 0xff >> size;
                    if size > 1 {
                        encoded[0] |= 0xff << (9 - size);
                    }
                } else {
                    for index in 0..8 {
                        encoded[1 + index] = (bits >> (8 * (7 - index))) as u8;
                    }
                    encoded[0] = if size == 8 { 0xfe } else { 0xff };
                }
                unsafe { archive.write_bytes(encoded.as_ptr(), size) };
                unsafe { archive.write_bytes(value.as_ptr(), value.len()) };
            }
        }

        #[allow(non_camel_case_types)]
        pub struct Deserialize_;

        impl Deserialize_ {
            #[allow(unsafe_code)]
            pub unsafe fn deserialize(value: &mut String, archive: &mut BinaryReadArchive) {
                let mut encoded = [0u8; 9];
                unsafe { archive.read_or_abort(encoded.as_mut_ptr(), 1) };
                let size = sparse_size(encoded[0]);
                if size > 1 {
                    unsafe { archive.read_or_abort(encoded.as_mut_ptr().add(1), size - 1) };
                }
                let length = if size < 8 {
                    let mut bits = 0u64;
                    for index in 0..size - 1 {
                        bits |= (encoded[size - 1 - index] as u64) << (8 * index);
                    }
                    bits | (((encoded[0] & (0xff >> size)) as u64) << (8 * (size - 1)))
                } else {
                    let mut bits = 0u64;
                    for index in 0..8 {
                        bits |= (encoded[8 - index] as u64) << (8 * index);
                    }
                    bits
                } as usize;
                let mut bytes = vec![0u8; length];
                unsafe { archive.read_or_abort(bytes.as_mut_ptr(), bytes.len()) };
                *value = String::from_utf8(bytes).expect("valid UTF-8 AnyMessage type name");
            }
        }

        #[allow(unsafe_code)]
        pub unsafe fn serializable_holder_of<T: 'static>(
            _base: *const SerializableBase,
        ) -> *const SerializableSharedPtrHolder<T> {
            core::ptr::null()
        }

        #[allow(non_snake_case)]
        pub mod SerializableRegistry {
            use super::{SerializableBase, SerializableProxy};

            #[allow(unsafe_code)]
            pub unsafe fn create(_kind: i32) -> SerializableProxy {
                SerializableProxy::make(SerializableBase)
            }
        }
    }
}

/// Rust-only declarations behind `use cpp::std` in canonical code.
pub mod std {
    use crate::StdPair;
    use ::std::cell::UnsafeCell;
    use ::std::io::Write as _;

    /// Values accepted by the rustc-only `std::string::append` model.
    pub trait StringAppend {
        fn append_to(self, output: &mut Vec<u8>);
    }

    impl StringAppend for &str {
        fn append_to(self, output: &mut Vec<u8>) {
            output.extend_from_slice(self.as_bytes());
        }
    }

    impl StringAppend for &::std::string::String {
        fn append_to(self, output: &mut Vec<u8>) {
            output.extend_from_slice(self.as_bytes());
        }
    }

    /// Rustc-only byte model of `std::string`.
    #[allow(non_camel_case_types)]
    pub struct string(UnsafeCell<Vec<u8>>);

    impl Default for string {
        fn default() -> Self {
            Self(UnsafeCell::new(Vec::new()))
        }
    }

    impl StringAppend for string {
        fn append_to(self, output: &mut Vec<u8>) {
            output.extend_from_slice(self.0.into_inner().as_slice());
        }
    }

    impl StringAppend for &string {
        #[allow(unsafe_code)]
        fn append_to(self, output: &mut Vec<u8>) {
            // SAFETY: this facade is used only by single-threaded direct-rustc
            // logging tests; generated C++ maps the type to `std::string`.
            output.extend_from_slice(unsafe { (&*self.0.get()).as_slice() });
        }
    }

    /// Equality/ordering/hash and value-copy for the byte model. The
    /// production type is `std::string`, which is `Regular` and totally
    /// ordered, so canonical sources use it as a map key, inside a `Cell`, and
    /// compare it directly. The `UnsafeCell` interior blocks `derive`, so the
    /// four traits are written out over the same byte view `size()` uses.
    #[allow(unsafe_code)]
    fn string_bytes(value: &string) -> &[u8] {
        // SAFETY: identical to `size`/`to_rust_string` above -- direct-rustc
        // facade callers do not mutate this model concurrently.
        unsafe { (&*value.0.get()).as_slice() }
    }

    /// `std::string` converts to `std::string_view` implicitly in C++, which is
    /// what canonical sources rely on when they hand a stored address to a
    /// `&str` parameter. `Deref` is the Rust spelling of that implicit
    /// conversion and is invisible to the emitter: deref coercion happens in
    /// rustc's type checker, so the generated C++ call is unchanged.
    impl ::std::ops::Deref for string {
        type Target = str;

        fn deref(&self) -> &str {
            ::std::str::from_utf8(string_bytes(self)).unwrap_or("")
        }
    }

    impl Clone for string {
        fn clone(&self) -> Self {
            Self(UnsafeCell::new(string_bytes(self).to_vec()))
        }
    }

    impl PartialEq for string {
        fn eq(&self, other: &Self) -> bool {
            string_bytes(self) == string_bytes(other)
        }
    }

    impl Eq for string {}

    impl PartialOrd for string {
        fn partial_cmp(&self, other: &Self) -> Option<::std::cmp::Ordering> {
            Some(self.cmp(other))
        }
    }

    impl Ord for string {
        fn cmp(&self, other: &Self) -> ::std::cmp::Ordering {
            string_bytes(self).cmp(string_bytes(other))
        }
    }

    impl ::std::hash::Hash for string {
        fn hash<H: ::std::hash::Hasher>(&self, state: &mut H) {
            string_bytes(self).hash(state);
        }
    }

    impl string {
        pub fn append<T: StringAppend>(&mut self, value: T) {
            value.append_to(self.0.get_mut());
        }

        pub fn push_back(&mut self, value: i8) {
            self.0.get_mut().push(value as u8);
        }

        pub fn resize(&mut self, size: usize) {
            self.0.get_mut().resize(size, 0);
        }

        /// Rustc-only signature model of `std::string::c_str`.
        pub fn c_str(&self) -> *const i8 {
            core::ptr::null()
        }

        /// # Safety
        ///
        /// The returned pointer must not outlive this value or overlap any
        /// other access to its byte storage.
        #[allow(unsafe_code)]
        pub unsafe fn data(&self) -> *mut i8 {
            unsafe { (&mut *self.0.get()).as_mut_ptr().cast() }
        }

        pub fn is_empty(&self) -> bool {
            self.size() == 0
        }

        #[allow(unsafe_code)]
        pub fn size(&self) -> usize {
            // SAFETY: direct-rustc facade callers do not access this model
            // concurrently; the generated C++ uses `std::string` instead.
            unsafe { (&*self.0.get()).len() }
        }

        /// Clone the facade bytes into an ordinary Rust string for tests.
        #[allow(unsafe_code)]
        pub fn to_rust_string(&self) -> ::std::string::String {
            // SAFETY: direct-rustc facade callers do not mutate this model
            // concurrently; production maps the type to `std::string`.
            let bytes = unsafe { (&*self.0.get()).clone() };
            ::std::string::String::from_utf8(bytes).expect("valid UTF-8 in std::string facade")
        }
    }

    pub struct Cout;

    #[allow(non_upper_case_globals)]
    pub static cout: Cout = Cout;

    impl Cout {
        /// # Safety
        ///
        /// `data` must denote `size` readable bytes.
        #[allow(unsafe_code)]
        pub unsafe fn write(&self, data: *mut i8, size: usize) {
            let bytes = unsafe { core::slice::from_raw_parts(data.cast::<u8>(), size) };
            let _ = ::std::io::stdout().write_all(bytes);
        }

        /// # Safety
        ///
        /// The byte is written synchronously and has no additional precondition.
        #[allow(unsafe_code)]
        pub unsafe fn put(&self, value: i8) {
            let _ = ::std::io::stdout().write_all(&[value as u8]);
        }

        /// # Safety
        ///
        /// The flush is synchronous and has no additional precondition.
        #[allow(unsafe_code)]
        pub unsafe fn flush(&self) {
            let _ = ::std::io::stdout().flush();
        }
    }

    /// # Safety
    ///
    /// This facade has no caller-side precondition. `unsafe` records the
    /// foreign named-module boundary at canonical Rust call sites.
    #[allow(unsafe_code)]
    pub unsafe fn make_pair<A, B>(first: A, second: B) -> StdPair<A, B> {
        StdPair::new(first, second)
    }
}

fn sparse_buf_size(byte0: u8) -> usize {
    if byte0 & 0x80 == 0 {
        1
    } else if byte0 & 0xc0 == 0x80 {
        2
    } else if byte0 & 0xe0 == 0xc0 {
        3
    } else if byte0 & 0xf0 == 0xe0 {
        4
    } else if byte0 & 0xf8 == 0xf0 {
        5
    } else if byte0 & 0xfc == 0xf8 {
        6
    } else if byte0 & 0xfe == 0xfc {
        7
    } else if byte0 == 0xfe {
        8
    } else {
        9
    }
}

#[allow(unsafe_code)]
unsafe fn sparse_dump32(value: i32, buffer: *mut u8) -> usize {
    unsafe { sparse_dump64(value as i64, buffer) }
}

#[allow(unsafe_code)]
unsafe fn sparse_load32(buffer: *const u8) -> i32 {
    unsafe { sparse_load_signed(buffer, 4) as i32 }
}

fn sparse_value_size(value: i64) -> usize {
    if (-64..=63).contains(&value) {
        1
    } else if (-8_192..=8_191).contains(&value) {
        2
    } else if (-1_048_576..=1_048_575).contains(&value) {
        3
    } else if (-134_217_728..=134_217_727).contains(&value) {
        4
    } else if (-17_179_869_184..=17_179_869_183).contains(&value) {
        5
    } else if (-2_199_023_255_552..=2_199_023_255_551).contains(&value) {
        6
    } else if (-281_474_976_710_656..=281_474_976_710_655).contains(&value) {
        7
    } else if (-36_028_797_018_963_968..=36_028_797_018_963_967).contains(&value) {
        8
    } else {
        9
    }
}

#[allow(unsafe_code)]
unsafe fn sparse_dump64(value: i64, buffer: *mut u8) -> usize {
    let encoded = value as u64;
    let size = sparse_value_size(value);
    unsafe {
        if size <= 7 {
            for index in 0..size {
                *buffer.add(index) = ((encoded >> (8 * (size - 1 - index))) & 0xff) as u8;
            }
            let prefix = match size {
                1 => 0x00,
                2 => 0x80,
                3 => 0xc0,
                4 => 0xe0,
                5 => 0xf0,
                6 => 0xf8,
                7 => 0xfc,
                _ => unreachable!(),
            };
            *buffer &= 0xff >> size;
            *buffer |= prefix;
            return size;
        }
        for index in 0..8 {
            *buffer.add(index + 1) = ((encoded >> (8 * (7 - index))) & 0xff) as u8;
        }
        *buffer = if size == 8 { 0xfe } else { 0xff };
    }
    size
}

#[allow(unsafe_code)]
unsafe fn sparse_load_signed(buffer: *const u8, width: usize) -> i64 {
    let size = unsafe { sparse_buf_size(*buffer) };
    // The 64-bit marker 0xfe reports eight while still carrying eight payload
    // bytes after the marker. The historical decoder therefore enters this
    // full-width branch for `size == width`, not only `width + 1`.
    let full_width_marker = if width == 8 { width } else { width + 1 };
    if size >= full_width_marker {
        let mut value = 0u64;
        for index in 0..width {
            value |= unsafe { *buffer.add(width - index) as u64 } << (8 * index);
        }
        return value as i64;
    }
    let mut value = 0u64;
    for index in 0..size.saturating_sub(1) {
        value |= unsafe { *buffer.add(size - 1 - index) as u64 } << (8 * index);
    }
    let mut top = unsafe { *buffer } & (0xff >> size);
    let negative = ((top >> (7 - size)) & 1) == 1;
    if negative {
        top |= (0xff << (7 - size)) as u8;
        for index in size..width {
            value |= 0xffu64 << (8 * index);
        }
    }
    value |= (top as u64) << (8 * (size - 1));
    value as i64
}

#[allow(unsafe_code)]
unsafe fn sparse_load64(buffer: *const u8) -> i64 {
    unsafe { sparse_load_signed(buffer, 8) }
}

pub struct Arc<T: ?Sized> {
    inner: ::std::sync::Arc<T>,
}

impl<T: ?Sized> Clone for Arc<T> {
    fn clone(&self) -> Self {
        Self {
            inner: ::std::sync::Arc::clone(&self.inner),
        }
    }
}

impl<T: ?Sized> Arc<T> {
    pub fn get(&self) -> *const T {
        ::std::sync::Arc::as_ptr(&self.inner)
    }

    pub fn get_mut(&mut self) -> Option<&mut T> {
        ::std::sync::Arc::get_mut(&mut self.inner)
    }
}

impl<T: ?Sized> Deref for Arc<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

pub trait ArcMake<Argument> {
    type Output;
    fn make(argument: Argument) -> Self::Output;
}

impl<T> ArcMake<T> for Arc<T> {
    type Output = Arc<T>;

    fn make(argument: T) -> Self::Output {
        Arc {
            inner: ::std::sync::Arc::new(argument),
        }
    }
}

impl<T> ArcMake<Arc<T>> for Arc<SerializableSharedPtrHolder<T>> {
    type Output = SerializableProxy;

    fn make(_argument: Arc<T>) -> Self::Output {
        SerializableProxy::make(SerializableBase)
    }
}

impl<T: ?Sized> Arc<T> {
    pub fn make<Argument>(argument: Argument) -> <Self as ArcMake<Argument>>::Output
    where
        Self: ArcMake<Argument>,
    {
        <Self as ArcMake<Argument>>::make(argument)
    }
}

/// Rust-only test model for the binary archive supplied by
/// `rrr.serializable` in production C++.
#[derive(Default)]
pub struct BinaryWriteArchive {
    bytes: Vec<u8>,
}

impl BinaryWriteArchive {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Append a raw byte range to this archive.
    ///
    /// # Safety
    ///
    /// When `length` is nonzero, `pointer` must be non-null and reference
    /// `length` readable, initialized bytes for the duration of this call.
    #[allow(unsafe_code)]
    pub unsafe fn write_bytes(&mut self, pointer: *const u8, length: usize) {
        if length == 0 {
            return;
        }

        // SAFETY: upheld by the caller contract above. The early return keeps
        // the zero-length case independent of raw-pointer validity.
        let bytes = unsafe { ::std::slice::from_raw_parts(pointer, length) };
        self.bytes.extend_from_slice(bytes);
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.bytes
    }
}

/// Rust-only test model for the binary archive supplied by
/// `rrr.serializable` in production C++.
pub struct BinaryReadArchive {
    bytes: Vec<u8>,
    offset: usize,
}

impl BinaryReadArchive {
    pub fn new(bytes: &[u8]) -> Self {
        Self::from_bytes(bytes.to_vec())
    }

    pub fn from_bytes(bytes: Vec<u8>) -> Self {
        Self { bytes, offset: 0 }
    }

    pub fn remaining(&self) -> usize {
        self.bytes.len().saturating_sub(self.offset)
    }

    /// Copy the requested archive bytes into a raw destination.
    ///
    /// # Safety
    ///
    /// When `length` is nonzero, `pointer` must be non-null and reference
    /// `length` writable bytes which do not overlap this archive's storage.
    #[allow(unsafe_code)]
    pub unsafe fn read_exact(&mut self, pointer: *mut u8, length: usize) -> bool {
        let Some(end) = self.offset.checked_add(length) else {
            return false;
        };
        let Some(source) = self.bytes.get(self.offset..end) else {
            return false;
        };

        if length != 0 {
            // SAFETY: upheld by the caller contract above. The nonzero guard
            // keeps an empty copy independent of raw-pointer validity.
            let destination = unsafe { ::std::slice::from_raw_parts_mut(pointer, length) };
            destination.copy_from_slice(source);
        }
        self.offset = end;
        true
    }

    /// Copy the next archive bytes or abort this Rust-only model.
    ///
    /// # Safety
    ///
    /// The caller must uphold the same destination contract as `read_exact`.
    #[allow(unsafe_code)]
    pub unsafe fn read_or_abort(&mut self, pointer: *mut u8, length: usize) {
        assert!(
            unsafe { self.read_exact(pointer, length) },
            "binary archive source is truncated"
        );
    }
}

pub struct SerializableBase;

impl SerializableBase {
    pub fn save(&self, _archive: &mut BinaryWriteArchive) {}
    pub fn load(&mut self, _archive: &mut BinaryReadArchive) {}
    pub fn kind(&self) -> i32 {
        0
    }

    pub fn payload_type_id(&self) -> ::std::any::TypeId {
        ::std::any::TypeId::of::<Self>()
    }
}

pub struct SerializableSharedPtrHolder<T> {
    pub ptr: Arc<T>,
}

pub type SerializableProxy = Arc<SerializableBase>;

/// Rust-only contract for metric views used by the canonical load-balancer module.
pub trait LoadBalancerMetrics {
    fn in_flight_requests(&self) -> u64;
    fn avg_latency_us(&self) -> u64;
    fn requests_completed(&self) -> u64;
}

/// Rust-only contract for a client exposing a load-balancer metric view.
pub trait LoadBalancerClient {
    type Metrics: LoadBalancerMetrics;

    fn metrics(&self) -> &Self::Metrics;
}

/// Rust-only contract for pointer-like client handles.
pub trait LoadBalancerClientHandle: Deref
where
    Self::Target: LoadBalancerClient,
{
}

impl<T> LoadBalancerClientHandle for T
where
    T: Deref,
    T::Target: LoadBalancerClient,
{
}

/// Rust-only contract for indexable client pools.
#[allow(clippy::len_without_is_empty)]
pub trait LoadBalancerClientVec: Index<usize>
where
    Self::Output: LoadBalancerClientHandle,
    <Self::Output as Deref>::Target: LoadBalancerClient,
{
    fn len(&self) -> usize;
}

impl<T> LoadBalancerClientVec for Vec<T>
where
    T: LoadBalancerClientHandle,
    <T as Deref>::Target: LoadBalancerClient,
{
    fn len(&self) -> usize {
        Vec::len(self)
    }
}

/// Rust-side model of rusty-cpp's move-only type-erased callable.
///
/// `None` is the exact empty state. The explicit representation padding and
/// alignment keep the Rust facade at 48/16 on both 32- and 64-bit pointer
/// widths, matching the production 64-bit `rusty::Function` layout. The boxed
/// trait object gives rustc the same `Fn`/`FnMut` call semantics.
#[repr(C, align(16))]
pub struct Function<F: ?Sized> {
    inner: Option<Box<F>>,
    runtime_layout_padding: [u8; 32],
}

impl<F: ?Sized> Function<F> {
    /// Returns true when no callback is installed.
    pub fn is_empty(&self) -> bool {
        self.inner.is_none()
    }
}

impl<F: ?Sized> Default for Function<F> {
    fn default() -> Self {
        Self {
            inner: None,
            runtime_layout_padding: [0; 32],
        }
    }
}

impl<F: ?Sized> Deref for Function<F> {
    type Target = F;

    fn deref(&self) -> &F {
        self.inner
            .as_deref()
            .expect("attempted to call an empty rusty::Function")
    }
}

impl<F: ?Sized> DerefMut for Function<F> {
    fn deref_mut(&mut self) -> &mut F {
        self.inner
            .as_deref_mut()
            .expect("attempted to call an empty rusty::Function")
    }
}

impl<A: 'static, B: 'static> Function<dyn Fn(A, B)> {
    /// Erases a const-callable two-argument callback.
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: Fn(A, B) + 'static,
    {
        Self {
            inner: Some(Box::new(callback)),
            runtime_layout_padding: [0; 32],
        }
    }
}

/// Conversion used by the serializable registry's rustc-only HashMap facade.
pub trait FromCallable0<C, R> {
    fn from_callable(callback: C) -> Self;
}

impl<C, R> FromCallable0<C, R> for Function<dyn Fn() -> R>
where
    C: Fn() -> R + 'static,
    R: 'static,
{
    fn from_callable(callback: C) -> Self {
        Self {
            inner: Some(Box::new(callback)),
            runtime_layout_padding: [0; 32],
        }
    }
}

impl<R: 'static> Function<dyn Fn() -> R + Send> {
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: Fn() -> R + Send + 'static,
    {
        Self {
            inner: Some(Box::new(callback)),
            runtime_layout_padding: [0; 32],
        }
    }
}

impl<R: 'static> Function<dyn Fn() -> R> {
    /// Erases a const-callable zero-argument callback with a return value.
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: Fn() -> R + 'static,
    {
        <Self as FromCallable0<C, R>>::from_callable(callback)
    }
}

impl Function<dyn FnMut()> {
    /// Erases a mutable zero-argument callback.
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: FnMut() + 'static,
    {
        Self {
            inner: Some(Box::new(callback)),
            runtime_layout_padding: [0; 32],
        }
    }
}

impl Function<dyn FnMut(i32)> {
    /// Erases a mutable one-argument callback.
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: FnMut(i32) + 'static,
    {
        Self {
            inner: Some(Box::new(callback)),
            runtime_layout_padding: [0; 32],
        }
    }
}

impl<A: ?Sized + 'static, R: 'static> Function<dyn FnMut(&mut A) -> R> {
    /// Erases a mutable callback whose argument is borrowed for the duration
    /// of each call.  This separate higher-ranked form models C++ callbacks
    /// such as `Function<bool(Context&)>` without requiring a `'static`
    /// reference at the Rust call site.
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: for<'a> FnMut(&'a mut A) -> R + 'static,
    {
        Self {
            inner: Some(Box::new(callback)),
            runtime_layout_padding: [0; 32],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::Function;
    use ::std::cell::Cell;
    use ::std::mem::{align_of, size_of};
    use ::std::rc::Rc;

    #[test]
    fn empty_and_layout_match_the_cpp_runtime() {
        let callback: Function<dyn Fn(i32, i32)> = Function::default();
        assert!(callback.is_empty());
        assert_eq!(size_of::<Function<dyn Fn(i32, i32)>>(), 48);
        assert_eq!(align_of::<Function<dyn Fn(i32, i32)>>(), 16);
        assert_eq!(size_of::<Function<dyn FnMut()>>(), 48);
        assert_eq!(align_of::<Function<dyn FnMut()>>(), 16);
        assert_eq!(size_of::<Function<dyn FnMut(i32)>>(), 48);
        assert_eq!(align_of::<Function<dyn FnMut(i32)>>(), 16);

        macro_rules! assert_not_auto_trait {
            ($type:ty, $auto_trait:ident) => {{
                trait AmbiguousIfImplemented<Marker> {
                    fn marker() {}
                }
                impl<T: ?Sized> AmbiguousIfImplemented<()> for T {}
                impl<T: ?Sized + $auto_trait> AmbiguousIfImplemented<u8> for T {}
                let _ = <$type as AmbiguousIfImplemented<_>>::marker;
            }};
        }
        assert_not_auto_trait!(Function<dyn Fn(i32, i32)>, Send);
        assert_not_auto_trait!(Function<dyn Fn(i32, i32)>, Sync);
        assert_not_auto_trait!(Function<dyn FnMut()>, Send);
        assert_not_auto_trait!(Function<dyn FnMut()>, Sync);
        assert_not_auto_trait!(Function<dyn FnMut(i32)>, Send);
        assert_not_auto_trait!(Function<dyn FnMut(i32)>, Sync);
    }

    #[test]
    fn fn_and_fn_mut_dispatch() {
        let observed = Rc::new(Cell::new((0, 0)));
        let sink = Rc::clone(&observed);
        let callback = Function::<dyn Fn(i32, i32)>::from_callable(move |a, b| {
            sink.set((a, b));
        });
        callback(4, 9);
        assert_eq!(observed.get(), (4, 9));

        let calls = Rc::new(Cell::new(0));
        let counter = Rc::clone(&calls);
        let mut callback = Function::<dyn FnMut()>::from_callable(move || {
            counter.set(counter.get() + 1);
        });
        callback();
        callback();
        assert_eq!(calls.get(), 2);

        let sum = Rc::new(Cell::new(0));
        let accumulator = Rc::clone(&sum);
        let mut callback = Function::<dyn FnMut(i32)>::from_callable(move |value| {
            accumulator.set(accumulator.get() + value);
        });
        callback(7);
        callback(5);
        assert_eq!(sum.get(), 12);
    }
}
