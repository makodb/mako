use srpc::debugging;

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
extern "C" fn srpc_stderr() -> *mut rusty::CFile {
    core::ptr::null_mut()
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
extern "C" fn srpc_backtrace_capture(_out_symbols: *mut *mut *mut i8) -> i32 {
    -1_i32
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
extern "C" fn srpc_backtrace_free(_symbols: *mut *mut i8) {}

#[test]
fn branch_hints_preserve_boolean_identity() {
    assert!(debugging::likely(true));
    assert!(!debugging::likely(false));
    assert!(debugging::unlikely(true));
    assert!(!debugging::unlikely(false));
}

#[test]
fn explicit_rust_source_location_drives_the_success_path() {
    let location = rusty::SourceLocation::current();
    debugging::verify(&true, &location);
}
