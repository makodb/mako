use sto_core::{Active, Transaction};

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(transaction: Transaction<'_, Active>) -> Transaction<'static, Active> {
    transaction
}

fn main() {
    assert_send::<Transaction<'static, Active>>();
    assert_sync::<Transaction<'static, Active>>();
}
