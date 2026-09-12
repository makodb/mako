use sto_core::{
    AccessError, RegisteredResource, TerminalReadOpen, TerminalReadReady,
    TerminalReadTransaction,
    TransactionalResource,
};

fn ordinary_access_is_unavailable<A: TransactionalResource>(
    transaction: &mut TerminalReadTransaction<'_, TerminalReadOpen>,
    resource: &RegisteredResource<A>,
    key: A::Key,
) {
    let _ = transaction.with_item(resource, key, |_| Ok::<(), AccessError>(()));
}

fn later_access_is_unavailable<A: TransactionalResource>(
    transaction: &mut TerminalReadTransaction<'_, TerminalReadReady>,
    resource: &RegisteredResource<A>,
    key: A::Key,
) {
    let _ = transaction.with_item(resource, key, |_| Ok::<(), AccessError>(()));
}

fn main() {}
