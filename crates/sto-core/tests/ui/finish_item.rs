mod support;

use sto_core::FinishItem;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(item: FinishItem<'_, support::Adapter>) -> FinishItem<'static, support::Adapter> {
    item
}

fn main() {
    assert_send::<FinishItem<'static, support::Adapter>>();
    assert_sync::<FinishItem<'static, support::Adapter>>();
}
