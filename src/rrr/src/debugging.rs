// Canonical Rust source for the rrr.debugging module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
//! Canonical Rust owner for branch hints, verification, and stack traces.

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
    use super::LegacyCChar;

    unsafe extern "C" {
        pub(super) fn srpc_stderr() -> *mut rusty::CFile;
        pub(super) fn srpc_backtrace_capture(out_symbols: *mut *mut *mut LegacyCChar) -> i32;
        pub(super) fn srpc_backtrace_free(symbols: *mut *mut LegacyCChar);
        pub(super) fn fputs(text: *const LegacyCChar, stream: *mut rusty::CFile) -> i32;
    }
}

struct BtCapture {
    ok: bool,
    symbols: Vec<rusty::LoggingString>,
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
        let mut symbol = bt_empty_string();
        // SAFETY: the C seam returns `frame_count` readable pointers.
        let symbol_pointer = unsafe { *raw_symbols.add(index as usize) };
        let mut offset = 0_usize;
        // SAFETY: each returned pointer names a readable NUL-terminated string.
        while unsafe { *symbol_pointer.add(offset) } != 0 as LegacyCChar {
            // SAFETY: offset advances only through bytes preceding the NUL.
            symbol.push_back(unsafe { *symbol_pointer.add(offset) });
            offset += 1_usize;
        }
        capture.symbols.push(symbol);
        index += 1_i32;
    }
    // SAFETY: `raw_symbols` is exactly the allocation returned by the C seam.
    unsafe { debugging_ffi::srpc_backtrace_free(raw_symbols) };
    capture
}

fn bt_index_prefix(index: i32) -> rusty::LoggingString {
    let mut output = bt_empty_string();
    output.append(&index.to_string());
    while output.size() < 3_usize {
        output.append(" ");
    }
    output.append("  ");
    output
}

fn bt_empty_string() -> rusty::LoggingString {
    Default::default()
}

fn bt_render(capture: &BtCapture) -> rusty::LoggingString {
    let mut output = bt_empty_string();
    if !capture.ok {
        output.append("  *** failed to obtain stack trace!\n");
        return output;
    }

    output.append("  *** begin stack trace ***\n");
    let mut index = 0_usize;
    while index < capture.symbols.len() {
        output.append(bt_index_prefix(index as i32));
        output.append(&capture.symbols[index]);
        output.append("\n");
        index += 1_usize;
    }
    output.append("  ***  end stack trace  ***\n");
    output
}

/// Print the current in-process stack trace to `stream`.
///
/// # Safety
///
/// `stream` must point to a live libc `FILE` object.
#[allow(unsafe_code)]
pub unsafe fn print_stack_trace(
    #[cfg_attr(any(), cpp_default_argument(stderr))] stream: *mut ::rusty::CFile,
) {
    let capture = bt_capture();
    let report = bt_render(&capture);
    // SAFETY: generated C++ maps this call to `std::string::c_str`, and the
    // caller upholds the stream contract.
    unsafe {
        debugging_ffi::fputs(report.c_str(), stream);
    }
}

#[allow(unsafe_code)]
pub fn verify_failed(file: &str, line: u32) {
    // SAFETY: libc owns the returned process-wide stream for its lifetime.
    let error_stream = unsafe { debugging_ffi::srpc_stderr() };
    // SAFETY: the process-wide stderr stream is live for this synchronous call.
    unsafe { print_stack_trace(error_stream) };
    let mut message: rusty::LoggingString = Default::default();
    message.append("verify failed at ");
    message.append(file);
    message.append(", line ");
    message.append(&line.to_string());
    rusty::panic::do_panic(message)
}

/// Verify an expression while preserving the C++ caller-location default.
///
/// Canonical Rust callers pass an explicit rustc-only `SourceLocation`. The
/// inert parameter marker is intended to make generated C++ retain the legacy
/// `std::source_location::current()` default at every C++ call site.
pub fn verify<Expr>(
    expr: &Expr,
    #[cfg_attr(any(), cpp_default_argument(source_location))] location: &::rusty::SourceLocation,
) where
    Expr: Copy + Into<bool>,
{
    let value: bool = (*expr).into();
    if unlikely(!value) {
        verify_failed(location.file_name(), location.line());
    }
}
