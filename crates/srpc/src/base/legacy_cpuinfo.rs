//! Legacy `rrr.cpuinfo` process telemetry.
//!
//! The historical public surface is deliberately small: `CPUInfo` exists so
//! C++ callers can invoke its all-static `cpu_stat` method.  The sample
//! history, constructor, `/proc` parsers, and delta routine were private in
//! the pre-module class.  Inline-DSL conversion temporarily exported those
//! implementation seams; this whole-file owner does not perpetuate them.
//!
//! The field order and carriers retain the final legacy layout: six ten-entry
//! histories, four cached rates, two signed-long-compatible counters, the
//! ring index and pid, then `Mutex<bool>`.  `cpp_no_auto_traits` preserves the
//! established generated-C++ trait surface while native Rust still derives
//! `Send` and `Sync` from its fields.
//!
//! Three behavioral oddities are intentional compatibility constraints:
//!
//! * samples less than or equal to 60 clock ticks apart reuse the cached
//!   result (or return four `-1.0` sentinels while warming up);
//! * network rates perform unsigned integer division before conversion to
//!   `f64`, so fractional bytes-per-tick are truncated;
//! * `/proc` integers use libc `strtoul` with base zero and therefore retain
//!   octal/hex detection and lenient trailing-garbage handling.

#![allow(non_camel_case_types)]
#![allow(unsafe_code)]

use cpp::rrr::logging as cpp_logging;
use cpp::rusty::sys as cpp_sys;
use cpp::std as cpp_std;
use std::sync::Mutex;

const CPUINFO_HISTORY: usize = 10_usize;
const CPUINFO_MIN_TICK_DELTA: i64 = 60_i64;

// `_SC_PAGE_SIZE` on the supported Linux/glibc consumer ABI.  This value is
// passed to the existing `rusty::sys::process::sysconf` seam; the non-Linux
// constructor does not evaluate it.
const LINUX_SC_PAGE_SIZE: i32 = 30_i32;

// `int8_t` is the established logging API's file-character type.  `char` is
// required for std::strtoul, so that call uses this separately mapped alias.
type LegacyCChar = i8;

#[repr(C)]
#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct CPUInfo {
    last_bytes_rxed: [u64; CPUINFO_HISTORY],
    last_bytes_txed: [u64; CPUINFO_HISTORY],
    last_mem_usage: [u64; CPUINFO_HISTORY],
    last_ticks_: [i64; CPUINFO_HISTORY],
    last_user_ticks_: [i64; CPUINFO_HISTORY],
    last_kernel_ticks_: [i64; CPUINFO_HISTORY],
    last_cpu: f64,
    last_txed: f64,
    last_rxed: f64,
    last_mem: f64,
    total_mem: i64,
    page_size: i64,
    index: i32,
    pid_: i32,
    // Retained for the exact legacy object layout.  The valid-Rust singleton
    // locks the whole CPUInfo value from the outside, which gives Rust a sound
    // mutable reference while preserving the same serialized sampling order.
    mtx_: Mutex<bool>,
}

impl CPUInfo {
    /// Sample process CPU, network, and resident-memory deltas.
    ///
    /// The result order is `[cpu, network-field-1, network-field-9, rss]`,
    /// exactly as in the legacy class.  The two network names were historically
    /// reversed relative to `/proc/<pid>/net/dev`; their positions stay frozen.
    pub fn cpu_stat() -> Vec<f64> {
        static INSTANCE: Mutex<Option<CPUInfo>> = Mutex::new(None);

        let mut slot = INSTANCE.lock().unwrap();
        if slot.is_none() {
            *slot = Some(cpuinfo_new());
        }
        cpuinfo_get_cpu_stat(slot.as_mut().unwrap())
    }
}

