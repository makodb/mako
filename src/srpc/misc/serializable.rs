//! Canonical serialization, archives, payload ownership, and registry.

#![allow(non_camel_case_types, non_snake_case)]

use crate::basetypes::SparseInt;
use crate::debugging::verify_at;
use std::sync::Arc;

pub type v32 = crate::basetypes::v32;
pub type v64 = crate::basetypes::v64;

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_fd_write_once(fd: i32, pointer: *const core::ffi::c_void, length: usize) -> i64;
    fn srpc_fd_read_once(fd: i32, pointer: *mut core::ffi::c_void, length: usize) -> i64;
    fn srpc_fd_last_errno() -> i32;
    fn srpc_fd_interrupted_errno() -> i32;
}

#[cfg_attr(any(), cpp_trait_member_dispatch)]
#[allow(unsafe_code)]
pub trait SinkBase {
    /// # Safety
    ///
    /// If `n` is nonzero, `p` must address at least `n` initialized bytes in
    /// one allocation, `n` must not exceed `isize::MAX`, and that range must
    /// remain readable and unaliased with any destination storage the concrete
    /// sink may mutate or reallocate for the duration of the call. A null
    /// pointer is permitted only when `n == 0`.
    unsafe fn write_bytes(&mut self, p: *const u8, n: usize);
}

#[cfg_attr(any(), cpp_trait_member_dispatch)]
#[allow(unsafe_code)]
pub trait SourceBase {
    /// # Safety
    ///
    /// If `n` is nonzero, `p` must address at least `n` writable bytes in one
    /// allocation and `n` must not exceed `isize::MAX`. The destination must
    /// not overlap the concrete source's retained readable storage. Any such
    /// retained source storage must itself remain valid, readable, and free of
    /// concurrent mutation for its advertised remaining range, and its public
    /// cursor must satisfy `pos_ <= len_`. The
    /// implementation initializes exactly the returned number of bytes and
    /// never returns a value greater than `n`.
    unsafe fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize;
}

pub type SinkProxy = Box<dyn SinkBase>;
pub type SourceProxy = Box<dyn SourceBase>;

pub struct BufferSink {
    pub bytes: Vec<u8>,
}
#[allow(unsafe_code)]
impl SinkBase for BufferSink {
    unsafe fn write_bytes(&mut self, p: *const u8, n: usize) {
        if n == 0 {
            return;
        }
        // Appends, so extend_from_slice is exactly the old kernel's work:
        // grow if needed, then copy. The hand-rolled capacity doubling is
        // gone with it — Vec already amortises growth.
        //
        // The `sink_span` hand-bridge that used to sit above this block —
        // "the SinkBase trait hands write_bytes a raw (ptr, len) pair, and
        // the DSL cannot build a span from one" — was STALE.
        // `core::slice::from_raw_parts` lowers to `rusty::from_raw_parts(p,
        // n)`, whose body IS `std::span<const uint8_t>(p, n)`: the exact
        // expression the kernel spelled by hand. The same shape already
        // ships in GEN blocks inmemory_channel.14 and fiber_channel.3.
        //
        // @unsafe - builds a borrowed `&[u8]` over the caller's raw
        // pointer. Inherent boundary: the SinkBase contract pins those
        // bytes for the duration of the call.
        self.bytes
            .extend_from_slice(unsafe { core::slice::from_raw_parts(p, n) });
    }
}

pub struct BufferSource {
    pub data_: *const u8,
    pub len_: usize,
    pub pos_: usize,
}

impl BufferSource {
    pub fn new(data: *const u8, len: usize) -> BufferSource {
        BufferSource {
            data_: data,
            len_: len,
            pos_: 0usize,
        }
    }

    pub fn pos(&self) -> usize {
        self.pos_
    }
    pub fn remaining(&self) -> usize {
        self.len_ - self.pos_
    }
    pub fn eof(&self) -> bool {
        self.pos_ >= self.len_
    }
}

#[allow(unsafe_code)]
impl SourceBase for BufferSource {
    unsafe fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize {
        let avail: usize = self.len_ - self.pos_;
        let mut take: usize = n;
        if avail < take {
            take = avail;
        }
        if take > 0usize {
            unsafe {
                // Keep the raw field in a typed local before pointer arithmetic.
                // This makes both rustc and C++ lowering retain the pointer
                // category instead of mistaking `.add` for a user method.
                let data: *const u8 = self.data_;
                let start: *const u8 = data.add(self.pos_);
                core::ptr::copy_nonoverlapping(start, p, take);
            }
            self.pos_ += take;
        }
        take
    }
}

