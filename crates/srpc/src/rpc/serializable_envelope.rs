//! Closed-set, type-erased serializable payload envelope.
//!
//! This valid-Rust owner preserves the public surface of the legacy
//! `rrr.serializable_envelope` C++ module.  The generated C++ keeps the
//! established `rusty::Arc<SerializableBase>` carrier, nominal
//! `PayloadMember<Set, T>` membership constraints, cached public `kind_`, and
//! `[v32 kind][payload]` archive format.  Native Rust uses the Cargo-only
//! reserved `cpp::` shim at the bottom of this file to provide the equivalent
//! type erasure, checked downcast, archive, and registry behavior.

#![allow(
    ambiguous_wide_pointer_comparisons,
    unsafe_code,
    unused_mut,
    unused_unsafe
)]

use crate::base::legacy_basetypes::v32;
use cpp::rrr::debugging as cpp_debugging;
use cpp::rrr::serializable as cpp_serializable;
use cpp::rusty as cpp_rusty;
use std::marker::PhantomData;
use std::sync::Arc;

/// Nominal membership in a closed payload set.
///
/// The generated C++ primary template has `value == false`; downstream C++
/// specializations emitted from the existing inline registrations set both
/// `value` and `KIND`.  Native Rust implementations supply the same fixed
/// discriminant through this associated constant.
#[cfg_attr(any(), cpp_marker_trait)]
pub trait PayloadMember<Set> {
    const KIND: i32;
}

pub unsafe fn envelope_holder_ptr_mut<T>(
    holder: *const cpp_serializable::details::SerializableSharedPtrHolder<T>,
) -> *mut T {
    let pointer: *const T = unsafe { cpp_rusty::Arc::<T>::get(&(*holder).ptr) };
    pointer as *mut T
}

/// Arc-backed closed-set serializable sum type.
pub struct SerializableEnvelope<PayloadSet> {
    /// Cached payload kind.  This field is intentionally public for deptran's
    /// long-standing direct `cmd.kind_` checks.
    pub kind_: i32,
    inner_: Option<cpp_serializable::SerializableProxy>,
    _payload_set: [PhantomData<PayloadSet>; 0],
}

impl<PayloadSet> SerializableEnvelope<PayloadSet> {
    fn base_ptr(&self) -> *const cpp_serializable::SerializableBase {
        if self.inner_.is_none() {
            // The indexed C++ projection is the existing thin-pointer
            // `rusty::ptr::null()`.  Native Rust safely returns a live empty
            // sentinel trait object; checked holder recovery treats it as a
            // type mismatch.  No invalid trait-object metadata is fabricated.
            return unsafe { cpp_rusty::ptr::null() };
        }
        unsafe {
            cpp_rusty::Arc::<cpp_serializable::SerializableBase>::get(self.inner_.as_ref().unwrap())
        }
    }

    fn refresh_kind(&mut self) {
        if self.inner_.is_none() {
            self.kind_ = 0_i32;
            return;
        }
        let base = self.base_ptr();
        self.kind_ = unsafe { (*base).kind() };
    }

    pub fn has_value(&self) -> bool {
        self.inner_.is_some()
    }

    pub fn kind(&self) -> i32 {
        if self.inner_.is_none() {
            return 0_i32;
        }
        let base = self.base_ptr();
        unsafe { (*base).kind() }
    }

    /// Copy `value` into a newly allocated payload holder.
    pub fn pack<T>(value: &T) -> SerializableEnvelope<PayloadSet>
    where
        T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + Clone + 'static,
    {
        SerializableEnvelope::<PayloadSet>::pack_aliased::<T>(Arc::<T>::new(value.clone()))
    }

    /// Retain the caller's Arc, preserving payload identity and mutations.
    pub fn pack_aliased<T>(sp: Arc<T>) -> SerializableEnvelope<PayloadSet>
    where
        T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + 'static,
    {
        let mut envelope: SerializableEnvelope<PayloadSet> = Default::default();
        let holder = unsafe { cpp_serializable::details::SerializableSharedPtrHolder::<T>(sp) };
        envelope.inner_ = Some(Arc::<
            cpp_serializable::details::SerializableSharedPtrHolder<T>,
        >::new(holder));
        envelope.refresh_kind();
        envelope
    }

