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

use cpp::srpc::debugging;
use cpp::srpc::serializable;
use rusty as cpp;
use std::any::TypeId;

type LegacyStdString = String;

#[cfg_attr(not(any()), derive(Clone, Default))]
pub struct AnyMessage {
    pub type_name_: LegacyStdString,
    pub payload_: Option<rusty::SerializableProxy>,
}

impl AnyMessage {
    pub fn save(&self, archive: &mut serializable::BinaryWriteArchive) {
        unsafe { serializable::Serialize_::serialize(&self.type_name_, archive) };
        if self.payload_.is_some() {
            let payload = self.payload_.as_ref().unwrap();
            let base = payload.get();
            #[allow(unsafe_code)]
            unsafe {
                (*base).save(archive);
            }
        }
    }

    pub fn load(&mut self, archive: &mut serializable::BinaryReadArchive) {
        unsafe { serializable::Deserialize_::deserialize(&mut self.type_name_, archive) };
        let proxy_option = any_message_registry::create(&self.type_name_);
        unsafe { debugging::verify(proxy_option.is_some()) };
        let mut proxy = proxy_option.unwrap();
        proxy.get_mut().unwrap().load(archive);
        self.payload_ = Some(proxy);
    }

    pub fn is_a<T: 'static>(&self) -> bool {
        anymessage_is_a::<T>(self)
    }

    pub fn unpack<T: 'static>(&self) -> Option<rusty::Arc<T>> {
        anymessage_unpack::<T>(self)
    }

    pub fn pack_as<T: 'static>(name: LegacyStdString, value: rusty::Arc<T>) -> AnyMessage {
        anymessage_pack_as::<T>(name, value)
    }

    pub fn pack<T: 'static>(value: rusty::Arc<T>) -> AnyMessage {
        anymessage_pack::<T>(value)
    }
}

pub mod any_message_registry {
    use super::{debugging, LegacyStdString};
    use std::any::TypeId;
    use std::collections::HashMap;
    use std::sync::Mutex;

    pub type Factory = Box<dyn FnMut() -> rusty::SerializableProxy + Send + Sync>;

    struct RegistryMap {
        by_name: HashMap<LegacyStdString, Factory>,
        name_by_type: HashMap<TypeId, LegacyStdString>,
    }

    static REGISTRY: Mutex<Option<RegistryMap>> = Mutex::new(None);

    pub fn register_type(name: LegacyStdString, type_id: TypeId, factory: self::Factory) -> i32 {
        let mut guard = REGISTRY.lock().unwrap();
        if guard.is_none() {
            *guard = Some(RegistryMap {
                by_name: HashMap::new(),
                name_by_type: HashMap::new(),
            });
        }
        let map = guard.as_mut().unwrap();
        unsafe { debugging::verify(map.by_name.get(&name).is_none()) };
        if map.name_by_type.get(&type_id).is_none() {
            map.name_by_type.insert(type_id, name.clone());
        }
        map.by_name.insert(name, factory);
        0_i32
    }

    pub fn create(name: &LegacyStdString) -> Option<rusty::SerializableProxy> {
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
        map.by_name.insert(name.clone(), factory);
        Some(payload)
    }

    pub fn name_for_type_owned(type_id: TypeId) -> LegacyStdString {
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

    pub fn is_registered_name(name: &LegacyStdString) -> bool {
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

pub fn reg_any_message_as<T>(name: LegacyStdString) -> i32
where
    T: Default + 'static,
{
    let factory: any_message_registry::Factory = Box::new(|| -> rusty::SerializableProxy {
        let pointer = rusty::Arc::<T>::make(T::default());
        rusty::Arc::<rusty::SerializableSharedPtrHolder<T>>::make(pointer)
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

pub fn anymessage_unpack<T: 'static>(message: &AnyMessage) -> Option<rusty::Arc<T>> {
    if !anymessage_is_a::<T>(message) || message.payload_.is_none() {
        return None;
    }
    let base = message.payload_.as_ref().unwrap().get();
    let holder = unsafe { serializable::serializable_holder_of::<T>(base) };
    if holder.is_null() {
        return None;
    }
    Some(unsafe { (*holder).ptr.clone() })
}

pub fn anymessage_pack_as<T: 'static>(name: LegacyStdString, value: rusty::Arc<T>) -> AnyMessage {
    let payload: rusty::SerializableProxy =
        rusty::Arc::<rusty::SerializableSharedPtrHolder<T>>::make(value);
    AnyMessage {
        type_name_: name,
        payload_: Some(payload),
    }
}

pub fn anymessage_pack<T: 'static>(value: rusty::Arc<T>) -> AnyMessage {
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
