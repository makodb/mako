use sto_core::WorkerContext;

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn main() {
    assert_send::<WorkerContext>();
    assert_sync::<WorkerContext>();
}