/// # Safety
///
/// `sink` must be non-null, uniquely borrowed, and remain alive and unmoved
/// for every use of the returned proxy.
#[allow(unsafe_code)]
pub unsafe fn make_sink_proxy_buffer(sink: *mut BufferSink) -> SinkProxy {
    Box::new(borrowed_io::BorrowedSink { pointer: sink })
}

/// # Safety
///
/// `source` must be non-null, uniquely borrowed, and remain alive and unmoved
/// for every use of the returned proxy. Its public raw backing fields must
/// satisfy [`SourceBase::read_bytes`]'s retained-source contract throughout
/// that lifetime.
#[allow(unsafe_code)]
pub unsafe fn make_source_proxy_buffer(source: *mut BufferSource) -> SourceProxy {
    Box::new(borrowed_io::BorrowedSource { pointer: source })
}

pub struct FdSink {
    pub fd_: i32,
}

impl FdSink {
    pub fn new(fd: i32) -> FdSink {
        FdSink { fd_: fd }
    }

    pub fn fd(&self) -> i32 {
        self.fd_
    }
}

#[allow(unsafe_code)]
impl SinkBase for FdSink {
    unsafe fn write_bytes(&mut self, p: *const u8, n: usize) {
        let mut written = 0usize;
        while written < n {
            let count = unsafe {
                srpc_fd_write_once(self.fd_, p.add(written) as *const core::ffi::c_void, n - written)
            };
            if count < 0 && unsafe { srpc_fd_last_errno() == srpc_fd_interrupted_errno() } {
                continue;
            }
            if count <= 0 {
                std::process::abort();
            }
            written += count as usize;
        }
    }
}

pub struct FdSource {
    pub fd_: i32,
}

impl FdSource {
    pub fn new(fd: i32) -> FdSource {
        FdSource { fd_: fd }
    }

    pub fn fd(&self) -> i32 {
        self.fd_
    }
}

#[allow(unsafe_code)]
impl SourceBase for FdSource {
    unsafe fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize {
        let mut got = 0usize;
        while got < n {
            let count = unsafe {
                srpc_fd_read_once(self.fd_, p.add(got) as *mut core::ffi::c_void, n - got)
            };
            if count < 0 && unsafe { srpc_fd_last_errno() == srpc_fd_interrupted_errno() } {
                continue;
            }
            if count < 0 {
                std::process::abort();
            }
            if count == 0 {
                break;
            }
            got += count as usize;
        }
        got
    }
}

mod borrowed_io {
    use super::{SinkBase, SourceBase};

    // The proxy owns a pointer carrier. Its unsafe constructor keeps the target
    // exclusively borrowed and alive; both lanes use this forwarding implementation
    // and ordinary Box trait-object coercion.
    pub(super) struct BorrowedSink<T: SinkBase> {
        pub(super) pointer: *mut T,
    }

    #[allow(unsafe_code)]
    #[cfg_attr(any(), cpp_inherit)]
    impl<T: SinkBase> SinkBase for BorrowedSink<T> {
        unsafe fn write_bytes(&mut self, pointer: *const u8, length: usize) {
            // SAFETY: construction pins the exclusive target borrow; the caller
            // supplies the unchanged byte-range contract.
            unsafe { (*self.pointer).write_bytes(pointer, length) }
        }
    }

    pub(super) struct BorrowedSource<T: SourceBase> {
        pub(super) pointer: *mut T,
    }

    #[allow(unsafe_code)]
    #[cfg_attr(any(), cpp_inherit)]
    impl<T: SourceBase> SourceBase for BorrowedSource<T> {
        unsafe fn read_bytes(&mut self, pointer: *mut u8, length: usize) -> usize {
            // SAFETY: construction pins the exclusive target borrow; the caller
            // supplies the unchanged byte-range contract.
            unsafe { (*self.pointer).read_bytes(pointer, length) }
        }
    }

}

/// # Safety
///
/// `sink` must satisfy the same exclusive-lifetime contract as
/// [`make_sink_proxy_buffer`]. Its file descriptor must remain open for
/// writing and externally serialized for the returned proxy's lifetime; this
/// function does not take descriptor ownership.
#[allow(unsafe_code)]
pub unsafe fn make_sink_proxy_fd(sink: *mut FdSink) -> SinkProxy {
    Box::new(borrowed_io::BorrowedSink { pointer: sink })
}