fn cpuinfo_blank() -> CPUInfo {
    CPUInfo {
        last_bytes_rxed: [0_u64; CPUINFO_HISTORY],
        last_bytes_txed: [0_u64; CPUINFO_HISTORY],
        last_mem_usage: [0_u64; CPUINFO_HISTORY],
        last_ticks_: [0_i64; CPUINFO_HISTORY],
        last_user_ticks_: [0_i64; CPUINFO_HISTORY],
        last_kernel_ticks_: [0_i64; CPUINFO_HISTORY],
        last_cpu: 0.0_f64,
        last_txed: 0.0_f64,
        last_rxed: 0.0_f64,
        last_mem: 0.0_f64,
        total_mem: 0_i64,
        page_size: 0_i64,
        index: 0_i32,
        pid_: 0_i32,
        mtx_: Mutex::new(false),
    }
}

#[cfg(target_os = "linux")]
fn cpuinfo_new() -> CPUInfo {
    let mut info = cpuinfo_blank();

    let mem_info = unsafe { cpp_sys::process::sysinfo() };
    info.total_mem = (mem_info.total_ram_bytes / 1_024_u64) as i64;
    let message: cpp_std::string = format!("total amount of ram is: {}", info.total_mem);
    unsafe {
        cpp_logging::log_line(4_i32, 0_i32, core::ptr::null::<i8>(), &message);
    }

    info.page_size = unsafe { cpp_sys::process::sysconf(LINUX_SC_PAGE_SIZE) } / 1_024_i64;

    let sample = unsafe { cpp_sys::process::process_times() };
    info.last_ticks_[0_usize] = sample.wall_ticks;
    info.last_kernel_ticks_[0_usize] = sample.system_ticks;
    info.last_user_ticks_[0_usize] = sample.user_ticks;

    info.pid_ = unsafe { cpp_sys::process::getpid() };
    let pid: cpp_std::string = format!("{}", info.pid_);
    let mut ignored: Vec<f64> = Vec::<f64>::new();
    let initial_ticks = info.last_ticks_[0_usize];
    cpuinfo_get_network(&mut info, &pid, &mut ignored, initial_ticks);
    cpuinfo_get_memory(&mut info, &pid, &mut ignored, initial_ticks);
    info.index = 1_i32;
    info
}

#[cfg(not(target_os = "linux"))]
fn cpuinfo_new() -> CPUInfo {
    let mut info = cpuinfo_blank();
    info.pid_ = unsafe { cpp_sys::process::getpid() };
    info
}

#[cfg(target_os = "linux")]
fn cpuinfo_get_cpu_stat(info: &mut CPUInfo) -> Vec<f64> {
    let mut result: Vec<f64> = Vec::<f64>::new();
    let sample = unsafe { cpp_sys::process::process_times() };
    let ticks = sample.wall_ticks;
    let stime = sample.system_ticks;
    let utime = sample.user_ticks;

    let last_ticks = if info.index < CPUINFO_HISTORY as i32 {
        info.last_ticks_[(info.index - 1_i32) as usize]
    } else {
        info.last_ticks_[CPUINFO_HISTORY - 1_usize]
    };

    let message: cpp_std::string = format!("ticks: {} -> {}", last_ticks, ticks);
    unsafe {
        cpp_logging::log_line(4_i32, 0_i32, core::ptr::null::<i8>(), &message);
    }

    if ticks <= last_ticks + CPUINFO_MIN_TICK_DELTA {
        if info.index < CPUINFO_HISTORY as i32 {
            result.push(-1.0_f64);
            result.push(-1.0_f64);
            result.push(-1.0_f64);
            result.push(-1.0_f64);
        } else {
            result.push(info.last_cpu);
            result.push(info.last_txed);
            result.push(info.last_rxed);
            result.push(info.last_mem);
        }
        return result;
    }

    if info.index < CPUINFO_HISTORY as i32 {
        let index = info.index as usize;
        info.last_kernel_ticks_[index] = stime;
        info.last_user_ticks_[index] = utime;
        info.last_ticks_[index] = ticks;
        info.index += 1_i32;
    } else {
        let mut index: usize = 0_usize;
        while index + 1_usize < CPUINFO_HISTORY {
            info.last_kernel_ticks_[index] = info.last_kernel_ticks_[index + 1_usize];
            info.last_user_ticks_[index] = info.last_user_ticks_[index + 1_usize];
            info.last_ticks_[index] = info.last_ticks_[index + 1_usize];
            index += 1_usize;
        }
        info.last_kernel_ticks_[CPUINFO_HISTORY - 1_usize] = stime;
        info.last_user_ticks_[CPUINFO_HISTORY - 1_usize] = utime;
        info.last_ticks_[CPUINFO_HISTORY - 1_usize] = ticks;
    }

    let cpu_total = if info.index < CPUINFO_HISTORY as i32 {
        -1.0_f64
    } else {
        let busy =
            (stime - info.last_kernel_ticks_[8_usize]) + (utime - info.last_user_ticks_[8_usize]);
        (busy as f64) / ((ticks - info.last_ticks_[8_usize]) as f64)
    };
    info.last_cpu = cpu_total;
    if info.index < CPUINFO_HISTORY as i32 {
        result.push(-1.0_f64);
    } else {
        result.push(cpu_total);
    }

    let pid: cpp_std::string = format!("{}", info.pid_);
    cpuinfo_get_network(info, &pid, &mut result, ticks);
    cpuinfo_get_memory(info, &pid, &mut result, ticks);
    result
}

