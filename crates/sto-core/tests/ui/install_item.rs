mod support;

use sto_core::InstallItem;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(item: InstallItem<'_, support::Adapter>) -> InstallItem<'static, support::Adapter> {
    item
}

fn main() {
    assert_send::<InstallItem<'static, support::Adapter>>();
    assert_sync::<InstallItem<'static, support::Adapter>>();
}
