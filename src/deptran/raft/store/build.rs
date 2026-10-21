// The RocksDB base links librocksdb (feature `rocksdb`). The search path
// comes from LIBRARY_PATH, as for the Mako build.
fn main() {
    if std::env::var_os("CARGO_FEATURE_ROCKSDB").is_some() {
        println!("cargo:rustc-link-lib=rocksdb");
        if let Ok(paths) = std::env::var("LIBRARY_PATH") {
            for p in paths.split(':').filter(|p| !p.is_empty()) {
                println!("cargo:rustc-link-search=native={p}");
            }
        }
    }
    println!("cargo:rerun-if-env-changed=LIBRARY_PATH");
}