#[cfg(not(target_os = "linux"))]
fn cpuinfo_get_cpu_stat(_info: &mut CPUInfo) -> Vec<f64> {
    vec![-1.0_f64, -1.0_f64, -1.0_f64, -1.0_f64]
}

fn cpuinfo_get_network(
    info: &mut CPUInfo,
    pid: &cpp_std::string,
    result: &mut Vec<f64>,
    ticks: i64,
) {
    let path: cpp_std::string = format!("/proc/{}/net/dev", pid);
    let content = cpuinfo_read_proc(&path);
    let bytes = content.as_bytes();
    let mut line_start: usize = 0_usize;
    let mut line_end: usize = 0_usize;
    cpuinfo_nth_line_range(bytes, 3_usize, &mut line_start, &mut line_end);

    let txed = cpuinfo_parse_field(bytes, line_start, line_end, 1_usize);
    let rxed = cpuinfo_parse_field(bytes, line_start, line_end, 9_usize);

    let mut tx_total: f64 = -1.0_f64;
    let mut rx_total: f64 = -1.0_f64;
    if info.index < CPUINFO_HISTORY as i32 {
        let index = info.index as usize;
        info.last_bytes_txed[index] = txed;
        info.last_bytes_rxed[index] = rxed;
    } else {
        let mut index: usize = 0_usize;
        while index + 1_usize < CPUINFO_HISTORY {
            info.last_bytes_txed[index] = info.last_bytes_txed[index + 1_usize];
            info.last_bytes_rxed[index] = info.last_bytes_rxed[index + 1_usize];
            index += 1_usize;
        }
        info.last_bytes_txed[CPUINFO_HISTORY - 1_usize] = txed;
        info.last_bytes_rxed[CPUINFO_HISTORY - 1_usize] = rxed;
    }

    if ticks != info.last_ticks_[0_usize] && info.index >= CPUINFO_HISTORY as i32 {
        let tick_delta = (ticks - info.last_ticks_[8_usize]) as u64;
        // Preserve the legacy unsigned-integer division, not floating-point
        // division.
        tx_total = (txed.wrapping_sub(info.last_bytes_txed[8_usize]) / tick_delta) as f64;
        rx_total = (rxed.wrapping_sub(info.last_bytes_rxed[8_usize]) / tick_delta) as f64;
    }

    result.push(tx_total);
    result.push(rx_total);
    info.last_txed = tx_total;
    info.last_rxed = rx_total;
}

