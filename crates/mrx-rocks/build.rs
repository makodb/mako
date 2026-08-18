//! Locate RocksDB.
//!
//! Deliberately links the *system* RocksDB rather than vendoring one.
//! Vendoring puts a second copy of the library, with a second set of
//! global state, into a process that already links mako's — a class of
//! bug not worth inviting for the convenience of a `cargo build` that
//! works everywhere.
//!
//! `ROCKSDB_LIB_DIR` overrides the search; `ROCKSDB_STATIC=1` links
//! statically.

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=ROCKSDB_LIB_DIR");
    println!("cargo:rerun-if-env-changed=ROCKSDB_STATIC");
    println!("cargo:rustc-check-cfg=cfg(have_rocksdb)");

    let Some(dir) = find() else {
        println!(
            "cargo:warning=librocksdb not found; mrx-rocks will compile but \
             its tests are skipped. Set ROCKSDB_LIB_DIR to enable them."
        );
        return;
    };
    println!("cargo:rustc-link-search=native={}", dir.display());
    if std::env::var("ROCKSDB_STATIC").is_ok_and(|v| v == "1") {
        println!("cargo:rustc-link-lib=static=rocksdb");
        // A static RocksDB does not carry its own dependencies.
        for lib in ["stdc++", "z", "bz2", "lz4", "snappy", "zstd"] {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
    } else {
        println!("cargo:rustc-link-lib=dylib=rocksdb");
    }
    println!("cargo:rustc-cfg=have_rocksdb");
}

fn find() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("ROCKSDB_LIB_DIR") {
        return Some(PathBuf::from(d));
    }
    [
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib64",
        "/usr/lib",
        "/usr/local/lib",
    ]
    .iter()
    .map(Path::new)
    .find(|d| d.join("librocksdb.so").exists() || d.join("librocksdb.a").exists())
    .map(PathBuf::from)
}
