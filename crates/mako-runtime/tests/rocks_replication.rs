#![cfg(all(feature = "logging", feature = "rocksdb-engine"))]
fn database(root: &std::path::Path, owner: u64) -> mako_storage_api::Database {
    // Combined builds exercise a mixed-engine transaction; the isolated profile
    // runs the identical scenario with a direct RocksDB coordinator.
    #[cfg(feature = "masstree-engine")]
    if owner == 1 {
        return mako_storage_masstree::database().unwrap();
    }
    mako_storage_rocksdb::database(root, owner, true).unwrap()
}
#[path = "support/replication_contract.rs"]
mod contract;
