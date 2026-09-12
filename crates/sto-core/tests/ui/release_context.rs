use sto_core::ReleaseContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: ReleaseContext<'_>) -> ReleaseContext<'static> {
    context
}

fn main() {
    assert_send::<ReleaseContext<'static>>();
    assert_sync::<ReleaseContext<'static>>();
}
