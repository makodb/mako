#![allow(unsafe_code)]

use srpc::base::legacy_logging::Log;
use srpc::rpc::utils::{find_open_port, get_host_name, AddrInfo};
use std::ffi::CStr;
use std::sync::atomic::{AtomicI32, Ordering};

static OPEN_PORT_RESULT: AtomicI32 = AtomicI32::new(-1_i32);

#[unsafe(no_mangle)]
extern "C" fn srpc_path_basename(_path: *const core::ffi::c_char) -> *const core::ffi::c_char {
    core::ptr::null()
}

#[unsafe(no_mangle)]
extern "C" fn srpc_time_now_str(_now: *mut core::ffi::c_char) {}

#[unsafe(no_mangle)]
extern "C" fn srpc_find_open_port() -> i32 {
    OPEN_PORT_RESULT.load(Ordering::Relaxed)
}

#[test]
fn addrinfo_default_layout_and_traits_match_the_legacy_owner() {
    let info = AddrInfo::new();
    assert!(!info.valid());
    assert!(info.get().is_null());
    assert!(!info.owned_.get());
    assert_eq!(core::mem::size_of::<AddrInfo>(), 16_usize);
    assert_eq!(core::mem::align_of::<AddrInfo>(), 8_usize);
    assert!(!core::mem::needs_drop::<*mut core::ffi::c_void>());
    assert!(core::mem::needs_drop::<AddrInfo>());
}

#[test]
fn adopt_owns_a_real_libc_addrinfo_chain_across_a_rust_move() {
    unsafe extern "C" {
        fn getaddrinfo(
            node: *const core::ffi::c_char,
            service: *const core::ffi::c_char,
            hints: *const core::ffi::c_void,
            result: *mut *mut core::ffi::c_void,
        ) -> i32;
    }

    let node = b"127.0.0.1\0";
    let mut raw = core::ptr::null_mut();
    let status = unsafe {
        getaddrinfo(
            node.as_ptr() as *const core::ffi::c_char,
            core::ptr::null(),
            core::ptr::null(),
            &mut raw,
        )
    };
    assert_eq!(status, 0_i32);
    assert!(!raw.is_null());

    let adopted = AddrInfo::adopt(raw);
    assert!(adopted.valid());
    assert_eq!(adopted.get(), raw);
    assert!(adopted.owned_.get());

    let moved = adopted;
    assert_eq!(moved.get(), raw);
    drop(moved);
}

#[test]
fn open_port_wrapper_preserves_positive_and_failure_results() {
    Log::set_level(-1_i32);

    OPEN_PORT_RESULT.store(42_424_i32, Ordering::Relaxed);
    assert_eq!(find_open_port(), 42_424_i32);

    OPEN_PORT_RESULT.store(0_i32, Ordering::Relaxed);
    assert_eq!(find_open_port(), -1_i32);

    OPEN_PORT_RESULT.store(-1_i32, Ordering::Relaxed);
    assert_eq!(find_open_port(), -1_i32);

    Log::set_level(Log::DEBUG);
}

#[test]
fn native_hostname_matches_the_live_direct_libc_result() {
    unsafe extern "C" {
        fn gethostname(name: *mut core::ffi::c_char, length: usize) -> i32;
    }

    let mut buffer = [0_u8; 256];
    let rc = unsafe { gethostname(buffer.as_mut_ptr() as *mut core::ffi::c_char, 255_usize) };
    assert_eq!(rc, 0_i32);
    buffer[255_usize] = 0_u8;
    let direct = unsafe { CStr::from_ptr(buffer.as_ptr() as *const core::ffi::c_char) }
        .to_string_lossy()
        .into_owned();
    assert!(!direct.is_empty());
    assert_eq!(get_host_name(), direct);
}

#[test]
fn removed_utils_apis_are_not_reintroduced() {
    let owner = include_str!("../src/rpc/utils.rs");
    assert_eq!(owner.matches("pub struct AddrInfo").count(), 1_usize);
    assert_eq!(owner.matches("pub fn find_open_port").count(), 1_usize);
    assert_eq!(owner.matches("pub fn get_host_name").count(), 1_usize);
    assert!(!owner.contains("pub fn set_nonblocking"));
    assert!(!owner.contains("pub fn addrinfo_resolve"));
}
