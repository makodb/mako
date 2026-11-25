#![cfg(feature = "replication")]
fn database(root: &std::path::Path) -> mako_storage_api::Database {
    assert!(
        !root.exists(),
        "Masstree must not create a RocksDB row mirror"
    );
    mako_storage_masstree::database().unwrap()
}
#[path = "support/replication_contract.rs"]
mod contract;