/// # Safety
///
/// `source` must be non-null, uniquely borrowed, and remain alive and unmoved
/// for every use of the returned proxy. Its file descriptor must remain open
/// for reading and externally serialized for that lifetime; this function
/// does not take descriptor ownership.
#[allow(unsafe_code)]
pub unsafe fn make_source_proxy_fd(source: *mut FdSource) -> SourceProxy {
    Box::new(borrowed_io::BorrowedSource { pointer: source })
}

pub struct BinaryWriteArchive {
    pub sink_: SinkProxy,
}

#[allow(unsafe_code)]
impl BinaryWriteArchive {
    /// # Safety
    ///
    /// `p` must satisfy [`SinkBase::write_bytes`]'s readable-buffer contract.
    pub unsafe fn write_bytes(&mut self, p: *const u8, n: usize) {
        unsafe { (*self.sink_).write_bytes(p, n) }
    }
}

pub trait Serialize {
    fn serialize(&self, ar: &mut BinaryWriteArchive);
}
#[allow(unsafe_code)]
impl Serialize for v32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        let bsize = unsafe { SparseInt::dump32(self.get(), b.as_mut_ptr()) };
        unsafe { ar.write_bytes(b.as_ptr(), bsize) };
    }
}
#[allow(unsafe_code)]
impl Serialize for v64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        let bsize = unsafe { SparseInt::dump64(self.get(), b.as_mut_ptr()) };
        unsafe { ar.write_bytes(b.as_ptr(), bsize) };
    }
}
#[allow(unsafe_code)]
impl Serialize for i32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i32) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i32>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for i8 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i8) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i8>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for i16 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i16) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i16>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for i64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const i64) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<i64>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for u8 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = self as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u8>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for u16 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u16) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u16>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for u32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u32) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u32>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for u64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const u64) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<u64>());
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for f64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe {
            let p: *const u8 = (self as *const f64) as *const u8;
            ar.write_bytes(p, std::mem::size_of::<f64>());
        }
    }
}

/// Forward a field through canonical overload selection, including scalar leaves.
pub fn serialize_value<Value: Serialize + ?Sized>(value: &Value, ar: &mut BinaryWriteArchive) {
    Serialize_::serialize(value, ar);
}

/// Forward a mutable field through canonical overload selection.
pub fn deserialize_value<Value: Deserialize + ?Sized>(value: &mut Value, ar: &mut BinaryReadArchive) {
    Deserialize_::deserialize(value, ar);
}

/// Write a map entry or pair in its canonical field order.
pub fn serialize_pair_fields<First: Serialize + ?Sized, Second: Serialize + ?Sized>(
    first: &First, second: &Second, ar: &mut BinaryWriteArchive,
) {
    Serialize_::serialize(first, ar);
    Serialize_::serialize(second, ar);
}

/// Read a map entry or pair in its canonical field order.
pub fn deserialize_pair_fields<First: Deserialize + ?Sized, Second: Deserialize + ?Sized>(
    first: &mut First, second: &mut Second, ar: &mut BinaryReadArchive,
) {
    Deserialize_::deserialize(first, ar);
    Deserialize_::deserialize(second, ar);
}

/// Write the common collection length and traverse each element once.
/// The caller supplies only element access and trait dispatch.
pub fn serialize_counted<WriteNext>(count: usize, ar: &mut BinaryWriteArchive, mut write_next: WriteNext)
where
    WriteNext: FnMut(&mut BinaryWriteArchive),
{
    Serialize_::serialize(&v64::new(count as i64), ar);
    let mut index = 0usize;
    while index < count {
        write_next(ar);
        index += 1usize;
    }
}

/// Read the common collection length, reset storage, and load each element.
/// Separate state arguments keep both callbacks from borrowing storage at once.
pub fn deserialize_counted<State, Prepare, ReadNext>(
    state: &mut State, ar: &mut BinaryReadArchive, mut prepare: Prepare, mut read_next: ReadNext,
)
where
    Prepare: FnMut(&mut State, usize),
    ReadNext: FnMut(&mut State, &mut BinaryReadArchive),
{
    let mut length = v64::new(0i64);
    Deserialize_::deserialize(&mut length, ar);
    let count = length.get() as usize;
    prepare(state, count);
    let mut index = 0usize;
    while index < count {
        read_next(state, ar);
        index += 1usize;
    }
}

