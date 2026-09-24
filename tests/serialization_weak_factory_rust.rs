use srpc::serializable::{
    BinaryReadArchive, BinaryWriteArchive, BufferSource, SerializablePayload, SerializableRegistry,
    make_serializable_proxy, make_source_proxy_buffer,
    serializable_registry_register_factory,
};
use std::sync::{Arc, Mutex, Weak};

struct Payload {
    value: i32,
}
impl SerializablePayload for Payload {
    fn save(&self, _: &mut BinaryWriteArchive) {}
    fn load(&mut self, _: &mut BinaryReadArchive) {
        self.value = 99;
    }
    fn kind(&self) -> i32 {
        196601
    }
}

#[test]
#[allow(unsafe_code)]
fn factory_retaining_weak_payload_cannot_open_a_mutation_window() {
    let retained = Arc::new(Mutex::new(None::<Weak<Payload>>));
    let factory_retained = retained.clone();
    serializable_registry_register_factory(
        196601,
        Box::new(move || {
            let payload = Arc::new(Payload { value: 7 });
            *factory_retained.lock().unwrap() = Some(Arc::downgrade(&payload));
            make_serializable_proxy(payload)
        }),
    );
    let mut proxy = SerializableRegistry::create(196601);
    let mut source = BufferSource::new(std::ptr::null(), 0);
    // The stack cursor outlives the archive and advertises no input bytes.
    let mut archive = BinaryReadArchive::new(unsafe {
        make_source_proxy_buffer(&raw mut source)
    });
    let rejected = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        Arc::get_mut(&mut proxy).unwrap().load(&mut archive);
    }));
    assert!(rejected.is_err());
    assert_eq!(
        retained
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .upgrade()
            .unwrap()
            .value,
        7
    );
    drop(retained.lock().unwrap().take());
    Arc::get_mut(&mut proxy).unwrap().load(&mut archive);
    // The registered factory creates the canonical holder for Payload, and
    // proxy keeps that holder allocated throughout the type check and read.
    let holder =
        unsafe { srpc::serializable::serializable_holder_of::<Payload>(Arc::as_ptr(&proxy)) };
    assert!(!holder.is_null());
    // The type check above returned a non-null Payload holder owned by proxy.
    assert_eq!(unsafe { &*holder }.ptr.value, 99);
    SerializableRegistry::clear_for_testing();
}
