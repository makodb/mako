//! Canonical Rust owner for `rrr.serializable_envelope`.

use cpp::rrr::basetypes as cpp_basetypes;
use cpp::rrr::debugging as cpp_debugging;
use cpp::rrr::serializable as cpp_serializable;
use rusty as cpp;

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
    h: *const rusty::SerializableSharedPtrHolder<T>,
) -> *mut T {
    let p: *const T = unsafe { (*h).ptr.get() };
    p as *mut T
}

#[cfg_attr(any(), cpp_marker_trait)]
pub trait PayloadMember<Set> {
    const KIND: i32;
}

pub struct SerializableEnvelope<PayloadSet> {
    pub kind_: i32,
    inner_: Option<rusty::SerializableProxy>,
    _payload_set: [core::marker::PhantomData<PayloadSet>; 0],
}

impl<PayloadSet> SerializableEnvelope<PayloadSet> {
    fn base_ptr(&self) -> *const rusty::SerializableBase {
        if self.inner_.is_none() {
            return core::ptr::null();
        }
        self.inner_.as_ref().unwrap().get()
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

    pub fn pack<T: PayloadMember<PayloadSet> + Clone + 'static>(
        value: &T,
    ) -> SerializableEnvelope<PayloadSet> {
        SerializableEnvelope::<PayloadSet>::pack_aliased::<T>(rusty::Arc::<T>::make(value.clone()))
    }

    #[allow(clippy::field_reassign_with_default)]
    pub fn pack_aliased<T: PayloadMember<PayloadSet> + 'static>(
        sp: rusty::Arc<T>,
    ) -> SerializableEnvelope<PayloadSet> {
        let mut env: SerializableEnvelope<PayloadSet> = Default::default();
        env.inner_ = Some(rusty::Arc::<rusty::SerializableSharedPtrHolder<T>>::make(
            sp,
        ));
        env.refresh_kind();
        env
    }

    #[allow(unsafe_code)]
    pub fn unpack<T: PayloadMember<PayloadSet> + 'static>(&self) -> *const T {
        let h = unsafe { cpp_serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if h.is_null() {
            return core::ptr::null();
        }
        unsafe { (*h).ptr.get() }
    }

    #[allow(unsafe_code)]
    pub fn unpack_shared<T: PayloadMember<PayloadSet> + 'static>(&self) -> Option<rusty::Arc<T>> {
        let h = unsafe { cpp_serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if h.is_null() {
            return None;
        }
        Some(unsafe { (*h).ptr.clone() })
    }

    pub fn is_a<T: PayloadMember<PayloadSet> + 'static>(&self) -> bool {
        let p: *const T = self.unpack::<T>();
        !p.is_null()
    }

    #[allow(unsafe_code)]
    pub fn save(&self, ar: &mut rusty::BinaryWriteArchive) {
        unsafe { cpp_debugging::verify(self.has_value()) };
        let b = self.base_ptr();
        unsafe {
            let mut kind_bytes: [u8; 9] = [0u8; 9];
            let byte_count = cpp_basetypes::SparseInt::dump32((*b).kind(), kind_bytes.as_mut_ptr());
            ar.write_bytes(kind_bytes.as_ptr(), byte_count);
            (*b).save(ar);
        }
    }

    #[allow(unsafe_code)]
    pub fn load(&mut self, ar: &mut rusty::BinaryReadArchive) {
        let mut kind_bytes: [u8; 9] = [0u8; 9];
        unsafe { cpp_debugging::verify(ar.read_exact(kind_bytes.as_mut_ptr(), 1)) };
        let byte_count = unsafe { cpp_basetypes::SparseInt::buf_size(kind_bytes[0]) };
        if byte_count > 1 {
            unsafe {
                cpp_debugging::verify(ar.read_exact(kind_bytes.as_mut_ptr().add(1), byte_count - 1))
            };
        }
        let kind: i32 = unsafe { cpp_basetypes::SparseInt::load32(kind_bytes.as_ptr()) };
        let mut proxy: rusty::SerializableProxy =
            unsafe { cpp_serializable::SerializableRegistry::create(kind) };
        proxy.get_mut().unwrap().load(ar);
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
    pub unsafe fn unpack_mut<T: PayloadMember<PayloadSet> + 'static>(&mut self) -> *mut T {
        let h = unsafe { cpp_serializable::serializable_holder_of::<T>(self.base_ptr()) };
        if h.is_null() {
            return core::ptr::null_mut();
        }
        let p: *const T = unsafe { (*h).ptr.get() };
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
        self.base_ptr() == other.base_ptr()
    }
}

pub fn marshallable_cast<T: PayloadMember<PayloadSet> + 'static, PayloadSet>(
    env: &SerializableEnvelope<PayloadSet>,
) -> Option<rusty::Arc<T>> {
    env.unpack_shared::<T>()
}

pub fn serialize<PayloadSet>(
    env: &SerializableEnvelope<PayloadSet>,
    ar: &mut rusty::BinaryWriteArchive,
) {
    env.save(ar);
}

pub fn deserialize<PayloadSet>(
    env: &mut SerializableEnvelope<PayloadSet>,
    ar: &mut rusty::BinaryReadArchive,
) {
    env.load(ar);
}
