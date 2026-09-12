use sto_core::PredicateContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: PredicateContext<'_>) -> PredicateContext<'static> {
    context
}

fn main() {
    assert_send::<PredicateContext<'static>>();
    assert_sync::<PredicateContext<'static>>();
}
