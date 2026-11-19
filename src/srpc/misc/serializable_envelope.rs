//! Canonical Rust owner for `srpc.serializable_envelope`.

use crate::basetypes::SparseInt;
use crate::debugging::verify_at;
use crate::serializable::{BinaryReadArchive, BinaryWriteArchive, SerializableBase, SerializablePayload, SerializableProxy};
#[allow(unused_imports)]
use crate::serializable as _;
type SerializableSharedPtrHolder<T> = crate::serializable::details::SerializableSharedPtrHolder<T>;
use std::sync::Arc;

#[allow(unsafe_code)]
/// Recover a mutable payload pointer from a checked shared-holder pointer.
///
/// # Safety
///
/// `h` must be non-null and point to a live `SerializableSharedPtrHolder<T>`.
/// The caller must additionally own an exclusive mutation window for the
/// holder's inner `Arc<T>`: no cloned envelope, shared payload handle, raw
/// pointer, or reference may observe or access `T` until the returned pointer
/// is no longer used.
pub unsafe fn envelope_holder_ptr_mut<T>(
    h: *const SerializableSharedPtrHolder<T>,
) -> *mut T {
    let p: *const T = unsafe { Arc::as_ptr(&(*h).ptr) };
    p as *mut T
}

#[cfg_attr(any(), cpp_marker_trait)]
pub trait PayloadMember<Set> {
    const KIND: i32;
}

pub struct SerializableEnvelope<PayloadSet> {
    pub kind_: i32,
    inner_: Option<SerializableProxy>,
    _payload_set: [core::marker::PhantomData<PayloadSet>; 0],
}

impl<PayloadSet> SerializableEnvelope<PayloadSet> {
    fn base_ptr(&self) -> *const dyn SerializableBase {
        Arc::as_ptr(self.inner_.as_ref().unwrap())
    }

    #[allow(unsafe_code)]
    fn refresh_kind(&mut self) {
        if self.inner_.is_none() {
            self.kind_ = 0i32;
            return;
        }
        let b = self.base_ptr();
        self.kind_ = unsafe { (*b).kind() };
    }

    pub fn has_value(&self) -> bool {
        self.inner_.is_some()
    }

    #[allow(unsafe_code)]
    pub fn kind(&self) -> i32 {
        if self.inner_.is_none() {
            return 0i32;
        }
        let b = self.base_ptr();
        unsafe { (*b).kind() }
    }

