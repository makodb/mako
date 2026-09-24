// Canonical Rust source for the srpc.utils module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use cpp::srpc::logging as cpp_logging;
use rusty as cpp;
use std::cell::Cell;

// The emitter maps these source-level aliases back to the exact legacy C++
// spellings (`addrinfo` and `std::string`) through the checked type-map.
type LegacyAddrInfo = core::ffi::c_void;
type LegacyStdString = String;

#[allow(unsafe_code)]
mod utils_ffi {
    use super::LegacyAddrInfo;

    unsafe extern "C" {
        pub(super) fn freeaddrinfo(info: *mut LegacyAddrInfo);
        pub(super) fn srpc_find_open_port() -> i32;
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

/// Return the first bindable port from the existing terminal C scan, or -1.
#[allow(unsafe_code)]
pub fn find_open_port() -> i32 {
    let port = unsafe { utils_ffi::srpc_find_open_port() };
    if port > 0 {
        let mut message: LegacyStdString = "Found open port: ".to_string();
        message += &port.to_string();
        // SAFETY: the file pointer is null, so the logger performs no path scan.
        unsafe { cpp_logging::log_line(3, 0, core::ptr::null(), &message) };
        return port;
    }

    let message: LegacyStdString = "Failed to find open port.".to_string();
    // SAFETY: the file pointer is null, so the logger performs no path scan.
    unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
    -1
}

/// Return the host name, logging and preserving an empty result on failure.
#[allow(unsafe_code)]
pub fn get_host_name() -> LegacyStdString {
    let name: LegacyStdString = rusty::sys::env::hostname();
    if name.is_empty() {
        let message: LegacyStdString = "Failed to get hostname.".to_string();
        // SAFETY: the file pointer is null, so the logger performs no path scan.
        unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
    }
    name
}
