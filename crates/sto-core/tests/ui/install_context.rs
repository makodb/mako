use sto_core::InstallContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(context: InstallContext<'_>) -> InstallContext<'static> {
    context
}

fn main() {
    assert_send::<InstallContext<'static>>();
    assert_sync::<InstallContext<'static>>();
}
