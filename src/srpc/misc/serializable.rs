//! Canonical Rust prototype for `srpc.serializable`.

#![allow(non_camel_case_types, non_snake_case)]

use cpp::srpc::basetypes as cpp_basetypes;
use cpp::srpc::debugging as cpp_debugging;
use cpp::rusty as cpp_rusty;
#[allow(unused_imports)]
use cpp::std as _;
use rusty as cpp;
use rusty::Arc;

// Keep these module-owned aliases explicit.  Pulling the types through an
// ordinary Rust `use crate::basetypes::{...}` made the C++ emitter invent a
// nonexistent nested `srpc::basetypes` namespace; the actual provider exports
// both directly from `srpc`.
pub type v32 = rusty::SerializableV32;
pub type v64 = rusty::SerializableV64;

#[allow(unsafe_code)]
unsafe extern "C" {
    pub fn srpc_fd_write_all(fd: i32, pointer: *const rusty::LegacyCVoid, length: usize);
    pub fn srpc_fd_read_upto(fd: i32, pointer: *mut rusty::LegacyCVoid, length: usize) -> usize;
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
    rusty::make_box::<rusty::RustcSinkBaseAdapterRefMut<BufferSink>>(unsafe { &mut *sink })
}

/// # Safety
///
/// `source` must be non-null, uniquely borrowed, and remain alive and unmoved
/// for every use of the returned proxy. Its public raw backing fields must
/// satisfy [`SourceBase::read_bytes`]'s retained-source contract throughout
/// that lifetime.
#[allow(unsafe_code)]
pub unsafe fn make_source_proxy_buffer(source: *mut BufferSource) -> SourceProxy {
    rusty::make_box::<rusty::RustcSourceBaseAdapterRefMut<BufferSource>>(unsafe { &mut *source })
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
        unsafe { srpc_fd_write_all(self.fd_, p as *const rusty::LegacyCVoid, n) };
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
        unsafe { srpc_fd_read_upto(self.fd_, p as *mut rusty::LegacyCVoid, n) }
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
    rusty::make_box::<rusty::RustcSinkBaseAdapterRefMut<FdSink>>(unsafe { &mut *sink })
}

/// # Safety
///
/// `source` must be non-null, uniquely borrowed, and remain alive and unmoved
/// for every use of the returned proxy. Its file descriptor must remain open
/// for reading and externally serialized for that lifetime; this function
/// does not take descriptor ownership.
#[allow(unsafe_code)]
pub unsafe fn make_source_proxy_fd(source: *mut FdSource) -> SourceProxy {
    rusty::make_box::<rusty::RustcSourceBaseAdapterRefMut<FdSource>>(unsafe { &mut *source })
}

pub struct BinaryWriteArchive {
    pub sink_: SinkProxy,
}

#[allow(unsafe_code)]
impl BinaryWriteArchive {
    // Emit raw bytes (used for unstructured payloads).
    // @unsafe - virtual write through the type-erased sink proxy.
    // The explicit `(*self.sink_)` deref is LOAD-BEARING: SinkProxy is a
    // hand-written C++ alias, so the transpiler cannot see the Box
    // behind it and a bare `self.sink_.write_bytes(..)` lowers to a `.`
    // member access on the handle (which does not compile). The deref
    // lowers through rusty::detail::deref_if_pointer_like, i.e. exactly
    // the `(*sink_).write_bytes(..)` the old kernel spelled `sink_->`.
    /// # Safety
    ///
    /// `p` must satisfy [`SinkBase::write_bytes`]'s readable-buffer contract.
    pub unsafe fn write_bytes(&mut self, p: *const u8, n: usize) {
        unsafe { cpp_rusty::srpc_sink_write(&mut *self.sink_, p, n) }
    }
}

