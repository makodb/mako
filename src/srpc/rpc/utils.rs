// Canonical Rust source for the srpc.utils module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use crate::logging::log_line;
use std::cell::Cell;

// The emitter maps these source-level aliases back to the exact legacy C++
// spellings (`addrinfo` and `std::string`) through the checked type-map.
type LegacyAddrInfo = core::ffi::c_void;

#[allow(unsafe_code)]
mod utils_ffi {
    use super::LegacyAddrInfo;

    unsafe extern "C" {
        pub(super) fn freeaddrinfo(info: *mut LegacyAddrInfo);
        pub(super) fn srpc_net_socket_open() -> i32;
        pub(super) fn srpc_net_resolve_any() -> *mut LegacyAddrInfo;
        pub(super) fn srpc_net_bind_port(fd: i32, address: *mut LegacyAddrInfo, port_native: u16) -> i32;
        pub(super) fn srpc_net_socket_name_status(fd: i32) -> i32;
        pub(super) fn srpc_net_close(fd: i32) -> i32;
        pub(super) fn srpc_net_hostname(buffer: *mut u8, capacity: usize) -> i32;
    }
}

/// Move-only owner of a libc `addrinfo` chain.
#[repr(C)]
pub struct AddrInfo {
    info_: *mut LegacyAddrInfo,
    // The marker is load-bearing for the translated C++ surface: rusty::Cell
    // keeps AddrInfo non-copyable, as the retired carrier was.
    owned_: Cell<bool>,
}

impl AddrInfo {
    /// Construct an invalid, non-owning value.
    #[allow(clippy::new_without_default)]
    pub fn new() -> AddrInfo {
        AddrInfo {
            info_: core::ptr::null_mut(),
            owned_: Cell::new(false),
        }
    }

    /// Adopt a raw `addrinfo` chain returned by libc.
    ///
    /// # Safety
    ///
    /// `info` must be null or a uniquely owned chain returned by a compatible
    /// libc allocation routine. After this call, the caller must not free or
    /// otherwise use the chain through another owning handle.
    #[allow(unsafe_code)]
    pub unsafe fn adopt(info: *mut LegacyAddrInfo) -> AddrInfo {
        AddrInfo {
            info_: info,
            owned_: Cell::new(true),
        }
    }

    pub fn get(&self) -> *mut LegacyAddrInfo {
        self.info_
    }

    pub fn valid(&self) -> bool {
        !self.info_.is_null()
    }
}

impl Drop for AddrInfo {
    #[allow(unsafe_code)]
    fn drop(&mut self) {
        if !self.info_.is_null() {
            unsafe { utils_ffi::freeaddrinfo(self.info_) };
        }
    }
}

// Scan and cleanup are shared by both compiler lanes. The C operations only
// construct platform address layouts and execute individual socket calls.
#[allow(unsafe_code)]
fn scan_open_port() -> i32 {
    let fd = unsafe { utils_ffi::srpc_net_socket_open() };
    if fd < 0 {
        return -1;
    }
    let local = unsafe { utils_ffi::srpc_net_resolve_any() };
    if local.is_null() {
        unsafe { utils_ffi::srpc_net_close(fd) };
        return -1;
    }
    let mut port: i32 = 0;
    let mut candidate: i32 = 1024;
    while candidate < 65000 {
        if unsafe { utils_ffi::srpc_net_bind_port(fd, local, candidate as u16) } != 0 {
            candidate += 1;
            continue;
        }
        if unsafe { utils_ffi::srpc_net_socket_name_status(fd) } != 0 {
            port = -1;
        } else {
            port = candidate;
        }
        break;
    }
    unsafe {
        utils_ffi::freeaddrinfo(local);
        utils_ffi::srpc_net_close(fd);
    }
    port
}

/// Return the first bindable port in the historical scan order, or -1.
#[allow(unsafe_code)]
pub fn find_open_port() -> i32 {
    let port = scan_open_port();
    if port > 0 {
        let mut message: String = "Found open port: ".to_string();
        message += &port.to_string();
        // SAFETY: the file pointer is null, so the logger performs no path scan.
        unsafe { log_line(3, 0, core::ptr::null(), &message) };
        return port;
    }

    let message: String = "Failed to find open port.".to_string();
    // SAFETY: the file pointer is null, so the logger performs no path scan.
    unsafe { log_line(1, 0, core::ptr::null(), &message) };
    -1
}

/// Return the host name, logging and preserving an empty result on failure.
#[allow(unsafe_code)]
pub fn get_host_name() -> String {
    let mut bytes: [u8; 256] = [0; 256];
    let status = unsafe { utils_ffi::srpc_net_hostname(bytes.as_mut_ptr(), 255) };
    let mut length: usize = 0;
    if status == 0 {
        while length < 255 && bytes[length] != 0 {
            length += 1;
        }
    }
    let name: String = String::from_utf8_lossy(&bytes[..length]).to_string();
    if name.is_empty() {
        let message: String = "Failed to get hostname.".to_string();
        // SAFETY: the file pointer is null, so the logger performs no path scan.
        unsafe { log_line(1, 0, core::ptr::null(), &message) };
    }
    name
}