    pub fn pack<T: PayloadMember<PayloadSet> + SerializablePayload + Clone + 'static>(
        value: &T,
    ) -> SerializableEnvelope<PayloadSet> {
        SerializableEnvelope::<PayloadSet>::pack_aliased::<T>(Arc::new(value.clone()))
    }

    #[allow(clippy::field_reassign_with_default)]
    pub fn pack_aliased<T: PayloadMember<PayloadSet> + SerializablePayload + 'static>(
        sp: Arc<T>,
    ) -> SerializableEnvelope<PayloadSet> {
        let mut env: SerializableEnvelope<PayloadSet> = Default::default();
        env.inner_ = Some(crate::serializable::make_serializable_proxy(sp));
        env.refresh_kind();
        env
    }

    #[allow(unsafe_code)]
    pub fn unpack<T: PayloadMember<PayloadSet> + SerializablePayload + 'static>(&self) -> *const T {
        if self.inner_.is_none() {
            return core::ptr::null();
        }
        let h = unsafe { crate::serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if h.is_null() {
            return core::ptr::null();
        }
        unsafe { Arc::as_ptr(&(*h).ptr) }
    }

    #[allow(unsafe_code)]
    pub fn unpack_shared<T: PayloadMember<PayloadSet> + SerializablePayload + 'static>(&self) -> Option<Arc<T>> {
        let base = Arc::as_ptr(self.inner_.as_ref()?);
        let h = unsafe { crate::serializable::serializable_holder_of::<T>(base) };
        if h.is_null() {
            return None;
        }
        Some(unsafe { (*h).ptr.clone() })
    }

    pub fn is_a<T: PayloadMember<PayloadSet> + SerializablePayload + 'static>(&self) -> bool {
        let p: *const T = self.unpack::<T>();
        !p.is_null()
    }

    #[allow(unsafe_code)]
    pub fn save(&self, ar: &mut BinaryWriteArchive) {
        verify_at(self.has_value(), file!(), line!());
        let b = self.base_ptr();
        unsafe {
            let mut kind_bytes: [u8; 9] = [0u8; 9];
            let byte_count = SparseInt::dump32((*b).kind(), kind_bytes.as_mut_ptr());
            ar.write_bytes(kind_bytes.as_ptr(), byte_count);
            (*b).save(ar);
        }
    }

    #[allow(unsafe_code)]
    pub fn load(&mut self, ar: &mut BinaryReadArchive) {
        let mut kind_bytes: [u8; 9] = [0u8; 9];
        unsafe { verify_at(ar.read_exact(kind_bytes.as_mut_ptr(), 1), file!(), line!()) };
        let byte_count = SparseInt::buf_size(kind_bytes[0]);
        if byte_count > 1 {
            unsafe {
                verify_at(ar.read_exact(kind_bytes.as_mut_ptr().add(1), byte_count - 1), file!(), line!());
            };
        }
        let kind: i32 = unsafe { SparseInt::load32(kind_bytes.as_ptr()) };
        let mut proxy: SerializableProxy =
            crate::serializable::SerializableRegistry::create(kind);
        Arc::get_mut(&mut proxy).unwrap().load(ar);
        self.inner_ = Some(proxy);
        self.refresh_kind();
    }

    /// Return the historical mutable payload pointer after a checked downcast.
    ///
    /// # Safety
    ///
    /// The payload must be exclusively owned for the entire use of the
    /// returned pointer. In particular, no envelope clone, `unpack_shared`
    /// result, earlier raw pointer, or Rust/C++ reference may alias the inner
    /// `Arc<T>`. `&mut self` alone does not prove this because cloning an
    /// envelope shares its holder and payload.
    #[allow(unsafe_code)]
    pub unsafe fn unpack_mut<T: PayloadMember<PayloadSet> + SerializablePayload + 'static>(&mut self) -> *mut T {
        if self.inner_.is_none() {
            return core::ptr::null_mut();
        }
        let h = unsafe { crate::serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if h.is_null() {
            return core::ptr::null_mut();
        }
        let p: *const T = unsafe { Arc::as_ptr(&(*h).ptr) };
        p as *mut T
    }
}

impl<PayloadSet> Default for SerializableEnvelope<PayloadSet> {
    fn default() -> SerializableEnvelope<PayloadSet> {
        SerializableEnvelope {
            kind_: 0i32,
            inner_: None,
            _payload_set: [],
        }
    }
}

impl<PayloadSet> Clone for SerializableEnvelope<PayloadSet> {
    #[allow(clippy::field_reassign_with_default)]
    fn clone(&self) -> SerializableEnvelope<PayloadSet> {
        let mut env: SerializableEnvelope<PayloadSet> = Default::default();
        env.kind_ = self.kind_;
        env.inner_ = self.inner_.clone();
        env
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
        self.base_ptr() as *const () == other.base_ptr() as *const ()
    }
}

pub fn marshallable_cast<T: PayloadMember<PayloadSet> + SerializablePayload + 'static, PayloadSet>(
    env: &SerializableEnvelope<PayloadSet>,
) -> Option<Arc<T>> {
    env.unpack_shared::<T>()
}

pub fn serialize<PayloadSet>(
    env: &SerializableEnvelope<PayloadSet>,
    ar: &mut BinaryWriteArchive,
) {
    env.save(ar);
}

pub fn deserialize<PayloadSet>(
    env: &mut SerializableEnvelope<PayloadSet>,
    ar: &mut BinaryReadArchive,
) {
    env.load(ar);
}
