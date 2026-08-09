//! Open-set, name-tagged serializable payload envelope.
//!
//! This valid-Rust owner preserves the public surface of the legacy
//! `rrr.any_message` C++ module: `AnyMessage` remains a two-field aggregate,
//! factories remain move-only callables, public type identity remains
//! `TypeId`/`std::type_index`, and the wire format remains
//! `[v64 string length][type name][payload bytes]`.

#![allow(unsafe_code, unused_unsafe)]

use cpp::rrr::debugging;
use cpp::rrr::serializable;
use cpp::rusty as cpp_rusty;
use std::any::TypeId;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

type LegacyStdString = String;

pub type SerializableProxy = Arc<dyn serializable::SerializableBase>;

#[cfg_attr(not(any()), derive(Clone, Default))]
pub struct AnyMessage {
    pub type_name_: LegacyStdString,
    pub payload_: Option<SerializableProxy>,
}

impl AnyMessage {
    pub fn save(&self, archive: &mut serializable::BinaryWriteArchive) {
        unsafe { serializable::Serialize_::serialize(&self.type_name_, archive) };
        if self.payload_.is_some() {
            let payload = self.payload_.as_ref().unwrap();
            let base =
                unsafe { cpp_rusty::Arc::<dyn serializable::SerializableBase>::get(payload) };
            unsafe { (*base).save(archive) };
        }
    }

    pub fn load(&mut self, archive: &mut serializable::BinaryReadArchive) {
        unsafe { serializable::Deserialize_::deserialize(&mut self.type_name_, archive) };
        let proxy_option = any_message_registry::create(&self.type_name_);
        unsafe { debugging::verify(proxy_option.is_some()) };
        let mut proxy = proxy_option.unwrap();
        Arc::get_mut(&mut proxy).unwrap().load(archive);
        self.payload_ = Some(proxy);
    }

    pub fn is_a<T: 'static>(&self) -> bool {
        anymessage_is_a::<T>(self)
    }

    pub fn unpack<T>(&self) -> Option<Arc<T>>
    where
        T: serializable::SerializablePayload + 'static,
    {
        anymessage_unpack::<T>(self)
    }

    pub fn pack_as<T>(name: LegacyStdString, value: Arc<T>) -> AnyMessage
    where
        T: serializable::SerializablePayload + 'static,
    {
        anymessage_pack_as::<T>(name, value)
    }

    pub fn pack<T>(value: Arc<T>) -> AnyMessage
    where
        T: serializable::SerializablePayload + 'static,
    {
        anymessage_pack::<T>(value)
    }
}

pub mod any_message_registry {
    use super::*;

    pub type Factory = Box<dyn FnMut() -> SerializableProxy + Send + Sync>;

    struct RegistryMap {
        by_name: HashMap<LegacyStdString, Factory>,
        name_by_type_hash: HashMap<usize, LegacyStdString>,
    }

    fn registry() -> &'static Mutex<Option<RegistryMap>> {
        static REGISTRY: Mutex<Option<RegistryMap>> = Mutex::new(None);
        &REGISTRY
    }

    fn map_mut<'a>(
        guard: &'a mut std::sync::MutexGuard<'_, Option<RegistryMap>>,
    ) -> &'a mut RegistryMap {
        if guard.is_none() {
            **guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type_hash: HashMap::new(),
            });
        }
        guard.as_mut().unwrap()
    }

    pub fn register_type(name: LegacyStdString, type_id: TypeId, factory: self::Factory) -> i32 {
        let mut guard = registry().lock().unwrap();
        let hash = unsafe { cpp_rusty::type_id_hash_code(type_id) };
        let map = map_mut(&mut guard);
        unsafe { debugging::verify(map.by_name.get(&name).is_none()) };
        if map.name_by_type_hash.get(&hash).is_none() {
            map.name_by_type_hash.insert(hash, name.clone());
        }
        map.by_name.insert(name, factory);
        0_i32
    }

    pub fn create(name: &LegacyStdString) -> Option<SerializableProxy> {
        let mut guard = registry().lock().unwrap();
        let map = map_mut(&mut guard);
        match map.by_name.get_mut(name) {
            Some(factory) => Some(factory()),
            None => None,
        }
    }

    pub fn name_for_type_owned(type_id: TypeId) -> LegacyStdString {
        let mut guard = registry().lock().unwrap();
        let hash = unsafe { cpp_rusty::type_id_hash_code(type_id) };
        let map = map_mut(&mut guard);
        match map.name_by_type_hash.get(&hash) {
            Some(name) => name.clone(),
            None => Default::default(),
        }
    }

    pub fn is_registered_name(name: &LegacyStdString) -> bool {
        let mut guard = registry().lock().unwrap();
        map_mut(&mut guard).by_name.get(name).is_some()
    }

    pub fn is_registered_type(type_id: TypeId) -> bool {
        let mut guard = registry().lock().unwrap();
        let hash = unsafe { cpp_rusty::type_id_hash_code(type_id) };
        map_mut(&mut guard).name_by_type_hash.get(&hash).is_some()
    }

    pub fn clear_for_testing() {
        let mut guard = registry().lock().unwrap();
        let map = map_mut(&mut guard);
        map.by_name.clear();
        map.name_by_type_hash.clear();
    }
}