/// Decode map entries in wire order, keeping the first value for each key.
/// The storage adapter must expose an insert-if-vacant primitive (for example,
/// Rust's `entry(key).or_insert(value)` or C++'s `try_emplace(key, value)`).
/// This avoids a separate membership lookup while consuming duplicate values.
pub fn deserialize_map_first<Key, Value, State, Clear, InsertIfVacant>(
    state: &mut State, ar: &mut BinaryReadArchive, mut clear: Clear,
    mut insert_if_vacant: InsertIfVacant,
)
where
    Key: Deserialize + Default,
    Value: Deserialize + Default,
    Clear: FnMut(&mut State),
    InsertIfVacant: FnMut(&mut State, Key, Value),
{
    let mut length = v64::new(0i64);
    Deserialize_::deserialize(&mut length, ar);
    clear(state);
    let count = length.get() as usize;
    let mut index = 0usize;
    while index < count {
        let mut key = Key::default();
        let mut value = Value::default();
        Deserialize_::deserialize(&mut key, ar);
        Deserialize_::deserialize(&mut value, ar);
        insert_if_vacant(state, key, value);
        index += 1usize;
    }
}

/// Write raw bytes with the canonical length prefix, without a text conversion.
///
/// # Safety
///
/// `data` must name `length` readable bytes for the synchronous archive write.
#[allow(unsafe_code)]
pub unsafe fn serialize_bytes(data: *const u8, length: usize, ar: &mut BinaryWriteArchive) {
    Serialize_::serialize(&v64::new(length as i64), ar);
    if length > 0usize {
        unsafe { ar.write_bytes(data, length) };
    }
}

/// Read an arbitrary byte sequence into caller-owned storage. Rust strings
/// separately validate UTF-8; C++ byte strings retain every byte.
///
/// # Safety
///
/// After `resize(state, length)`, `data(state)` must expose `length` writable
/// bytes that stay valid until the synchronous archive read finishes.
#[allow(unsafe_code)]
pub unsafe fn deserialize_bytes_with<State, Resize, Data>(
    state: &mut State, ar: &mut BinaryReadArchive, mut resize: Resize, mut data: Data,
)
where
    Resize: FnMut(&mut State, usize),
    Data: FnMut(&mut State) -> *mut u8,
{
    let mut length = v64::new(0i64);
    Deserialize_::deserialize(&mut length, ar);
    let count = length.get() as usize;
    resize(state, count);
    if count > 0usize {
        unsafe { ar.read_or_abort(data(state), count) };
    }
}

#[allow(unsafe_code)]
impl Serialize for String {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        unsafe { serialize_bytes(self.as_ptr(), self.len(), ar) };
    }
}

// Index loops, not `for e in self`: rusty::iter over these vector
// shapes in this position mis-yields (the element call deduced T = the
// whole container and landed on the poisoned catch-all).
impl<T: Serialize> Serialize for Vec<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.len() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut i: usize = 0usize;
        while i < self.len() {
            Serialize_::serialize(&self[i], ar);
            i += 1usize;
        }
    }
}

// rusty B-tree containers iterate Rust-style (no begin()/end()); the
// explicit iterator loop is the same shape their old C++ bodies used.
impl<T: Serialize> Serialize for std::collections::BTreeSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.len() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut it = self.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            Serialize_::serialize(e.unwrap(), ar);
        }
    }
}

impl<K: Serialize, V: Serialize> Serialize for std::collections::BTreeMap<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.len() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut it = self.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            Serialize_::serialize(kv.0, ar);
            Serialize_::serialize(kv.1, ar);
        }
    }
}

// The two hashbrown write bodies, same explicit-iterator shape as the
// B-tree pair above. Both target the std spelling: the transpiler lowers
// `std::collections::HashSet`/`HashMap` to the same `rusty::HashSet`/
// `rusty::HashMap` the facade names did, so the emitted overloads are
// unchanged. Under rustc `HashSet::iter()` yields `&T` directly (the
// facade walk over its `.map` field yielded a `(&T, &())` pair, which is
// what the old `kv.0` selected).
//
// WARNING (unchanged by this conversion): ANY hashbrown enumeration
// (iter()/begin()/drain()) routes through the `rusty::iter(table)`
// dispatcher in slice.hpp, whose return-type name crashes clang-22's
// Itanium mangler (SIGSEGV in mangleSourceName). These two templates
// MUST therefore stay UNINSTANTIATED — no production code serializes a
// rusty::HashSet/HashMap today, and the DECODER side (insert-only) is
// crash-free and is what the RustyHashSetPrimitives /
// RustyHashMapPrimitives tests exercise. If that ever changes, the
// encoder needs a mangler-safe enumeration path (or a fixed toolchain).
impl<T: Serialize> Serialize for std::collections::HashSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.len() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut it = self.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let elem = e.unwrap();
            Serialize_::serialize(elem, ar);
        }
    }
}

