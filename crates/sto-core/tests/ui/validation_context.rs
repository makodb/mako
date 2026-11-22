use sto_core::ValidationContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: ValidationContext<'_>) -> ValidationContext<'static> {
    context
}

fn main() {
    assert_send::<ValidationContext<'static>>();
    assert_sync::<ValidationContext<'static>>();
}