pub fn reg_any_message_as<T>(name: LegacyStdString) -> i32
where
    T: serializable::SerializablePayload + Default + 'static,
{
    let factory: any_message_registry::Factory = Box::new(|| -> SerializableProxy {
        let pointer = unsafe { cpp_rusty::arc_make_default::<T>() };
        let holder = unsafe { serializable::details::SerializableSharedPtrHolder::<T>(pointer) };
        let concrete: Arc<serializable::details::SerializableSharedPtrHolder<T>> = Arc::new(holder);
        concrete
    });
    any_message_registry::register_type(name, TypeId::of::<T>(), factory)
}

pub fn anymessage_is_a<T: 'static>(message: &AnyMessage) -> bool {
    let name = any_message_registry::name_for_type_owned(TypeId::of::<T>());
    if name.is_empty() {
        return false;
    }
    message.type_name_ == name
}

pub fn anymessage_unpack<T>(message: &AnyMessage) -> Option<Arc<T>>
where
    T: serializable::SerializablePayload + 'static,
{
    if !anymessage_is_a::<T>(message) || message.payload_.is_none() {
        return None;
    }
    let payload = message.payload_.as_ref().unwrap();
    let base = unsafe { cpp_rusty::Arc::<dyn serializable::SerializableBase>::get(payload) };
    let holder = unsafe { serializable::serializable_holder_of::<T>(base) };
    if holder.is_null() {
        return None;
    }
    Some(unsafe { (*holder).ptr.clone() })
}

pub fn anymessage_pack_as<T>(name: LegacyStdString, value: Arc<T>) -> AnyMessage
where
    T: serializable::SerializablePayload + 'static,
{
    let holder = unsafe { serializable::details::SerializableSharedPtrHolder::<T>(value) };
    let concrete: Arc<serializable::details::SerializableSharedPtrHolder<T>> = Arc::new(holder);
    let payload: SerializableProxy = concrete;
    AnyMessage {
        type_name_: name,
        payload_: Some(payload),
    }
}

pub fn anymessage_pack<T>(value: Arc<T>) -> AnyMessage
where
    T: serializable::SerializablePayload + 'static,
{
    let name = any_message_registry::name_for_type_owned(TypeId::of::<T>());
    unsafe { debugging::verify(!name.is_empty()) };
    anymessage_pack_as::<T>(name, value)
}

pub fn serialize(message: &AnyMessage, archive: &mut serializable::BinaryWriteArchive) {
    message.save(archive);
}

pub fn deserialize(message: &mut AnyMessage, archive: &mut serializable::BinaryReadArchive) {
    message.load(archive);
}

// Cargo-only implementations of the reserved `cpp::` imports. The C++
// consumer suppresses this module and binds the names through the module-local
// fail-closed symbol index.
#[allow(dead_code)]
pub mod cpp {
    pub mod rrr {
        pub mod debugging {
            pub unsafe fn verify(value: bool) {
                assert!(value);
            }
        }

        pub mod serializable {
            use std::any::Any;
            use std::sync::Arc;

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

            pub trait SerializablePayload: Any + Send + Sync {
                fn save(&self, archive: &mut BinaryWriteArchive);
                fn load(&mut self, archive: &mut BinaryReadArchive);
                fn kind(&self) -> i32;
            }

            pub trait SerializableBase: Any + Send + Sync {
                fn save(&self, archive: &mut BinaryWriteArchive);
                fn load(&mut self, archive: &mut BinaryReadArchive);
                fn kind(&self) -> i32;
                fn as_any(&self) -> &dyn Any;
            }

            pub mod details {
                use super::*;

                pub struct SerializableSharedPtrHolder<T> {
                    pub ptr: Arc<T>,
                }

                #[allow(non_snake_case)]
                pub unsafe fn SerializableSharedPtrHolder<T>(
                    ptr: Arc<T>,
                ) -> SerializableSharedPtrHolder<T> {
                    SerializableSharedPtrHolder { ptr }
                }

                impl<T: SerializablePayload> SerializableBase for SerializableSharedPtrHolder<T> {
                    fn save(&self, archive: &mut BinaryWriteArchive) {
                        self.ptr.save(archive);
                    }

                    fn load(&mut self, archive: &mut BinaryReadArchive) {
                        Arc::get_mut(&mut self.ptr).unwrap().load(archive);
                    }

                    fn kind(&self) -> i32 {
                        self.ptr.kind()
                    }

                    fn as_any(&self) -> &dyn Any {
                        self
                    }
                }
            }