pub trait Serialize {
    fn serialize(&self, ar: &mut BinaryWriteArchive);
}
#[allow(unsafe_code)]
impl Serialize for rusty::SerializableV32 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        let bsize = unsafe { cpp_basetypes::SparseInt::dump32(self.get(), b.as_mut_ptr()) };
        unsafe { ar.write_bytes(b.as_ptr(), bsize) };
    }
}
#[allow(unsafe_code)]
impl Serialize for rusty::SerializableV64 {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        let bsize = unsafe { cpp_basetypes::SparseInt::dump64(self.get(), b.as_mut_ptr()) };
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

// ---- Variable-length byte sequences: v64 length prefix + raw bytes.
// BOTH leaves carry the body (rather than rusty::LoggingString forwarding to a
// rusty::SerializableStdStringView temporary, as the old hand pair did): a
// `rusty::SerializableStdStringView{self}` conversion has no DSL spelling. The wire
// bytes are identical either way.
//
// `self.data() as *const u8` lowers to
// rusty::detail::ptr_cast<const uint8_t*>, replacing the hand
// reinterpret_cast. The length write MUST be QUALIFIED — unqualified
// lookup inside the generated namespace finds the sibling it just
// emitted and stops (the same hazard the container impls below avoid).
//
// Only behavioural delta vs. the deleted hand overloads: string_view
// goes from by-value to `const rusty::SerializableStdStringView&`; rvalues bind to that
// reference with identical semantics.
#[allow(unsafe_code)]
impl Serialize for rusty::SerializableStdStringView {
    #[allow(clippy::unnecessary_cast)]
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        if self.size() > 0usize {
            // The cast is redundant in the rustc facade but load-bearing in
            // production: its mapped C++ type is `std::string_view`, whose
            // `data()` returns `const char*`.
            let p: *const u8 = self.data() as *const u8;
            unsafe { ar.write_bytes(p, self.size()) };
        }
    }
}
#[allow(unsafe_code)]
impl Serialize for rusty::LoggingString {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        if self.size() > 0usize {
            let p: *const u8 = unsafe { self.data() } as *const u8;
            unsafe { ar.write_bytes(p, self.size()) };
        }
    }
}
impl<T> Serialize for rusty::SerializableStdList<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

// Index loops, not `for e in self`: rusty::iter over these vector
// shapes in this position mis-yields (the element call deduced T = the
// whole container and landed on the poisoned catch-all).
impl<T> Serialize for Vec<T> {
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

impl<T> Serialize for rusty::SerializableStdVector<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut i: usize = 0usize;
        while i < self.size() {
            Serialize_::serialize(&self[i], ar);
            i += 1usize;
        }
    }
}

impl<T> Serialize for rusty::SerializableStdSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

impl<T> Serialize for rusty::SerializableStdUnorderedSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        for e in self {
            Serialize_::serialize(e, ar);
        }
    }
}

impl<K, V> Serialize for rusty::SerializableStdMap<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        for kv in self {
            Serialize_::serialize(&kv.first, ar);
            Serialize_::serialize(&kv.second, ar);
        }
    }
}

impl<K, V> Serialize for rusty::SerializableStdUnorderedMap<K, V> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.size() as i64);
        Serialize_::serialize(&v_len, ar);
        for kv in self {
            Serialize_::serialize(&kv.first, ar);
            Serialize_::serialize(&kv.second, ar);
        }
    }
}

// rusty B-tree containers iterate Rust-style (no begin()/end()); the
// explicit iterator loop is the same shape their old C++ bodies used.
impl<T> Serialize for rusty::BTreeSet<T> {
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

impl<K, V> Serialize for rusty::BTreeMap<K, V> {
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
            Serialize_::serialize(&kv.0, ar);
            Serialize_::serialize(&kv.1, ar);
        }
    }
}

// The two hashbrown write bodies, same explicit-iterator shape as the
// B-tree pair above. HashSet has no const begin()/end() of its own, so
// it walks the underlying HashMap field: `self.map.iter()` lowers to
// `rusty::iter(self_.map)`, whose next() yields
// Option<tuple<const T&, const monostate&>> — hence the `kv.0`.
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
impl<T> Serialize for rusty::HashSet<T> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        let v_len: v64 = v64::new(self.len() as i64);
        Serialize_::serialize(&v_len, ar);
        let mut it = self.map.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            Serialize_::serialize(&kv.0, ar);
        }
    }
}

impl<K, V> Serialize for rusty::HashMap<K, V> {
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
            Serialize_::serialize(&kv.0, ar);
            Serialize_::serialize(&kv.1, ar);
        }
    }
}

// rusty::StdPair: write first then second, no length prefix (each side
// already knows the type and consumes its own bytes). It stays last in
// the trait block; codegen emits every Serialize_ overload declaration
// before any definition, so both element calls see the complete set.
impl<T1, T2> Serialize for rusty::StdPair<T1, T2> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        Serialize_::serialize(&self.first, ar);
        Serialize_::serialize(&self.second, ar);
    }
}