impl<K: Serialize, V: Serialize> Serialize for std::collections::HashMap<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.len() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut it = self.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            Serialize_::serialize(kv.0, ar);
            Serialize_::serialize(kv.1, ar);
        }
    }
}

// Rust tuples and C++ std::pair use the same field order.
impl<First: Serialize, Second: Serialize> Serialize for (First, Second) {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        serialize_pair_fields(&self.0, &self.1, ar);
    }
}

#[allow(non_snake_case, unsafe_code)]
pub mod Serialize_ {
    use super::{BinaryWriteArchive, Serialize};

    // Rust dispatches its canonical trait directly. The module epilogue defines
    // the C++ counterpart with the existing poison-scoped ADL call adapter.
    #[cfg_attr(any(), cpp_declaration)]
    pub fn adl_serialize_bridge<T: Serialize + ?Sized>(value: &T, archive: &mut BinaryWriteArchive) {
        Serialize::serialize(value, archive)
    }

    #[allow(non_snake_case)]
    pub mod adl_detail_ {
        use super::{BinaryWriteArchive, Serialize};

        pub fn dispatch_serialize<T: Serialize + ?Sized>(value: &T, archive: &mut BinaryWriteArchive) {
            super::adl_serialize_bridge(value, archive)
        }
    }

    pub fn serialize<T: Serialize + ?Sized>(value: &T, archive: &mut BinaryWriteArchive) {
        // SAFETY: the runtime bridge borrows both arguments only for this call
        // and performs a poison-scoped, ADL-only lookup. A missing overload
        // remains a hard C++ template-instantiation error.
        adl_detail_::dispatch_serialize(value, archive)
    }
}

/// First decoding failure observed while reading an archive.
///
/// `None` is zero so C++ aggregate construction of `BinaryReadArchive` keeps
/// working when older call sites provide only the source proxy.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    None = 0,
    InvalidUtf8 = 1,
}

pub struct BinaryReadArchive {
    pub source_: SourceProxy,
    error_: DecodeError,
}

#[allow(unsafe_code)]
impl BinaryReadArchive {
    pub fn new(source: SourceProxy) -> BinaryReadArchive {
        BinaryReadArchive {
            source_: source,
            error_: DecodeError::None,
        }
    }

    pub fn error(&self) -> DecodeError {
        self.error_
    }

    pub fn failed(&self) -> bool {
        self.error_ != DecodeError::None
    }

    /// Preserve the first failure so callers get the cause closest to the
    /// malformed bytes.
    pub fn record_error(&mut self, error: DecodeError) {
        if self.error_ == DecodeError::None {
            self.error_ = error;
        }
    }

    /// # Safety
    ///
    /// `p` and the concrete source retained by `self.source_` must satisfy all
    /// of [`SourceBase::read_bytes`]'s destination, non-overlap, and retained
    /// backing-storage requirements.
    pub unsafe fn read_exact(&mut self, p: *mut u8, n: usize) -> bool {
        let got: usize = unsafe { (*self.source_).read_bytes(p, n) };
        got == n
    }
    // Read exactly n bytes or abort — the operator>> truncation contract
    // (short reads at this layer are programming errors, not recoverable).
    // The DSL leaf Deserialize impls call this so the verify() lives once.
    /// # Safety
    ///
    /// `p` and the concrete source retained by `self.source_` must satisfy all
    /// of [`SourceBase::read_bytes`]'s destination, non-overlap, and retained
    /// backing-storage requirements.
    pub unsafe fn read_or_abort(&mut self, p: *mut u8, n: usize) {
        verify_at(self.read_exact(p, n), file!(), line!());
    }
}

