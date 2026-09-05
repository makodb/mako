use sto_core::{LockUse, PreflightFreeValidationContext, TransactionLock};

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

fn escape(
    context: PreflightFreeValidationContext<'_>,
) -> PreflightFreeValidationContext<'static> {
    context
}

fn cannot_resolve_a_guard<L: TransactionLock>(
    context: &PreflightFreeValidationContext<'_>,
    lock_use: &LockUse<L>,
) {
    let _ = context.guard(lock_use);
}

fn main() {
    assert_send::<PreflightFreeValidationContext<'static>>();
    assert_sync::<PreflightFreeValidationContext<'static>>();
}
