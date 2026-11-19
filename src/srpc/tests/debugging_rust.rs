use srpc::debugging;

#[test]
fn branch_hints_preserve_boolean_identity() {
    assert!(debugging::likely(true));
    assert!(!debugging::likely(false));
    assert!(debugging::unlikely(true));
    assert!(!debugging::unlikely(false));
}

#[test]
fn explicit_rust_source_location_drives_the_success_path() {
    let location = core::panic::Location::caller();
    debugging::verify(&true, location);
}

#[test]
fn source_location_records_the_caller() {
    let expected_line = line!() + 1;
    let location = core::panic::Location::caller();
    assert_eq!(location.file(), file!());
    assert_eq!(location.line(), expected_line);
}

#[test]
fn verification_failure_renders_a_real_backtrace_and_reports_the_call_site() {
    let failure = std::panic::catch_unwind(|| debugging::verify_at(false, "canonical-test.rs", 37));
    let payload = failure.expect_err("a failed verification must panic");
    let message = payload.downcast_ref::<String>().map(String::as_str)
        .or_else(|| payload.downcast_ref::<&str>().copied()).unwrap();
    assert!(message.contains("canonical-test.rs"), "{message}");
    assert!(message.contains("37"), "{message}");
}