pub trait Deserialize {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive);
}
#[allow(unsafe_code)]
impl Deserialize for v32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        unsafe { verify_at(ar.read_exact(b.as_mut_ptr(), 1), file!(), line!()) };
        let total = SparseInt::buf_size(b[0]);
        if total > 1 {
            // @unsafe - the tail read lands after the already-consumed
            // first byte (the retired `varint_tail` kernel's whole job).
            unsafe { verify_at(ar.read_exact(b.as_mut_ptr().add(1), total - 1), file!(), line!()) };
        }
        self.set(unsafe { SparseInt::load32(b.as_ptr()) });
    }
}
#[allow(unsafe_code)]
impl Deserialize for v64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        unsafe { verify_at(ar.read_exact(b.as_mut_ptr(), 1), file!(), line!()) };
        let total = SparseInt::buf_size(b[0]);
        if total > 1 {
            // @unsafe - the tail read lands after the already-consumed
            // first byte (the retired `varint_tail` kernel's whole job).
            unsafe { verify_at(ar.read_exact(b.as_mut_ptr().add(1), total - 1), file!(), line!()) };
        }
        self.set(unsafe { SparseInt::load64(b.as_ptr()) });
    }
}
#[allow(unsafe_code)]
impl Deserialize for i32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i32) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i32>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for i8 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i8) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i8>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for i16 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i16) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i16>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for i64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut i64) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<i64>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for u8 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = self as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u8>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for u16 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u16) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u16>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for u32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u32) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u32>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for u64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut u64) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<u64>());
        }
    }
}
#[allow(unsafe_code)]
impl Deserialize for f64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        unsafe {
            let p: *mut u8 = (self as *mut f64) as *mut u8;
            ar.read_or_abort(p, std::mem::size_of::<f64>());
        }
    }
}

#[allow(unsafe_code)]
impl Deserialize for String {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut bytes = Vec::<u8>::new();
        unsafe {
            deserialize_bytes_with(&mut bytes, ar,
                |storage: &mut Vec<u8>, length: usize| storage.resize(length, 0u8),
                |storage: &mut Vec<u8>| storage.as_mut_ptr());
        }
        match String::from_utf8(bytes) {
            Ok(mut value) => *self = std::mem::take(&mut value),
            Err(_) => {
                self.clear();
                ar.record_error(DecodeError::InvalidUtf8);
            }
        }
    }
}

// ---- Container impls (generic; emitted straight into Deserialize_,
// where the callers and the nested-container fwd-decls already look —
// unlike the serialize side, no forwarders are needed here). Wire
// format: v64 length prefix + N elements in order; containers cleared
// first, matching the Marshal operator>> semantics the old hand
// bodies preserved. The hashbrown decoders ARE safe to convert: they
// only insert (the clang-22 mangler crash is in ENUMERATION, which
// only the serialize side does).

impl<First: Deserialize, Second: Deserialize> Deserialize for (First, Second) {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        deserialize_pair_fields(&mut self.0, &mut self.1, ar);
    }
}

impl<T: Default + Deserialize> Deserialize for Vec<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        deserialize_counted(self, ar,
            |storage: &mut Vec<T>, count: usize| { storage.clear(); storage.reserve(count); },
            |storage: &mut Vec<T>, archive: &mut BinaryReadArchive| {
                let mut element: T = Default::default();
                Deserialize_::deserialize(&mut element, archive);
                storage.push(element);
            });
    }
}

impl<T: Default + Deserialize + Ord> Deserialize for std::collections::BTreeSet<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.insert(elem);
            i += 1usize;
        }
    }
}

impl<T: Default + Deserialize + Eq + std::hash::Hash> Deserialize for std::collections::HashSet<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.insert(elem);
            i += 1usize;
        }
    }
}

impl<K: Default + Deserialize + Ord, V: Default + Deserialize> Deserialize for std::collections::BTreeMap<K, V> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut key: K = Default::default();
            let mut value: V = Default::default();
            Deserialize_::deserialize(&mut key, ar);
            Deserialize_::deserialize(&mut value, ar);
            self.insert(key, value);
            i += 1usize;
        }
    }
}

impl<K: Default + Deserialize + Eq + std::hash::Hash, V: Default + Deserialize> Deserialize for std::collections::HashMap<K, V> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut key: K = Default::default();
            let mut value: V = Default::default();
            Deserialize_::deserialize(&mut key, ar);
            Deserialize_::deserialize(&mut value, ar);
            self.insert(key, value);
            i += 1usize;
        }
    }
}

#[allow(non_snake_case, unsafe_code)]
pub mod Deserialize_ {
    use super::{BinaryReadArchive, Deserialize};

    // Rust dispatches its canonical trait directly. The module epilogue defines
    // the C++ counterpart with the existing poison-scoped ADL call adapter.
    #[cfg_attr(any(), cpp_declaration)]
    pub fn adl_deserialize_bridge<T: Deserialize + ?Sized>(value: &mut T, archive: &mut BinaryReadArchive) {
        Deserialize::deserialize(value, archive)
    }

