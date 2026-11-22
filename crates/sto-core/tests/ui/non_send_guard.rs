mod support;

use sto_core::{LockUse, ValidationContext};
use support::{LocalGuard, LocalLock};

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape_guard<'a>(
    context: &'a ValidationContext<'a>,
    lock_use: &'a LockUse<LocalLock>,
) -> &'static LocalGuard {
    context.guard(lock_use).unwrap()
}

fn main() {
    assert_send::<LocalGuard>();
    assert_sync::<LocalGuard>();
}
