use mako_rocksdb::{IteratorMode, OptimisticTransactionDB, Options, WriteBatch};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args()
        .nth(1)
        .ok_or("usage: rocksdb_transactions DIRECTORY [--masstree]")?;
    let mut options = Options::default();
    options.create_if_missing(true);
    options.set_masstree(std::env::args().any(|a| a == "--masstree"));
    let db = OptimisticTransactionDB::open_cf(&options, path, ["default", "accounts"])?;
    let accounts = db.cf_handle("accounts").expect("configured family");
    let tx = db.transaction();
    tx.put_cf(accounts, b"alice", b"100")?;
    tx.put_cf(accounts, b"bob", b"200")?;
    tx.put(b"last-operation", b"initialize-accounts")?;
    tx.commit()?;

    let mut batch = WriteBatch::default();
    batch.put(b"batch-key", b"value");
    batch.delete(b"unused-key");
    db.write(batch)?;
    for row in db.iterator_cf(accounts, IteratorMode::Start) {
        let (key, value) = row?;
        println!(
            "{} = {}",
            String::from_utf8_lossy(&key),
            String::from_utf8_lossy(&value)
        );
    }
    Ok(())
}