    #[allow(non_snake_case)]
    pub mod adl_detail_ {
        use super::{BinaryReadArchive, Deserialize};

        pub fn dispatch_deserialize<T: Deserialize + ?Sized>(value: &mut T, archive: &mut BinaryReadArchive) {
            super::adl_deserialize_bridge(value, archive)
        }
    }

    pub fn deserialize<T: Deserialize + ?Sized>(value: &mut T, archive: &mut BinaryReadArchive) {
        if archive.failed() {
            return;
        }
        adl_detail_::dispatch_deserialize(value, archive)
    }
}

/// Structural contract implemented by concrete application payloads.
/// The erased holder supplies type identity; payloads cannot forge it.
#[cfg_attr(any(), cpp_trait_member_dispatch)]
pub trait SerializablePayload {
    fn save(&self, ar: &mut BinaryWriteArchive);
    fn load(&mut self, ar: &mut BinaryReadArchive);
    fn kind(&self) -> i32;
}

mod sealed {
    #[cfg_attr(any(), cpp_marker_trait)]
    pub trait SerializableHolder {}
}

/// Erased interface implemented only by the canonical payload holder.
/// Sealing prevents safe callers from supplying an object that reports a
/// payload type without actually having that holder's memory layout.
///
/// ```compile_fail
/// use srpc::serializable::{SerializableBase, BinaryReadArchive, BinaryWriteArchive};
/// struct Forged;
/// impl SerializableBase for Forged {
///     fn save(&self, _: &mut BinaryWriteArchive) {}
///     fn load(&mut self, _: &mut BinaryReadArchive) {}
///     fn kind(&self) -> i32 { 61 }
///     fn payload_type_id(&self) -> std::any::TypeId { std::any::TypeId::of::<i64>() }
/// }
/// ```
pub trait SerializableBase: sealed::SerializableHolder {
    fn save(&self, ar: &mut BinaryWriteArchive);
    fn load(&mut self, ar: &mut BinaryReadArchive);
    fn kind(&self) -> i32;
    fn payload_type_id(&self) -> std::any::TypeId;
}

/// Shared ownership of a concrete canonical payload holder.
pub type SerializableProxy = Arc<dyn SerializableBase>;

/// Registry factories always own a callable; C++ emits `rusty::Function<SerializableProxy()>`.
pub type SerializableRegistryFactory = Box<dyn FnMut() -> SerializableProxy + Send>;

pub mod details {
    use super::{Arc, BinaryReadArchive, BinaryWriteArchive, SerializableBase, SerializablePayload, sealed};

    pub struct SerializableSharedPtrHolder<T> {
        pub ptr: Arc<T>,
    }

    impl<T: SerializablePayload + 'static> sealed::SerializableHolder for SerializableSharedPtrHolder<T> {}

    #[cfg_attr(any(), cpp_inherit)]
    impl<T: SerializablePayload + 'static> SerializableBase for SerializableSharedPtrHolder<T> {
        fn save(&self, ar: &mut BinaryWriteArchive) {
            self.ptr.save(ar)
        }
        // @unsafe - payload mutation requires one strong owner and no Weak
        // owners. A factory retaining either kind of additional owner is
        // rejected before load can mutate the payload.
        fn load(&mut self, ar: &mut BinaryReadArchive) {
            Arc::get_mut(&mut self.ptr).unwrap().load(ar)
        }
        fn kind(&self) -> i32 {
            self.ptr.kind()
        }
        fn payload_type_id(&self) -> std::any::TypeId {
            std::any::TypeId::of::<T>()
        }
    }
}

/// Recover the concrete holder after checking its runtime payload type.
///
/// # Safety
///
/// `base` must be null or point to a live `SerializableBase` implementation
/// for the duration of the call. The sealed base trait guarantees that a
/// matching payload type identifies this module's concrete holder. The
/// returned pointer borrows that same allocation and must never outlive it.
#[allow(unsafe_code)]
pub unsafe fn serializable_holder_of<T: 'static>(
    base: *const dyn SerializableBase,
) -> *const details::SerializableSharedPtrHolder<T> {
    if base.is_null() {
        return core::ptr::null();
    }
    if unsafe { (*base).payload_type_id() } != std::any::TypeId::of::<T>() {
        return core::ptr::null();
    }
    base as *const details::SerializableSharedPtrHolder<T>
}

#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct Serializable<const KIND: i32> {}

