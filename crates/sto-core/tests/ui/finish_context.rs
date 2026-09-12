use sto_core::FinishContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: FinishContext<'_>) -> FinishContext<'static> {
    context
}

fn main() {
    assert_send::<FinishContext<'static>>();
    assert_sync::<FinishContext<'static>>();
}
