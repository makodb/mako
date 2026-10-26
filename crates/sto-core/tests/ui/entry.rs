mod support;

use sto_core::TransProxy;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(entry: TransProxy<'_, support::Adapter>) -> TransProxy<'static, support::Adapter> {
    entry
}

fn main() {
    assert_send::<TransProxy<'static, support::Adapter>>();
    assert_sync::<TransProxy<'static, support::Adapter>>();
}
