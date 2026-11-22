use sto_core::CleanupContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: CleanupContext<'_>) -> CleanupContext<'static> {
    context
}

fn main() {
    assert_send::<CleanupContext<'static>>();
    assert_sync::<CleanupContext<'static>>();
}
