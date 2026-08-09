use srpc::base::callback_wrapper::detail::CallbackWrapper;
use std::cell::Cell;

#[test]
fn default_is_empty_and_factory_is_present() {
    let empty = CallbackWrapper::<Box<dyn Fn()>>::default();
    assert!(!empty.has_value());

    let callback = CallbackWrapper::from_callable(|| 42);
    assert!(callback.has_value());
    assert_eq!(callback.callable()(), 42);
}

#[test]
fn clones_share_the_same_callable_state() {
    let callback = CallbackWrapper::from_callable(Cell::new(0_u32));
    let copy = callback.clone();

    callback.callable().set(7);
    assert_eq!(copy.callable().get(), 7);
    assert!(core::ptr::eq(callback.callable(), copy.callable()));
}

#[test]
fn move_only_callables_and_multiple_arities_work() {
    let captured = Box::new(5_i32);
    let unary = CallbackWrapper::from_callable(move |value: i32| value + *captured);
    assert_eq!(unary.callable()(8), 13);

    let binary = CallbackWrapper::from_callable(|left: i32, right: i32| left * right);
    assert_eq!(binary.callable()(6, 7), 42);
}