#[allow(non_snake_case, unsafe_code)]
pub mod Serialize_ {
    use super::{cpp, BinaryWriteArchive};
    use cpp::rusty as cpp_rusty;

    fn adl_serialize_bridge<T>(value: &T, archive: &mut BinaryWriteArchive) {
        // SAFETY: the runtime bridge borrows both arguments only for this call
        // and performs a poison-scoped, ADL-only lookup. A missing overload
        // remains a hard C++ template-instantiation error.
        unsafe { cpp_rusty::srpc_adl_serialize(value, archive) }
    }

    #[allow(non_snake_case)]
    pub mod adl_detail_ {
        use super::BinaryWriteArchive;

        // Historical lookup poison: declaration only, deliberately undefined.
        unsafe extern "Rust" {
            pub fn serialize();
        }

        pub fn dispatch_serialize<T>(value: &T, archive: &mut BinaryWriteArchive) {
            super::adl_serialize_bridge(value, archive)
        }
    }

    pub fn serialize<T>(value: &T, archive: &mut BinaryWriteArchive) {
        // SAFETY: the runtime bridge borrows both arguments only for this call
        // and performs a poison-scoped, ADL-only lookup. A missing overload
        // remains a hard C++ template-instantiation error.
        adl_detail_::dispatch_serialize(value, archive)
    }
}

pub struct BinaryReadArchive {
    pub source_: SourceProxy,
}

#[allow(unsafe_code)]
impl BinaryReadArchive {
    // Read into raw bytes; false if the source ran out.
    // @unsafe - virtual read through the type-erased source proxy.
    // The explicit `(*self.source_)` deref is load-bearing for exactly
    // the same reason as BinaryWriteArchive::write_bytes above:
    // SourceProxy is a hand-written C++ alias, so the transpiler cannot
    // see the Box behind it and would emit a `.` on the handle.
    /// # Safety
    ///
    /// `p` and the concrete source retained by `self.source_` must satisfy all
    /// of [`SourceBase::read_bytes`]'s destination, non-overlap, and retained
    /// backing-storage requirements.
    pub unsafe fn read_exact(&mut self, p: *mut u8, n: usize) -> bool {
        let got: usize = unsafe { cpp_rusty::srpc_source_read(&mut *self.source_, p, n) };
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
        unsafe { cpp_debugging::verify(self.read_exact(p, n)) };
    }
}

pub trait Deserialize {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive);
}
#[allow(unsafe_code)]
impl Deserialize for rusty::SerializableV32 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        unsafe { cpp_debugging::verify(ar.read_exact(b.as_mut_ptr(), 1)) };
        let total = unsafe { cpp_basetypes::SparseInt::buf_size(b[0]) };
        if total > 1 {
            // @unsafe - the tail read lands after the already-consumed
            // first byte (the retired `varint_tail` kernel's whole job).
            unsafe { cpp_debugging::verify(ar.read_exact(b.as_mut_ptr().add(1), total - 1)) };
        }
        self.set(unsafe { cpp_basetypes::SparseInt::load32(b.as_ptr()) });
    }
}
#[allow(unsafe_code)]
impl Deserialize for rusty::SerializableV64 {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut b: [u8; 9] = [0u8; 9];
        unsafe { cpp_debugging::verify(ar.read_exact(b.as_mut_ptr(), 1)) };
        let total = unsafe { cpp_basetypes::SparseInt::buf_size(b[0]) };
        if total > 1 {
            // @unsafe - the tail read lands after the already-consumed
            // first byte (the retired `varint_tail` kernel's whole job).
            unsafe { cpp_debugging::verify(ar.read_exact(b.as_mut_ptr().add(1), total - 1)) };
        }
        self.set(unsafe { cpp_basetypes::SparseInt::load64(b.as_ptr()) });
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

// Read-side mirror of the string serialize leaf: v64 length prefix,
// resize, then read the bytes straight into the string's buffer.
// @unsafe { writing into rusty::LoggingString's internal buffer }
// `self.data() as *mut u8` picks the C++17 non-const data() overload
// (the receiver is `rusty::LoggingString&`) and lowers to
// rusty::detail::ptr_cast<uint8_t*>, replacing the old
// `reinterpret_cast<uint8_t*>(&self_[0])`. verify() keeps the
// abort-on-truncation contract the hand kernel had.
#[allow(unsafe_code)]
impl Deserialize for rusty::LoggingString {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        let len: usize = v_len.get() as usize;
        self.resize(len);
        if len > 0usize {
            let p: *mut u8 = unsafe { self.data() } as *mut u8;
            unsafe { cpp_debugging::verify(ar.read_exact(p, len)) };
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

impl<T1, T2> Deserialize for rusty::StdPair<T1, T2> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        Deserialize_::deserialize(&mut self.first, ar);
        Deserialize_::deserialize(&mut self.second, ar);
    }
}

impl<T: Default> Deserialize for Vec<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        self.reserve(n);
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.push(elem);
            i += 1usize;
        }
    }
}

