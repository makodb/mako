use srpc::utils::{find_open_port, get_host_name, AddrInfo};
use std::mem::{align_of, size_of};
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
fn port_scan_skips_a_socket_bound_by_a_real_listener() {
    let first = find_open_port();
    assert!((1024..65000).contains(&first));
    // The historical scan writes each candidate directly into sin_port.
    // Keep testing that existing representation while moving its policy.
    let socket_port = u16::from_be(first as u16);
    let occupied = std::net::TcpListener::bind(("0.0.0.0", socket_port)).unwrap();
    let next = find_open_port();
    assert!(
        next > first,
        "occupied candidate {first} was selected again: {next}"
    );
    drop(occupied);
    assert_eq!(find_open_port(), first);
}

#[test]
fn hostname_query_executes_with_the_native_kernel_linked() {
    let kernel_name = std::fs::read_to_string("/proc/sys/kernel/hostname").unwrap();
    assert_eq!(get_host_name(), kernel_name.trim_end_matches('\n'));
}