fn cpuinfo_get_memory(
    info: &mut CPUInfo,
    pid: &cpp_std::string,
    result: &mut Vec<f64>,
    ticks: i64,
) {
    let path: cpp_std::string = format!("/proc/{}/stat", pid);
    let content = cpuinfo_read_proc(&path);
    let bytes = content.as_bytes();
    let rss = cpuinfo_parse_field(bytes, 0_usize, bytes.len(), 23_usize) as i64;
    let mem_usage = (rss * info.page_size) as f64;
    let mut mem_total: f64 = -1.0_f64;

    if info.index < CPUINFO_HISTORY as i32 {
        info.last_mem_usage[info.index as usize] = mem_usage as u64;
    } else {
        let mut index: usize = 0_usize;
        while index + 1_usize < CPUINFO_HISTORY {
            info.last_mem_usage[index] = info.last_mem_usage[index + 1_usize];
            index += 1_usize;
        }
        info.last_mem_usage[CPUINFO_HISTORY - 1_usize] = mem_usage as u64;
    }

    if ticks != info.last_ticks_[0_usize] && info.index >= CPUINFO_HISTORY as i32 {
        mem_total = (mem_usage - info.last_mem_usage[8_usize] as f64)
            / (ticks - info.last_ticks_[8_usize]) as f64;
    }

    result.push(mem_total);
    info.last_mem = mem_total;
}

fn cpuinfo_read_proc(path: &cpp_std::string) -> cpp_std::string {
    let result = unsafe { cpp_sys::fs::read_to_string(path) };
    if result.is_err() {
        return format!("");
    }
    result.unwrap()
}

fn cpuinfo_nth_line_range(bytes: &[u8], wanted: usize, out_start: &mut usize, out_end: &mut usize) {
    let mut start: usize = 0_usize;
    let mut line: usize = 0_usize;
    while line < wanted {
        let mut newline = start;
        while newline < bytes.len() && bytes[newline] != b'\n' {
            newline += 1_usize;
        }
        if newline == bytes.len() {
            *out_start = bytes.len();
            *out_end = bytes.len();
            return;
        }
        start = newline + 1_usize;
        line += 1_usize;
    }

    let mut end = start;
    while end < bytes.len() && bytes[end] != b'\n' {
        end += 1_usize;
    }
    *out_start = start;
    *out_end = end;
}

fn cpuinfo_parse_field(bytes: &[u8], line_start: usize, line_end: usize, wanted: usize) -> u64 {
    let mut start = line_start;
    while start < line_end && bytes[start] == b' ' {
        start += 1_usize;
    }

    let mut field: usize = 0_usize;
    while start < line_end {
        let mut end = start;
        while end < line_end && bytes[end] != b' ' {
            end += 1_usize;
        }
        if field == wanted {
            return cpuinfo_parse_ulong_range(bytes, start, end);
        }
        start = end;
        while start < line_end && bytes[start] == b' ' {
            start += 1_usize;
        }
        field += 1_usize;
    }
    0_u64
}

fn cpuinfo_parse_ulong_range(bytes: &[u8], start: usize, end: usize) -> u64 {
    let mut token: Vec<u8> = Vec::<u8>::new();
    let mut index = start;
    while index < end {
        token.push(bytes[index]);
        index += 1_usize;
    }
    token.push(0_u8);

    let pointer = token.as_ptr() as *const LegacyCChar;
    unsafe { cpp_std::strtoul(pointer, core::ptr::null_mut(), 0_i32) as u64 }
}

// Cargo-only definitions for reserved `cpp::` imports.  The C++ consumer
// suppresses this module and resolves the calls through a fail-closed symbol
// index against `std`, `rusty`, and the legacy `rrr.logging` module.
#[allow(dead_code)]
pub(crate) mod cpp {
    #[cfg(test)]
    pub(crate) fn test_parse_token(token: &str) -> u64 {
        super::cpuinfo_parse_ulong_range(token.as_bytes(), 0_usize, token.len())
    }

    #[cfg(test)]
    pub(crate) fn test_parse_network_line(line: &str) -> (u64, u64) {
        let bytes = line.as_bytes();
        (
            super::cpuinfo_parse_field(bytes, 0_usize, bytes.len(), 1_usize),
            super::cpuinfo_parse_field(bytes, 0_usize, bytes.len(), 9_usize),
        )
    }

    pub mod std {
        #[allow(non_camel_case_types)]
        pub type string = String;

        mod ffi {
            unsafe extern "C" {
                pub(super) fn strtoul(
                    value: *const core::ffi::c_char,
                    end: *mut *mut core::ffi::c_char,
                    base: i32,
                ) -> usize;
            }
        }