impl<T: Default> Deserialize for rusty::SerializableStdVector<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        self.reserve(n);
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.push_back(elem);
            i += 1usize;
        }
    }
}

impl<T: Default> Deserialize for rusty::SerializableStdList<T> {
    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {
        let mut v_len = v64::new(0i64);
        Deserialize_::deserialize(&mut v_len, ar);
        self.clear();
        let n: usize = v_len.get() as usize;
        let mut i: usize = 0usize;
        while i < n {
            let mut elem: T = Default::default();
            Deserialize_::deserialize(&mut elem, ar);
            self.push_back(elem);
            i += 1usize;
        }
    }
}

impl<T: Default> Deserialize for rusty::BTreeSet<T> {
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

impl<T: Default> Deserialize for rusty::SerializableStdSet<T> {
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

impl<T: Default> Deserialize for rusty::HashSet<T> {
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

impl<T: Default> Deserialize for rusty::SerializableStdUnorderedSet<T> {
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

impl<K: Default, V: Default> Deserialize for rusty::BTreeMap<K, V> {
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

impl<K: Default, V: Default> Deserialize for rusty::SerializableStdMap<K, V> {
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
            self.emplace(key, value);
            i += 1usize;
        }
    }
}

impl<K: Default, V: Default> Deserialize for rusty::HashMap<K, V> {
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

impl<K: Default, V: Default> Deserialize for rusty::SerializableStdUnorderedMap<K, V> {
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
            self.emplace(key, value);
            i += 1usize;
        }
    }
}

#[allow(non_snake_case, unsafe_code)]
pub mod Deserialize_ {
    use super::{cpp, BinaryReadArchive};
    use cpp::rusty as cpp_rusty;

    fn adl_deserialize_bridge<T>(value: &mut T, archive: &mut BinaryReadArchive) {
        // SAFETY: same bounded ADL bridge contract as the write side, with the
        // payload mutably borrowed for the duration of the call only.
        unsafe { cpp_rusty::srpc_adl_deserialize(value, archive) }
    }

    #[allow(non_snake_case)]
    pub mod adl_detail_ {
        use super::BinaryReadArchive;

        // Historical lookup poison: declaration only, deliberately undefined.
        unsafe extern "Rust" {
            pub fn deserialize();
        }

        pub fn dispatch_deserialize<T>(value: &mut T, archive: &mut BinaryReadArchive) {
            super::adl_deserialize_bridge(value, archive)
        }
    }

    pub fn deserialize<T>(value: &mut T, archive: &mut BinaryReadArchive) {
        adl_detail_::dispatch_deserialize(value, archive)
    }
}

pub trait SerializableBase {
    fn save(&self, ar: &mut BinaryWriteArchive);
    fn load(&mut self, ar: &mut BinaryReadArchive);
    fn kind(&self) -> i32;
    fn payload_type_id(&self) -> std::any::TypeId;
}

/// Public owning proxy ABI.  The rustc facade models the same Arc-shaped
/// handle; the production type map spells its pointee as this module's
/// generated `SerializableBase` interface.
pub type SerializableProxy = rusty::SerializableProxy;

/// Public registry-factory ABI (`rusty::Function<SerializableProxy()>`).
pub type SerializableRegistryFactory = rusty::SerializableRegistryFactory;

pub mod details {
    use super::{Arc, BinaryReadArchive, BinaryWriteArchive, SerializableBase};
    use rusty::cpp_inherit;

    pub struct SerializableSharedPtrHolder<T> {
        pub ptr: Arc<T>,
    }

