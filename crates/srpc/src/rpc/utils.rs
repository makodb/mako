//! Legacy `rrr.utils` compatibility surface.
//!
//! This valid-Rust owner preserves the final three-item C++ API: the move-only
//! `AddrInfo` RAII wrapper, the plain-C open-port shim, and hostname lookup.
//! APIs removed before whole-file conversion remain removed.

#![allow(non_camel_case_types)]
#![allow(unsafe_code)]

use crate::base::legacy_logging::{log_line, Log};
use cpp::rusty::sys as cpp_sys;
use std::cell::Cell;

type LegacyAddrInfo = core::ffi::c_void;
type LegacyStdString = String;

/// Move-only owner of a libc `addrinfo` chain.
#[repr(C)]
pub struct AddrInfo {
    pub info_: *mut LegacyAddrInfo,
    pub owned_: Cell<bool>,
}

impl AddrInfo {
    /// Construct an invalid, non-owning value.
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> AddrInfo {
        AddrInfo {
            info_: core::ptr::null_mut(),
            owned_: Cell::new(false),
        }
    }

    /// Adopt a raw `addrinfo` chain returned by libc.
    #[cfg_attr(any(), cpp_ctor)]
    pub fn adopt(info: *mut LegacyAddrInfo) -> AddrInfo {
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
    fn drop(&mut self) {
        if !self.info_.is_null() {
            unsafe { utils_ffi::freeaddrinfo(self.info_) };
        }
    }
}

mod utils_ffi {
    use super::LegacyAddrInfo;

    unsafe extern "C" {
        pub(super) fn freeaddrinfo(info: *mut LegacyAddrInfo);
        pub(super) fn srpc_find_open_port() -> i32;
    }
}

/// Return the first bindable port from the existing C scan, or `-1`.
pub fn find_open_port() -> i32 {
    let port: i32 = unsafe { utils_ffi::srpc_find_open_port() };
    if port > 0_i32 {
        let message: LegacyStdString = format!("Found open port: {}", port);
        log_line(Log::INFO, 0_i32, core::ptr::null::<i8>(), &message);
        return port;
    }
    let message: LegacyStdString = "Failed to find open port.".to_string();
    log_line(Log::ERROR, 0_i32, core::ptr::null::<i8>(), &message);
    -1_i32
}

/// Return the host name, logging and preserving an empty result on failure.
pub fn get_host_name() -> LegacyStdString {
    let name: LegacyStdString = unsafe { cpp_sys::env::hostname() };
    if name.is_empty() {
        let message: LegacyStdString = "Failed to get hostname.".to_string();
        log_line(Log::ERROR, 0_i32, core::ptr::null::<i8>(), &message);
    }
    name
}

// Cargo-only definition for the reserved `cpp::rusty::sys` import.  Generated
// C++ resolves the indexed `rusty::sys::env::hostname` function instead.
#[allow(dead_code)]
pub(crate) mod cpp {
    pub mod rusty {
        pub mod sys {
            pub mod env {
                type GetHostname = unsafe extern "C" fn(*mut core::ffi::c_char, usize) -> i32;

                mod ffi {
                    unsafe extern "C" {
                        pub(super) fn gethostname(
                            name: *mut core::ffi::c_char,
                            length: usize,
                        ) -> i32;
                    }
                }

                unsafe fn hostname_with(gethostname: GetHostname) -> String {
                    let mut buffer = [0_u8; 256];
                    let rc = unsafe {
                        gethostname(buffer.as_mut_ptr() as *mut core::ffi::c_char, 255_usize)
                    };
                    if rc != 0_i32 {
                        return String::new();
                    }

                    buffer[255_usize] = 0_u8;
                    let length = buffer
                        .iter()
                        .position(|byte| *byte == 0_u8)
                        .unwrap_or(buffer.len());
                    String::from_utf8_lossy(&buffer[..length]).into_owned()
                }

                pub unsafe fn hostname() -> String {
                    unsafe { hostname_with(ffi::gethostname) }
                }

                #[cfg(test)]
                mod tests {
                    unsafe extern "C" fn fail(
                        _name: *mut core::ffi::c_char,
                        _length: usize,
                    ) -> i32 {
                        -1_i32
                    }

                    unsafe extern "C" fn no_nul(
                        name: *mut core::ffi::c_char,
                        length: usize,
                    ) -> i32 {
                        let mut index = 0_usize;
                        while index < length {
                            unsafe { *name.add(index) = b'x' as core::ffi::c_char };
                            index += 1_usize;
                        }
                        0_i32
                    }

                    unsafe extern "C" fn embedded_nul(
                        name: *mut core::ffi::c_char,
                        _length: usize,
                    ) -> i32 {
                        let bytes = b"node\0ignored";
                        let mut index = 0_usize;
                        while index < bytes.len() {
                            unsafe { *name.add(index) = bytes[index] as core::ffi::c_char };
                            index += 1_usize;
                        }
                        0_i32
                    }

                    #[test]
                    fn failure_returns_empty() {
                        assert_eq!(unsafe { super::hostname_with(fail) }, "");
                    }

                    #[test]
                    fn forced_last_nul_bounds_a_truncated_result() {
                        let result = unsafe { super::hostname_with(no_nul) };
                        assert_eq!(result.len(), 255_usize);
                        assert!(result.bytes().all(|byte| byte == b'x'));
                    }

                    #[test]
                    fn owned_result_stops_at_the_first_nul() {
                        assert_eq!(unsafe { super::hostname_with(embedded_nul) }, "node");
                    }
                }
            }
        }
    }
}
