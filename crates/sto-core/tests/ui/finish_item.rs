mod support;

use sto_core::CleanupItem;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(item: CleanupItem<'_, support::Adapter>) -> CleanupItem<'static, support::Adapter> {
    item
}

fn main() {
    assert_send::<CleanupItem<'static, support::Adapter>>();
    assert_sync::<CleanupItem<'static, support::Adapter>>();
}
