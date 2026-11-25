#![cfg(all(feature = "replication", feature = "rocksdb-engine"))]
fn database(root: &std::path::Path) -> mako_storage_api::Database {
    mako_storage_rocksdb::database(root, 0, true).unwrap()
}
#[path = "support/replication_contract.rs"]
mod contract;
