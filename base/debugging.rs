//! Canonical Rust owner for branch hints, verification, and stack traces.

/// Opaque libc FILE binding; Rust only passes borrowed native pointers.
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct CFile {
    _opaque: [u8; 0],
}

type LegacyCChar = i8;

pub fn likely(value: bool) -> bool {
    // Rust has no stable branch-expect intrinsic. The generated function keeps
    // the exact public behavior; the C++ compiler can still inline this body.
    value
}

pub fn unlikely(value: bool) -> bool {
    // See `likely`: this is the stable-Rust spelling of the same boolean API.
    value
}

#[allow(unsafe_code)]
mod debugging_ffi {
    use super::{CFile, LegacyCChar};

    unsafe extern "C" {
        pub(super) fn srpc_stderr() -> *mut CFile;
        pub(super) fn srpc_backtrace_capture(out_symbols: *mut *mut *mut LegacyCChar) -> i32;
        pub(super) fn srpc_backtrace_free(symbols: *mut *mut LegacyCChar);
        pub(super) fn fwrite(
            data: *const core::ffi::c_void,
            size: usize,
            count: usize,
            stream: *mut CFile,
        ) -> usize;
    }
}

struct BtCapture {
    ok: bool,
    symbols: Vec<String>,
}

impl BtCapture {
    fn new() -> BtCapture {
        BtCapture {
            ok: false,
            symbols: Vec::new(),
        }
    }
}

#[allow(unsafe_code)]
fn bt_capture() -> BtCapture {
    let mut capture = BtCapture::new();
    let mut raw_symbols: *mut *mut LegacyCChar = core::ptr::null_mut();
    // SAFETY: the C seam initializes `raw_symbols` or returns a negative count.
    let frame_count = unsafe { debugging_ffi::srpc_backtrace_capture(&raw mut raw_symbols) };
    if frame_count < 0_i32 {
        return capture;
    }

    capture.ok = true;
    let mut index = 0_i32;
    while index < frame_count - 1_i32 {
        let mut symbol: Vec<u8> = Vec::new();
        // SAFETY: the C seam returns `frame_count` readable pointers.
        let symbol_pointer = unsafe { *raw_symbols.add(index as usize) };
        let mut offset = 0_usize;
        // SAFETY: each returned pointer names a readable NUL-terminated string.
        while unsafe { *symbol_pointer.add(offset) } != 0 as LegacyCChar {
            // SAFETY: offset advances only through bytes preceding the NUL.
            symbol.push(unsafe { *symbol_pointer.add(offset) } as u8);
            offset += 1_usize;
        }
        capture.symbols.push(String::from(String::from_utf8_lossy(symbol.as_slice())));
        index += 1_i32;
    }
    // SAFETY: `raw_symbols` is exactly the allocation returned by the C seam.
    unsafe { debugging_ffi::srpc_backtrace_free(raw_symbols) };
    capture
}

fn bt_index_prefix(index: i32) -> String {
    let mut output = String::new();
    output.push_str(&index.to_string());
    while output.len() < 3_usize {
        output.push(' ');
    }
    output.push_str("  ");
    output
}

fn bt_render(capture: &BtCapture) -> String {
    let mut output = String::new();
    if !capture.ok {
        output.push_str("  *** failed to obtain stack trace!\n");
        return output;
    }

    output.push_str("  *** begin stack trace ***\n");
    let mut index = 0_usize;
    while index < capture.symbols.len() {
        output.push_str(&bt_index_prefix(index as i32));
        output.push_str(capture.symbols[index].as_str());
        output.push('\n');
        index += 1_usize;
    }
    output.push_str("  ***  end stack trace  ***\n");
    output
}

/// Print the current in-process stack trace to `stream`.
///
/// # Safety
///
/// `stream` must point to a live libc `FILE` object.
#[allow(unsafe_code)]
pub unsafe fn print_stack_trace(
    #[cfg_attr(any(), cpp_default_argument(stderr))] stream: *mut CFile,
) {
    let capture = bt_capture();
    let report = bt_render(&capture);
    // SAFETY: `report` owns `report.len()` readable bytes for this synchronous
    // call, and the caller upholds the stream contract.
    unsafe {
        debugging_ffi::fwrite(
            report.as_ptr() as *const core::ffi::c_void,
            1,
            report.len(),
            stream,
        );
    }
}

#[allow(unsafe_code)]
pub fn verify_failed(file: &str, line: u32) {
    // SAFETY: libc owns the returned process-wide stream for its lifetime.
    let error_stream = unsafe { debugging_ffi::srpc_stderr() };
    // SAFETY: the process-wide stderr stream is live for this synchronous call.
    unsafe { print_stack_trace(error_stream) };
    let mut message = String::new();
    message.push_str("verify failed at ");
    message.push_str(file);
    message.push_str(", line ");
    message.push_str(&line.to_string());
    std::panic::panic_any(message)
}

/// Verify an expression while preserving the C++ caller-location default.
///
/// Canonical Rust callers pass a standard caller location. The
/// inert parameter marker makes generated C++ retain the
/// `std::source_location::current()` default at every C++ call site.
pub fn verify<Expr>(
    expr: &Expr,
    #[cfg_attr(any(), cpp_default_argument(source_location))] location: &::core::panic::Location<'_>,
) where
    Expr: Copy + Into<bool>,
{
    let value: bool = (*expr).into();
    verify_at(value, location.file(), location.line());
}

/// Verify at an explicit call site using the same implementation in both languages.
pub fn verify_at(value: bool, file: &str, line: u32) {
    if unlikely(!value) {
        verify_failed(file, line);
    }
}