    #[cpp_inherit]
    impl<T: SerializableBase + 'static> SerializableBase for SerializableSharedPtrHolder<T> {
        fn save(&self, ar: &mut BinaryWriteArchive) {
            self.ptr.save(ar)
        }
        // @unsafe - unique-owner mutation window: load always runs on a
        // factory-fresh proxy (registry create -> strong_count 1), so
        // get_mut() is Some; a shared proxy here would be a bug and panics
        // loudly instead of silently mutating shared state.
        fn load(&mut self, ar: &mut BinaryReadArchive) {
            self.ptr.get_mut().unwrap().load(ar)
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
/// for the duration of the call. If that object reports
/// `TypeId::of::<T>()`, its dynamic object must actually be this module's
/// `details::SerializableSharedPtrHolder<T>`, created by the corresponding
/// proxy factory; an arbitrary `SerializableBase` implementation must not
/// forge another payload's `TypeId`. The returned pointer borrows that same
/// allocation and must never outlive it.
#[allow(unsafe_code)]
pub unsafe fn serializable_holder_of<T: 'static>(
    base: *const rusty::SerializableBase,
) -> *const rusty::SerializableSharedPtrHolder<T> {
    if base.is_null() {
        return core::ptr::null();
    }
    if unsafe { (*base).payload_type_id() } != std::any::TypeId::of::<T>() {
        return core::ptr::null();
    }
    base as *const rusty::SerializableSharedPtrHolder<T>
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
pub fn make_serializable_proxy_default<T: 'static>() -> rusty::SerializableProxy {
    // SAFETY: the C++ helper allocates T and immediately moves its owning Arc
    // into the only SerializableBase holder implementation.
    let sp: Arc<T> = unsafe { cpp_rusty::srpc_arc_default::<T>() };
    unsafe { cpp_rusty::srpc_holder_proxy::<rusty::SerializableSharedPtrHolder<T>, T>(sp) }
}

#[allow(unsafe_code)]
pub fn make_serializable_proxy_copy<T: 'static>(value: &T) -> rusty::SerializableProxy {
    // SAFETY: the helper copy-constructs T from a live shared reference and
    // transfers the resulting Arc into the holder proxy.
    let sp: Arc<T> = unsafe { cpp_rusty::srpc_arc_copy(value) };
    unsafe { cpp_rusty::srpc_holder_proxy::<rusty::SerializableSharedPtrHolder<T>, T>(sp) }
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
    pub fn reg<T: 'static>(kind: i32) -> i32 {
        let factory = unsafe {
            cpp_rusty::srpc_factory_from_callable(|| -> rusty::SerializableProxy {
                let sp: Arc<T> = cpp_rusty::srpc_arc_default::<T>();
                cpp_rusty::srpc_holder_proxy::<rusty::SerializableSharedPtrHolder<T>, T>(sp)
            })
        };
        serializable_registry_register_factory(kind, factory);
        0i32
    }

    // Create a fresh proxy for the given kind; aborts if unregistered.
    pub fn create(kind: i32) -> rusty::SerializableProxy {
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
    map: rusty::HashMap<i32, rusty::SerializableRegistryFactory>,
}

// The otherwise-unused parameter intentionally makes this a C++ function
// template: the generated lazy function-local registry then has linkonce
// linkage instead of adding a new externally strong provider symbol.
#[allow(clippy::extra_unused_type_parameters)]
fn registry<T>() -> &'static rusty::Mutex<SerializableRegistryMap> {
    static R: rusty::Mutex<SerializableRegistryMap> = rusty::Mutex::new(SerializableRegistryMap {
        map: rusty::HashMap::new(),
    });
    &R
}

#[allow(clippy::explicit_auto_deref)]
pub fn serializable_registry_register_factory(
    kind: i32,
    factory: rusty::SerializableRegistryFactory,
) {
    let mut guard = registry::<SerializableRegistryMap>().lock().unwrap();
    (*guard).map.insert(kind, factory);
}

#[allow(clippy::explicit_auto_deref)]
#[allow(unsafe_code)]
pub fn serializable_registry_create_impl(kind: i32) -> rusty::SerializableProxy {
    let guard = registry::<SerializableRegistryMap>().lock().unwrap();
    let entry = (*guard).map.get(&kind);
    unsafe { cpp_debugging::verify(entry.is_some()) };
    entry.unwrap()()
}

#[allow(clippy::explicit_auto_deref)]
pub fn serializable_registry_is_registered_impl(kind: i32) -> bool {
    let guard = registry::<SerializableRegistryMap>().lock().unwrap();
    (*guard).map.get(&kind).is_some()
}

#[allow(clippy::explicit_auto_deref)]
pub fn serializable_registry_clear_impl() {
    let mut guard = registry::<SerializableRegistryMap>().lock().unwrap();
    (*guard).map.clear();
}