    pub fn unpack<T>(&self) -> *const T
    where
        T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + 'static,
    {
        let holder = unsafe { cpp_serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if holder.is_null() {
            return core::ptr::null();
        }
        unsafe { cpp_rusty::Arc::<T>::get(&(*holder).ptr) }
    }

    pub fn unpack_shared<T>(&self) -> Option<Arc<T>>
    where
        T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + 'static,
    {
        let holder = unsafe { cpp_serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if holder.is_null() {
            return None;
        }
        Some(unsafe { (*holder).ptr.clone() })
    }

    pub fn is_a<T>(&self) -> bool
    where
        T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + 'static,
    {
        let pointer: *const T = self.unpack::<T>();
        !pointer.is_null()
    }

    /// Write `[v32 kind][payload bytes]` through the established archive.
    pub fn save(&self, archive: &mut cpp_serializable::BinaryWriteArchive) {
        unsafe { cpp_debugging::verify(self.has_value()) };
        let base = self.base_ptr();
        unsafe {
            cpp_serializable::Serialize_::serialize(v32::new((*base).kind()), archive);
            (*base).save(archive);
        }
    }

    /// Reconstruct a registered holder from `[v32 kind][payload bytes]`.
    pub fn load(&mut self, archive: &mut cpp_serializable::BinaryReadArchive) {
        let mut kind_value = v32::new(0_i32);
        unsafe { cpp_serializable::Deserialize_::deserialize(&mut kind_value, archive) };
        let kind: i32 = kind_value.get();
        let mut proxy: cpp_serializable::SerializableProxy =
            unsafe { cpp_serializable::SerializableRegistry::create(kind) };
        Arc::get_mut(&mut proxy).unwrap().load(archive);
        self.inner_ = Some(proxy);
        self.refresh_kind();
    }

    /// Explicit mutable escape retained for the legacy `T*` contract.
    pub fn unpack_mut<T>(&mut self) -> *mut T
    where
        T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + 'static,
    {
        let holder = unsafe { cpp_serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if holder.is_null() {
            return core::ptr::null_mut();
        }
        let pointer: *const T = unsafe { cpp_rusty::Arc::<T>::get(&(*holder).ptr) };
        pointer as *mut T
    }
}

impl<PayloadSet> Default for SerializableEnvelope<PayloadSet> {
    #[cfg_attr(any(), cpp_ctor)]
    fn default() -> SerializableEnvelope<PayloadSet> {
        SerializableEnvelope {
            kind_: 0_i32,
            inner_: None,
            _payload_set: [],
        }
    }
}

impl<PayloadSet> Clone for SerializableEnvelope<PayloadSet> {
    fn clone(&self) -> SerializableEnvelope<PayloadSet> {
        let mut envelope: SerializableEnvelope<PayloadSet> = Default::default();
        envelope.kind_ = self.kind_;
        envelope.inner_ = self.inner_.clone();
        envelope
    }
}

impl<PayloadSet> PartialEq for SerializableEnvelope<PayloadSet> {
    fn eq(&self, other: &SerializableEnvelope<PayloadSet>) -> bool {
        if self.inner_.is_none() && other.inner_.is_none() {
            return true;
        }
        if self.inner_.is_none() || other.inner_.is_none() {
            return false;
        }
        self.base_ptr() == other.base_ptr()
    }
}

/// Migration compatibility: callers spell `marshallable_cast::<T>(env)`.
pub fn marshallable_cast<T, PayloadSet>(
    envelope: &SerializableEnvelope<PayloadSet>,
) -> Option<Arc<T>>
where
    T: PayloadMember<PayloadSet> + cpp_serializable::SerializablePayload + 'static,
{
    envelope.unpack_shared::<T>()
}

pub fn serialize<PayloadSet>(
    envelope: &SerializableEnvelope<PayloadSet>,
    archive: &mut cpp_serializable::BinaryWriteArchive,
) {
    envelope.save(archive);
}

pub fn deserialize<PayloadSet>(
    envelope: &mut SerializableEnvelope<PayloadSet>,
    archive: &mut cpp_serializable::BinaryReadArchive,
) {
    envelope.load(archive);
}

// Cargo-only implementations of the reserved `cpp::` imports.  The C++
// consumer suppresses this module and resolves the referenced names against
// the module-local, fail-closed symbol index.
#[allow(dead_code)]
pub mod cpp {
    pub mod rrr {
        pub mod debugging {
            pub unsafe fn verify(condition: bool) {
                assert!(condition);
            }
        }

        pub mod serializable {
            use crate::base::legacy_basetypes::{v32, SparseInt};
            use std::any::Any;
            use std::collections::HashMap;
            use std::sync::{Arc, Mutex, OnceLock};

            /// Native payload half of the legacy `SerializableBase` contract.
            pub trait SerializablePayload: Any + Send + Sync {
                fn kind(&self) -> i32;
                fn save(&self, archive: &mut BinaryWriteArchive);
                fn load(&mut self, archive: &mut BinaryReadArchive);
            }

            pub trait SerializableBaseApi: Any + Send + Sync {
                fn kind(&self) -> i32;
                fn save(&self, archive: &mut BinaryWriteArchive);
                fn load(&mut self, archive: &mut BinaryReadArchive);
                fn as_any(&self) -> &dyn Any;
            }

            pub type SerializableBase = dyn SerializableBaseApi;
            pub type SerializableProxy = Arc<SerializableBase>;

            pub mod details {
                use super::{
                    BinaryReadArchive, BinaryWriteArchive, SerializableBaseApi, SerializablePayload,
                };
                use std::any::Any;
                use std::sync::Arc;

                pub struct SerializableSharedPtrHolder<T> {
                    pub ptr: Arc<T>,
                }

                impl<T> From<Arc<T>> for SerializableSharedPtrHolder<T> {
                    fn from(ptr: Arc<T>) -> SerializableSharedPtrHolder<T> {
                        SerializableSharedPtrHolder { ptr }
                    }
                }

                #[allow(non_snake_case)]
                pub fn SerializableSharedPtrHolder<T>(
                    ptr: Arc<T>,
                ) -> SerializableSharedPtrHolder<T> {
                    SerializableSharedPtrHolder { ptr }
                }

                impl<T: SerializablePayload> SerializableBaseApi for SerializableSharedPtrHolder<T> {
                    fn kind(&self) -> i32 {
                        self.ptr.kind()
                    }

                    fn save(&self, archive: &mut BinaryWriteArchive) {
                        self.ptr.save(archive);
                    }

                    fn load(&mut self, archive: &mut BinaryReadArchive) {
                        Arc::get_mut(&mut self.ptr).unwrap().load(archive);
                    }

                    fn as_any(&self) -> &dyn Any {
                        self
                    }
                }
            }

            pub unsafe fn serializable_holder_of<T: 'static>(
                base: *const SerializableBase,
            ) -> *const details::SerializableSharedPtrHolder<T> {
                if base.is_null() {
                    return core::ptr::null();
                }
                let base_ref: &SerializableBase = &*base;
                match base_ref
                    .as_any()
                    .downcast_ref::<details::SerializableSharedPtrHolder<T>>()
                {
                    Some(holder) => holder as *const details::SerializableSharedPtrHolder<T>,
                    None => core::ptr::null(),
                }
            }

            #[derive(Default)]
            pub struct BinaryWriteArchive {
                bytes: Vec<u8>,
            }

            impl BinaryWriteArchive {
                pub fn new() -> BinaryWriteArchive {
                    BinaryWriteArchive { bytes: Vec::new() }
                }

                pub fn write_bytes(&mut self, bytes: &[u8]) {
                    self.bytes.extend_from_slice(bytes);
                }

                pub fn as_bytes(&self) -> &[u8] {
                    &self.bytes
                }

                pub fn into_bytes(self) -> Vec<u8> {
                    self.bytes
                }
            }

            pub struct BinaryReadArchive {
                bytes: Vec<u8>,
                position: usize,
            }

            impl BinaryReadArchive {
                pub fn new(bytes: &[u8]) -> BinaryReadArchive {
                    BinaryReadArchive {
                        bytes: bytes.to_vec(),
                        position: 0_usize,
                    }
                }

                pub fn read_exact(&mut self, output: &mut [u8]) {
                    let end = self.position + output.len();
                    assert!(end <= self.bytes.len(), "archive underflow");
                    output.copy_from_slice(&self.bytes[self.position..end]);
                    self.position = end;
                }

                pub fn remaining(&self) -> usize {
                    self.bytes.len() - self.position
                }
            }

            #[allow(non_camel_case_types)]
            pub struct Serialize_;

            impl Serialize_ {
                pub unsafe fn serialize(value: v32, archive: &mut BinaryWriteArchive) {
                    let mut buffer = [0_u8; 9];
                    let size = SparseInt::dump32(value.get(), buffer.as_mut_ptr());
                    archive.write_bytes(&buffer[..size]);
                }
            }

            #[allow(non_camel_case_types)]
            pub struct Deserialize_;

            impl Deserialize_ {
                pub unsafe fn deserialize(value: &mut v32, archive: &mut BinaryReadArchive) {
                    let mut buffer = [0_u8; 9];
                    archive.read_exact(&mut buffer[..1]);
                    let size = SparseInt::buf_size(buffer[0]);
                    if size > 1_usize {
                        archive.read_exact(&mut buffer[1..size]);
                    }
                    value.set(SparseInt::load32(buffer.as_ptr()));
                }
            }

            type Factory = fn() -> SerializableProxy;

            fn registry() -> &'static Mutex<HashMap<i32, Factory>> {
                static REGISTRY: OnceLock<Mutex<HashMap<i32, Factory>>> = OnceLock::new();
                REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
            }

            fn make_proxy<T>() -> SerializableProxy
            where
                T: SerializablePayload + Default + 'static,
            {
                Arc::new(details::SerializableSharedPtrHolder {
                    ptr: Arc::new(T::default()),
                })
            }

            pub struct SerializableRegistry;

            impl SerializableRegistry {
                pub fn register<T>(kind: i32)
                where
                    T: SerializablePayload + Default + 'static,
                {
                    registry().lock().unwrap().insert(kind, make_proxy::<T>);
                }

                pub unsafe fn create(kind: i32) -> SerializableProxy {
                    let factory = *registry()
                        .lock()
                        .unwrap()
                        .get(&kind)
                        .unwrap_or_else(|| panic!("unregistered serializable kind {kind}"));
                    factory()
                }

                pub fn clear_for_testing() {
                    registry().lock().unwrap().clear();
                }
            }

            pub struct EmptySerializableBase;

            impl SerializableBaseApi for EmptySerializableBase {
                fn kind(&self) -> i32 {
                    0_i32
                }

                fn save(&self, _archive: &mut BinaryWriteArchive) {
                    panic!("empty serializable sentinel cannot be saved");
                }

                fn load(&mut self, _archive: &mut BinaryReadArchive) {
                    panic!("empty serializable sentinel cannot be loaded");
                }

                fn as_any(&self) -> &dyn Any {
                    self
                }
            }
        }
    }

    pub mod rusty {
        use std::marker::PhantomData;
        use std::sync::Arc as NativeArc;

        pub struct Arc<T: ?Sized>(PhantomData<*const T>);

        impl<T: ?Sized> Arc<T> {
            pub unsafe fn get(value: &NativeArc<T>) -> *const T {
                NativeArc::as_ptr(value)
            }
        }

        pub mod ptr {
            use crate::rpc::serializable_envelope::cpp::rrr::serializable::{
                EmptySerializableBase, SerializableBase, SerializableBaseApi,
            };

            static EMPTY: EmptySerializableBase = EmptySerializableBase;

            pub unsafe fn null() -> *const SerializableBase {
                let empty: &SerializableBase = &EMPTY as &dyn SerializableBaseApi;
                empty as *const SerializableBase
            }
        }
    }
}
