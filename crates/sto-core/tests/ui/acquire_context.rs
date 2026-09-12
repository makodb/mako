use sto_core::AcquireContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: AcquireContext<'_>) -> AcquireContext<'static> {
    context
}

fn main() {
    assert_send::<AcquireContext<'static>>();
    assert_sync::<AcquireContext<'static>>();
}
