mod support;

use sto_core::PreflightItem;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(item: PreflightItem<'_, support::Adapter>) -> PreflightItem<'static, support::Adapter> {
    item
}

fn main() {
    assert_send::<PreflightItem<'static, support::Adapter>>();
    assert_sync::<PreflightItem<'static, support::Adapter>>();
}
