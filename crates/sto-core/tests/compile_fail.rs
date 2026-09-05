#[test]
fn scoped_and_thread_local_capabilities_do_not_escape() {
    let cases = trybuild::TestCases::new();
    cases.compile_fail("tests/ui/*.rs");
}
