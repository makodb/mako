#![deny(unsafe_code)]

//! Rust-only facades for APIs supplied by the rusty-cpp C++ runtime.
//!
//! The `rrr` crate uses this package for direct rustc checking and tests. The
//! rusty-cpp crate emitter recognizes this exact local package identity and
//! omits it from generated C++ because the production definitions already
//! live in the rusty runtime headers.

use ::std::ops::{Deref, DerefMut, Index};

pub mod std {
    use ::std::cell::UnsafeCell;

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

pub mod panic {
    pub fn do_panic(message: crate::std::string) -> ! {
        ::std::panic::panic_any(message.to_rust_string())
    }
}

/// Rust-side model of helpers supplied by the C++ rusty runtime.
pub mod sys {
    pub mod env {
        /// Return a host name for direct-rustc tests without adding an unsafe
        /// syscall boundary to this compile-time-only facade. Production C++
        /// resolves this path to `rusty::sys::env::hostname()`.
        pub fn hostname() -> String {
            std::env::var("HOSTNAME").unwrap_or_default()
        }
    }
}

/// Rust-only declarations for C++ modules imported by canonical rrr sources.
/// The exact local `rusty` facade dependency is omitted from generated C++.
pub mod rrr {
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
}

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

impl<A: 'static> Function<dyn FnMut(A)> {
    /// Erases a mutable one-argument callback.
    pub fn from_callable<C>(callback: C) -> Self
    where
        C: FnMut(A) + 'static,
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
    use std::cell::Cell;
    use std::mem::{align_of, size_of};
    use std::rc::Rc;

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
