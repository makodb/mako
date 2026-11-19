//! Open-set, name-tagged serializable payload envelope.
//!
//! This valid-Rust owner preserves the public surface of the legacy
//! `srpc.any_message` C++ module: `AnyMessage` remains a two-field aggregate,
//! factories remain move-only callables, public type identity remains
//! `TypeId`/`std::type_index`, and the wire format remains
//! `[v64 string length][type name][payload bytes]`.

#![allow(
    unsafe_code,
    unused_unsafe,
    clippy::missing_safety_doc,
    clippy::slow_vector_initialization,
    clippy::unnecessary_get_then_check,
    clippy::unnecessary_unwrap
)]

use crate::debugging::verify_at;
use crate::serializable::{SerializablePayload, SerializableProxy};
use std::sync::Arc;
use std::any::TypeId;


#[cfg_attr(not(any()), derive(Clone, Default))]
pub struct AnyMessage {
    pub type_name_: String,
    pub payload_: Option<SerializableProxy>,
}

impl AnyMessage {
    pub fn save(&self, archive: &mut crate::serializable::BinaryWriteArchive) {
        crate::serializable::Serialize_::serialize(&self.type_name_, archive);
        if self.payload_.is_some() {
            let payload = self.payload_.as_ref().unwrap();
            let base = Arc::as_ptr(payload);
            #[allow(unsafe_code)]
            unsafe {
                (*base).save(archive);
            }
        }
    }

    pub fn load(&mut self, archive: &mut crate::serializable::BinaryReadArchive) {
        crate::serializable::Deserialize_::deserialize(&mut self.type_name_, archive);
        if archive.failed() {
            self.payload_ = None;
            return;
        }
        let proxy_option = any_message_registry::create(&self.type_name_);
        verify_at(proxy_option.is_some(), file!(), line!());
        let mut proxy = proxy_option.unwrap();
        Arc::get_mut(&mut proxy).unwrap().load(archive);
        self.payload_ = Some(proxy);
    }

    pub fn is_a<T: 'static>(&self) -> bool {
        anymessage_is_a::<T>(self)
    }

    pub fn unpack<T: 'static>(&self) -> Option<Arc<T>> {
        anymessage_unpack::<T>(self)
    }

    pub fn pack_as<T: SerializablePayload + 'static>(name: String, value: Arc<T>) -> AnyMessage {
        anymessage_pack_as::<T>(name, value)
    }

    pub fn pack<T: SerializablePayload + 'static>(value: Arc<T>) -> AnyMessage {
        anymessage_pack::<T>(value)
    }
}

pub mod any_message_registry {
    use crate::debugging::verify_at;
    use crate::serializable::SerializableProxy;
    use std::any::TypeId;
    use std::collections::HashMap;
    use std::sync::Mutex;

    pub type Factory = Box<dyn FnMut() -> SerializableProxy + Send + Sync>;

    struct RegistryMap {
        by_name: HashMap<String, Factory>,
        name_by_type: HashMap<TypeId, String>,
    }

    static REGISTRY: Mutex<Option<RegistryMap>> = Mutex::new(None);

    pub fn register_type(name: String, type_id: TypeId, factory: self::Factory) -> i32 {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        let map = guard.as_mut().unwrap();
        verify_at(map.by_name.get(&name).is_none(), file!(), line!());
        if map.name_by_type.get(&type_id).is_none() {
            map.name_by_type.insert(type_id, name.clone());
        }
        map.by_name.insert(name, factory);
        0_i32
    }

    pub fn create(name: &str) -> Option<SerializableProxy> {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        let map = guard.as_mut().unwrap();
        let mut factory = map.by_name.remove(name)?;
        let payload = factory();
        map.by_name.insert(name.to_string(), factory);
        Some(payload)
    }

    pub fn name_for_type_owned(type_id: TypeId) -> String {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        let map = guard.as_mut().unwrap();
        match map.name_by_type.get(&type_id) {
            Some(name) => name.clone(),
            None => Default::default(),
        }
    }

    pub fn is_registered_name(name: &str) -> bool {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        guard.as_mut().unwrap().by_name.get(name).is_some()
    }

    pub fn is_registered_type(type_id: TypeId) -> bool {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        guard.as_mut().unwrap().name_by_type.get(&type_id).is_some()
    }

    pub fn clear_for_testing() {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        let map = guard.as_mut().unwrap();
        map.by_name.clear();
        map.name_by_type.clear();
    }
}

pub fn reg_any_message_as<T>(name: String) -> i32
where
    T: SerializablePayload + Default + 'static,
{
    let factory: any_message_registry::Factory = Box::new(|| -> SerializableProxy {
        let pointer = Arc::new(T::default());
        crate::serializable::make_serializable_proxy(pointer)
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

pub fn anymessage_unpack<T: 'static>(message: &AnyMessage) -> Option<Arc<T>> {
    if !anymessage_is_a::<T>(message) || message.payload_.is_none() {
        return None;
    }
    let base = Arc::as_ptr(message.payload_.as_ref().unwrap());
    let holder = unsafe { crate::serializable::serializable_holder_of::<T>(base) };
    if holder.is_null() {
        return None;
    }
    Some(unsafe { (*holder).ptr.clone() })
}

pub fn anymessage_pack_as<T: SerializablePayload + 'static>(name: String, value: Arc<T>) -> AnyMessage {
    let payload: SerializableProxy =
        crate::serializable::make_serializable_proxy(value);
    AnyMessage {
        type_name_: name,
        payload_: Some(payload),
    }
}

pub fn anymessage_pack<T: SerializablePayload + 'static>(value: Arc<T>) -> AnyMessage {
    let name = any_message_registry::name_for_type_owned(TypeId::of::<T>());
    verify_at(!name.is_empty(), file!(), line!());
    anymessage_pack_as::<T>(name, value)
}

pub fn serialize(message: &AnyMessage, archive: &mut crate::serializable::BinaryWriteArchive) {
    message.save(archive);
}

pub fn deserialize(message: &mut AnyMessage, archive: &mut crate::serializable::BinaryReadArchive) {
    message.load(archive);
}