            pub unsafe fn serializable_holder_of<T: 'static>(
                base: *const dyn SerializableBase,
            ) -> *const details::SerializableSharedPtrHolder<T> {
                let base_ref = &*base;
                match base_ref
                    .as_any()
                    .downcast_ref::<details::SerializableSharedPtrHolder<T>>()
                {
                    Some(holder) => holder,
                    None => core::ptr::null(),
                }
            }

            fn sparse_size(byte0: u8) -> usize {
                if (byte0 & 0x80_u8) == 0_u8 {
                    1_usize
                } else if (byte0 & 0xC0_u8) == 0x80_u8 {
                    2_usize
                } else if (byte0 & 0xE0_u8) == 0xC0_u8 {
                    3_usize
                } else if (byte0 & 0xF0_u8) == 0xE0_u8 {
                    4_usize
                } else if (byte0 & 0xF8_u8) == 0xF0_u8 {
                    5_usize
                } else if (byte0 & 0xFC_u8) == 0xF8_u8 {
                    6_usize
                } else if (byte0 & 0xFE_u8) == 0xFC_u8 {
                    7_usize
                } else if byte0 == 0xFE_u8 {
                    8_usize
                } else {
                    9_usize
                }
            }

            fn write_length(length: usize, archive: &mut BinaryWriteArchive) {
                let value = i64::try_from(length).unwrap();
                let bits = value as u64;
                let size = if value <= 63_i64 {
                    1_usize
                } else if value <= 8_191_i64 {
                    2_usize
                } else if value <= 1_048_575_i64 {
                    3_usize
                } else if value <= 134_217_727_i64 {
                    4_usize
                } else if value <= 17_179_869_183_i64 {
                    5_usize
                } else if value <= 2_199_023_255_551_i64 {
                    6_usize
                } else if value <= 281_474_976_710_655_i64 {
                    7_usize
                } else if value <= 36_028_797_018_963_967_i64 {
                    8_usize
                } else {
                    9_usize
                };
                let mut encoded = [0_u8; 9];
                if size <= 7_usize {
                    for (index, byte) in encoded[..size].iter_mut().enumerate() {
                        *byte = (bits >> (8_usize * (size - 1_usize - index))) as u8;
                    }
                    encoded[0] &= 0xFF_u8 >> size;
                    if size > 1_usize {
                        encoded[0] |= 0xFF_u8 << (9_usize - size);
                    }
                } else {
                    for index in 0_usize..8_usize {
                        encoded[1_usize + index] = (bits >> (8_usize * (7_usize - index))) as u8;
                    }
                    encoded[0] = if size == 8_usize { 0xFE_u8 } else { 0xFF_u8 };
                }
                archive.write_bytes(&encoded[..size]);
            }

            fn read_length(archive: &mut BinaryReadArchive) -> usize {
                let mut encoded = [0_u8; 9];
                archive.read_exact(&mut encoded[..1]);
                let size = sparse_size(encoded[0]);
                if size > 1_usize {
                    archive.read_exact(&mut encoded[1..size]);
                }
                let value = if size < 8_usize {
                    let mut bits = 0_u64;
                    for index in 0_usize..size - 1_usize {
                        bits |= (encoded[size - 1_usize - index] as u64) << (8_usize * index);
                    }
                    let top = encoded[0] & (0xFF_u8 >> size);
                    bits | ((top as u64) << (8_usize * (size - 1_usize)))
                } else {
                    let mut bits = 0_u64;
                    for index in 0_usize..8_usize {
                        bits |= (encoded[8_usize - index] as u64) << (8_usize * index);
                    }
                    bits
                };
                usize::try_from(value).unwrap()
            }

            #[allow(non_camel_case_types)]
            pub struct Serialize_;

            impl Serialize_ {
                pub unsafe fn serialize(value: &String, archive: &mut BinaryWriteArchive) {
                    write_length(value.len(), archive);
                    archive.write_bytes(value.as_bytes());
                }
            }

            #[allow(non_camel_case_types)]
            pub struct Deserialize_;

            impl Deserialize_ {
                pub unsafe fn deserialize(value: &mut String, archive: &mut BinaryReadArchive) {
                    let length = read_length(archive);
                    let mut bytes = vec![0_u8; length];
                    archive.read_exact(&mut bytes);
                    *value = String::from_utf8(bytes).unwrap();
                }
            }
        }
    }

    pub mod rusty {
        use std::any::TypeId;
        use std::collections::hash_map::DefaultHasher;
        use std::hash::{Hash, Hasher};
        use std::marker::PhantomData;
        use std::sync::Arc as NativeArc;

        pub struct Arc<T: ?Sized>(PhantomData<*const T>);

        impl<T: ?Sized> Arc<T> {
            pub unsafe fn get(value: &NativeArc<T>) -> *const T {
                NativeArc::as_ptr(value)
            }
        }

        pub unsafe fn arc_make_default<T: Default>() -> NativeArc<T> {
            NativeArc::new(T::default())
        }

        pub fn type_id_hash_code(type_id: TypeId) -> usize {
            let mut hasher = DefaultHasher::new();
            type_id.hash(&mut hasher);
            hasher.finish() as usize
        }
    }
}
