use srpc::utils::{find_open_port, get_host_name, AddrInfo};
use std::mem::{align_of, size_of};
use std::sync::atomic::{AtomicI32, Ordering};

static NEXT_PORT: AtomicI32 = AtomicI32::new(0);

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_find_open_port() -> i32 {
    NEXT_PORT.load(Ordering::Relaxed)
}

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

#[test]
#[allow(unsafe_code)]
fn addrinfo_layout_empty_state_and_rust_traits_are_pinned() {
    assert_eq!(size_of::<AddrInfo>(), 16);
    assert_eq!(align_of::<AddrInfo>(), 8);
    let empty = AddrInfo::new();
    assert!(!empty.valid());
    assert!(empty.get().is_null());

    // SAFETY: null is explicitly permitted by `adopt`; Drop performs no free.
    let adopted_null = unsafe { AddrInfo::adopt(core::ptr::null_mut()) };
    assert!(!adopted_null.valid());

    assert_not_auto_trait!(AddrInfo, Send);
    assert_not_auto_trait!(AddrInfo, Sync);
}

#[test]
fn port_contract_and_hostname_facade_are_rustc_exercised() {
    NEXT_PORT.store(43_210, Ordering::Relaxed);
    assert_eq!(find_open_port(), 43_210);

    NEXT_PORT.store(0, Ordering::Relaxed);
    assert_eq!(find_open_port(), -1);
    NEXT_PORT.store(-7, Ordering::Relaxed);
    assert_eq!(find_open_port(), -1);

    // The direct-rustc facade may return an empty name when the environment
    // lacks HOSTNAME. The generated C++ runtime lane separately pins the real
    // gethostname success and failure behavior.
    let _ = get_host_name();
}
