use rrr::callback_wrapper::detail::CallbackWrapper;
use std::mem::{align_of, offset_of, size_of};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

struct StatefulCallable {
    calls: Arc<AtomicUsize>,
}

impl StatefulCallable {
    fn call(&self) -> usize {
        self.calls.fetch_add(1, Ordering::SeqCst) + 1
    }
}

fn assert_send_sync<T: Send + Sync>() {}
fn assert_clone_default<T: Clone + Default>() {}

#[test]
fn public_layout_and_traits_match_the_single_field_contract() {
    type Wrapper = CallbackWrapper<StatefulCallable>;
    type Field = Option<Arc<StatefulCallable>>;

    assert_clone_default::<Wrapper>();
    assert_send_sync::<Wrapper>();
    assert_eq!(offset_of!(Wrapper, inner), 0);
    assert_eq!(size_of::<Wrapper>(), size_of::<Field>());
    assert_eq!(align_of::<Wrapper>(), align_of::<Field>());
    assert_eq!(size_of::<Wrapper>(), size_of::<usize>());
    assert_eq!(align_of::<Wrapper>(), align_of::<usize>());

    let mut wrapper = CallbackWrapper::<fn()>::default();
    assert!(!wrapper.has_value());
    assert!(wrapper.inner.is_none());
    wrapper.inner = Some(Arc::new(|| {}));
    assert!(wrapper.has_value());
}

#[test]
fn from_callable_exposes_the_original_callable() {
    let calls = Arc::new(AtomicUsize::new(0));
    let wrapper = CallbackWrapper::from_callable(StatefulCallable {
        calls: Arc::clone(&calls),
    });

    assert!(wrapper.has_value());
    assert_eq!(wrapper.callable().call(), 1);
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn clone_shares_even_a_non_clone_callable() {
    let calls = Arc::new(AtomicUsize::new(0));
    let wrapper = CallbackWrapper::from_callable(StatefulCallable {
        calls: Arc::clone(&calls),
    });
    let cloned = wrapper.clone();

    assert!(std::ptr::eq(wrapper.callable(), cloned.callable()));
    assert_eq!(wrapper.callable().call(), 1);
    assert_eq!(cloned.callable().call(), 2);
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}
