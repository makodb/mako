use sto_core::LockContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: LockContext<'_>) -> LockContext<'static> {
    context
}

fn main() {
    assert_send::<LockContext<'static>>();
    assert_sync::<LockContext<'static>>();
}
