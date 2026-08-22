#![cfg(all(have_mako, have_rocksdb))]

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use mako_cache::{Db, Error, Options};

struct Scratch(PathBuf);

impl Scratch {
    fn new(tag: &str) -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let mut path = std::env::temp_dir();
        path.push(format!(
            "mako-cache-{tag}-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::remove_dir_all(&path);
        Self(path)
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn open(scratch: &Scratch) -> Db {
    Db::open(&scratch.0, Options::default()).expect("open Rocks cache")
}

#[test]
fn atomic_transactions_and_latest_overwrite_survive_rocks_reopen() {
    let scratch = Scratch::new("recovery");
    let cache = open(&scratch);

    let mut seed = cache.transaction().expect("begin seed");
    seed.put(b"doomed", b"present").expect("put doomed");
    seed.put(b"latest", b"v1").expect("put first version");
    seed.commit().expect("commit seed");

    let mut transaction = cache.transaction().expect("begin multi-key transaction");
    transaction.put(b"empty", b"").expect("put empty");
    transaction
        .put(b"binary\0key", b"\0\xffbinary\0value")
        .expect("put binary");
    assert!(transaction.remove(b"doomed").expect("delete key"));
    transaction.commit().expect("commit multi-key transaction");

    cache.put(b"latest", b"v2").expect("put second version");
    cache.put(b"latest", b"v3").expect("put latest version");
    cache.flush().expect("flush Rocks transactions");
    cache.close().expect("clean close");

    let reopened = open(&scratch);
    assert_eq!(
        reopened.get(b"empty").expect("recover empty"),
        Some(Vec::new())
    );
    assert_eq!(
        reopened
            .get(b"binary\0key")
            .expect("recover binary")
            .as_deref(),
        Some(&b"\0\xffbinary\0value"[..])
    );
    assert_eq!(reopened.get(b"doomed").expect("recover delete"), None);
    assert_eq!(
        reopened.get(b"latest").expect("recover latest").as_deref(),
        Some(&b"v3"[..])
    );
    reopened.close().expect("close reopened Rocks cache");
}

#[test]
fn required_read_your_writes_follows_the_native_feature_bit() {
    let scratch = Scratch::new("required-ryw");
    let supports_read_your_writes = mako_local::features()
        .expect("read native features")
        .read_my_writes();
    let mut options = Options::default();
    options.cache.require_read_my_writes = true;

    match (supports_read_your_writes, Db::open(&scratch.0, options)) {
        (true, Ok(cache)) => {
            cache.close().expect("close supported profile");
        }
        (false, Err(Error::MissingReadMyWrites)) => {}
        (true, Err(error)) => panic!("advertised read-your-writes was rejected: {error:?}"),
        (false, Err(error)) => panic!("expected missing read-your-writes, got {error:?}"),
        (false, Ok(cache)) => {
            cache.close().expect("close unexpectedly opened cache");
            panic!("cache accepted a native profile without required read-your-writes");
        }
    }
}
