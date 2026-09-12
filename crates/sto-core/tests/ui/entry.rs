mod support;

use sto_core::Entry;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(entry: Entry<'_, support::Adapter>) -> Entry<'static, support::Adapter> {
    entry
}

fn main() {
    assert_send::<Entry<'static, support::Adapter>>();
    assert_sync::<Entry<'static, support::Adapter>>();
}
