use sto_core::PreflightContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: PreflightContext<'_>) -> PreflightContext<'static> {
    context
}

fn main() {
    assert_send::<PreflightContext<'static>>();
    assert_sync::<PreflightContext<'static>>();
}
