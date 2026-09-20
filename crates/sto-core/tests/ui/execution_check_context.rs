use sto_core::ExecutionCheckContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: ExecutionCheckContext<'_>) -> ExecutionCheckContext<'static> {
    context
}

fn main() {
    assert_send::<ExecutionCheckContext<'static>>();
    assert_sync::<ExecutionCheckContext<'static>>();
}