impl<const KIND: i32> Serializable<KIND> {
    #[cfg_attr(any(), cpp_noexcept)]
    pub const fn kind(&self) -> i32 {
        Self::static_kind()
    }

    #[cfg_attr(any(), cpp_noexcept)]
    pub const fn static_kind() -> i32 {
        assert!(
            KIND != 0i32,
            "Serializable kind 0 is reserved for unknown / unset"
        );
        KIND
    }
}

#[allow(unsafe_code)]
pub fn make_serializable_proxy_default<T: SerializablePayload + Default + 'static>() -> SerializableProxy {
    make_serializable_proxy(Arc::new(T::default()))
}

#[allow(unsafe_code)]
pub fn make_serializable_proxy_copy<T: SerializablePayload + Clone + 'static>(value: &T) -> SerializableProxy {
    make_serializable_proxy(Arc::new(value.clone()))
}

/// Share a concrete payload through the canonical type-erased holder.
pub fn make_serializable_proxy<T: SerializablePayload + 'static>(value: Arc<T>) -> SerializableProxy {
    Arc::<details::SerializableSharedPtrHolder<T>>::new(details::SerializableSharedPtrHolder { ptr: value })
}

pub struct SerializableRegistry {}

#[allow(unsafe_code)]
impl SerializableRegistry {
    // Register T under `kind` (returns 0 for static-initializer use).
    // The factory closure captures NOTHING — it only names T — so the
    // `[&]` lambda the DSL emits cannot dangle even though the
    // rusty::Function it becomes is stored in a process-wide map that
    // outlives this call. DO NOT introduce a captured local here
    // without re-checking that; a by-reference capture would dangle.
    // The proxy is holder-shaped so SerializableEnvelope::load gives
    // unpack_shared<T> a refcount-shared Arc<T>.
    pub fn reg<T: SerializablePayload + Default + 'static>(kind: i32) -> i32 {
        let factory: SerializableRegistryFactory = Box::new(|| -> SerializableProxy {
            make_serializable_proxy_default::<T>()
        });
        serializable_registry_register_factory(kind, factory);
        0i32
    }

    // Create a fresh proxy for the given kind; aborts if unregistered.
    pub fn create(kind: i32) -> SerializableProxy {
        serializable_registry_create_impl(kind)
    }

    pub fn is_registered(kind: i32) -> bool {
        serializable_registry_is_registered_impl(kind)
    }

    // Test helper; not thread-safe.
    pub fn clear_for_testing() {
        serializable_registry_clear_impl()
    }
}

struct SerializableRegistryMap {
    // Initialize the standard map under the registry lock on first use.
    // None keeps the static initializer independent of a const HashMap::new.
    map: Option<std::collections::HashMap<i32, SerializableRegistryFactory>>,
}

// The otherwise-unused parameter intentionally makes this a C++ function
// template: the generated lazy function-local registry then has linkonce
// linkage instead of adding a new externally strong provider symbol.
#[allow(clippy::extra_unused_type_parameters)]
fn registry<T>() -> &'static std::sync::Mutex<SerializableRegistryMap> {
    static R: std::sync::Mutex<SerializableRegistryMap> = std::sync::Mutex::new(SerializableRegistryMap {
        map: None,
    });
    &R
}

pub fn serializable_registry_register_factory(
    kind: i32,
    factory: SerializableRegistryFactory,
) {
    let mut guard = registry::<SerializableRegistryMap>().lock().unwrap();
    if guard.map.is_none() {
        guard.map = Some(std::collections::HashMap::new());
    }
    guard.map.as_mut().unwrap().insert(kind, factory);
}

#[allow(unsafe_code)]
pub fn serializable_registry_create_impl(kind: i32) -> SerializableProxy {
    let mut guard = registry::<SerializableRegistryMap>().lock().unwrap();
    verify_at(guard.map.is_some(), file!(), line!());
    let entry = guard.map.as_mut().unwrap().get_mut(&kind);
    verify_at(entry.is_some(), file!(), line!());
    entry.unwrap()()
}

pub fn serializable_registry_is_registered_impl(kind: i32) -> bool {
    let guard = registry::<SerializableRegistryMap>().lock().unwrap();
    if guard.map.is_none() {
        return false;
    }
    guard.map.as_ref().unwrap().get(&kind).is_some()
}

pub fn serializable_registry_clear_impl() {
    let mut guard = registry::<SerializableRegistryMap>().lock().unwrap();
    if let Some(map) = guard.map.as_mut() {
        map.clear();
    }
}
