#![cfg(all(feature = "replication", feature = "rocksdb-engine"))]
fn database(root: &std::path::Path, owner: u64) -> mako_storage_api::Database {
    if root.file_name().unwrap().to_string_lossy().ends_with("-2") {
        mako_storage_rocksdb::database(root, owner, true).unwrap()
    } else {
        mako_storage_masstree::database().unwrap()
    }
}
#[path = "support/replication_contract.rs"]
mod contract;
