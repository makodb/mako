use srpc::fiber::this_fiber;

#[test]
fn context_identity_current_and_yield_preserve_behavior() {
    assert!(!this_fiber::in_fiber_context());
    assert_eq!(this_fiber::get_id(), 0);
    assert!(this_fiber::current().is_none());
    this_fiber::r#yield();

    let ((id, present), yields) = rusty::srpc::reactor::with_test_fiber(73, || {
        assert!(this_fiber::in_fiber_context());
        let current = this_fiber::current().expect("test fiber must be installed");
        this_fiber::r#yield();
        this_fiber::r#yield();
        (this_fiber::get_id(), current.id.get() == 73)
    });
    assert_eq!(id, 73);
    assert!(present);
    assert_eq!(yields, 2);
    assert!(!this_fiber::in_fiber_context());
}

#[test]
fn sleep_helpers_preserve_unsigned_wrapping_and_past_deadlines() {
    let _ = rusty::srpc::reactor::take_test_sleep_calls();

    this_fiber::sleep_us(17);
    this_fiber::sleep_ms(u64::MAX);
    this_fiber::sleep_s(u64::MAX);
    this_fiber::sleep_until_us(0);

    assert_eq!(
        rusty::srpc::reactor::take_test_sleep_calls(),
        vec![
            17,
            u64::MAX.wrapping_mul(1_000),
            u64::MAX.wrapping_mul(srpc::basetypes::SRPC_USEC_PER_SEC),
        ]
    );
}