        pub unsafe fn strtoul(
            value: *const core::ffi::c_char,
            end: *mut *mut core::ffi::c_char,
            base: i32,
        ) -> usize {
            unsafe { ffi::strtoul(value, end, base) }
        }
    }

    pub mod rrr {
        pub mod logging {
            pub unsafe fn log_line(_level: i32, _line: i32, _file: *const i8, _message: &String) {
                // Native tests do not install the C++ logging sink.  Generated
                // consumers call the real rrr.logging function directly.
            }
        }
    }

    pub mod rusty {
        pub mod sys {
            pub mod fs {
                pub unsafe fn read_to_string(path: &String) -> ::std::io::Result<String> {
                    ::std::fs::read_to_string(path)
                }
            }

            pub mod process {
                #[derive(Clone, Copy)]
                pub struct ProcessTimes {
                    pub wall_ticks: i64,
                    pub user_ticks: i64,
                    pub system_ticks: i64,
                }

                #[cfg(target_os = "linux")]
                #[derive(Clone, Copy)]
                pub struct SysInfo {
                    pub total_ram_bytes: u64,
                }

                #[repr(C)]
                struct NativeTms {
                    user: core::ffi::c_long,
                    system: core::ffi::c_long,
                    child_user: core::ffi::c_long,
                    child_system: core::ffi::c_long,
                }

                #[cfg(target_os = "linux")]
                #[repr(C)]
                struct NativeSysInfo {
                    uptime: core::ffi::c_long,
                    loads: [core::ffi::c_ulong; 3],
                    total_ram: core::ffi::c_ulong,
                    free_ram: core::ffi::c_ulong,
                    shared_ram: core::ffi::c_ulong,
                    buffer_ram: core::ffi::c_ulong,
                    total_swap: core::ffi::c_ulong,
                    free_swap: core::ffi::c_ulong,
                    procs: core::ffi::c_ushort,
                    pad: core::ffi::c_ushort,
                    total_high: core::ffi::c_ulong,
                    free_high: core::ffi::c_ulong,
                    mem_unit: core::ffi::c_uint,
                    #[cfg(target_pointer_width = "32")]
                    filler: [u8; 8],
                }

                mod ffi {
                    unsafe extern "C" {
                        pub(super) fn times(buffer: *mut super::NativeTms) -> core::ffi::c_long;
                        pub(super) fn sysconf(name: i32) -> core::ffi::c_long;

                        #[cfg(target_os = "linux")]
                        pub(super) fn sysinfo(buffer: *mut super::NativeSysInfo) -> i32;
                    }
                }

                pub unsafe fn getpid() -> i32 {
                    ::std::process::id() as i32
                }

                pub unsafe fn sysconf(name: i32) -> i64 {
                    unsafe { ffi::sysconf(name) as i64 }
                }

                pub unsafe fn process_times() -> ProcessTimes {
                    let mut native = NativeTms {
                        user: 0,
                        system: 0,
                        child_user: 0,
                        child_system: 0,
                    };
                    let wall = unsafe { ffi::times(&mut native) } as i64;
                    ProcessTimes {
                        wall_ticks: wall,
                        user_ticks: native.user as i64,
                        system_ticks: native.system as i64,
                    }
                }

                #[cfg(target_os = "linux")]
                pub unsafe fn sysinfo() -> SysInfo {
                    let mut native = NativeSysInfo {
                        uptime: 0,
                        loads: [0; 3],
                        total_ram: 0,
                        free_ram: 0,
                        shared_ram: 0,
                        buffer_ram: 0,
                        total_swap: 0,
                        free_swap: 0,
                        procs: 0,
                        pad: 0,
                        total_high: 0,
                        free_high: 0,
                        mem_unit: 0,
                        #[cfg(target_pointer_width = "32")]
                        filler: [0; 8],
                    };
                    let _ = unsafe { ffi::sysinfo(&mut native) };
                    let unit = if native.mem_unit == 0 {
                        1_u64
                    } else {
                        native.mem_unit as u64
                    };
                    SysInfo {
                        total_ram_bytes: (native.total_ram as u64).wrapping_mul(unit),
                    }
                }
            }
        }
    }
}
