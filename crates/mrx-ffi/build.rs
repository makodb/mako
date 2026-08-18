//! Availability detection and the one link flag that does not propagate.
//!
//! `cargo:rustc-link-lib` and `-link-search` from a dependency's build
//! script reach the final link on their own, so this crate does not
//! repeat what `mrx-masstree` and `mrx-rocks` already emit. Two things do
//! need repeating:
//!
//! * `cargo:rustc-link-arg` does **not** propagate from a dependency, so
//!   the libc++ rpath has to be emitted here as well or the test binary
//!   builds and then fails to start.
//! * `cfg` flags set by a dependency's build script apply only to that
//!   dependency, so the end-to-end test's gate has to be re-derived.

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=ROCKSDB_LIB_DIR");
    println!("cargo:rustc-check-cfg=cfg(have_mako)");
    println!("cargo:rustc-check-cfg=cfg(have_rocksdb)");

    if mako_build_dir().is_some() {
        println!("cargo:rustc-cfg=have_mako");
    }
    if rocksdb_dir().is_some() {
        println!("cargo:rustc-cfg=have_rocksdb");
    }

    // The C++ is built against a specific libc++; the system one in
    // /usr/lib shadows it unless the keg comes first. Getting this wrong
    // produced a long run of "__hash_memory undefined" failures on this
    // machine.
    if let Some(keg) = libcxx_dir() {
        println!("cargo:rustc-link-search=native={}", keg.display());
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", keg.display());
    }
}

fn mako_build_dir() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("MAKO_BUILD_DIR") {
        let p = PathBuf::from(d);
        return p.join("libmako.a").exists().then_some(p);
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent()?.parent()?;
    ["build_c22", "build", "build_docker"]
        .iter()
        .map(|d| root.join(d))
        .find(|p| p.join("libmako.a").exists())
}

fn rocksdb_dir() -> Option<PathBuf> {
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

fn libcxx_dir() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("LIBCXX_DIR") {
        return Some(PathBuf::from(d));
    }
    let home = std::env::var("HOME").ok()?;
    ["llvm@22", "llvm@21", "llvm"]
        .iter()
        .map(|v| PathBuf::from(&home).join(".linuxbrew/opt").join(v).join("lib"))
        .find(|p| p.join("libc++.so.1").exists() || p.join("libc++.so").exists())
}
