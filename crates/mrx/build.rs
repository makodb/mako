//! Availability detection and the one link flag that does not propagate.
//!
//! `cargo:rustc-link-lib` and `-link-search` from a dependency's build
//! script reach the final link on their own, so this crate does not
//! repeat what `mrx-masstree` and `mrx-rocks` already emit. Runtime search
//! arguments do not propagate, so the final crate repeats them for libc++
//! and Mako's vendored yaml-cpp.
//!
//! * `cargo:rustc-link-arg` does **not** propagate from a dependency, so
//!   the libc++ rpath has to be emitted here as well or the test binary
//!   builds and then fails to start.
//! * `cfg` flags set by a dependency's build script apply only to that
//!   dependency, so the end-to-end test's gate has to be re-derived.
//! * For the same reason the trailing link group that carries libmako.a's
//!   Rust staticlibs (native sharding, the Raft runtime) is repeated here.

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=ROCKSDB_LIB_DIR");
    println!("cargo:rerun-if-env-changed=LIBCXX_DIR");
    println!("cargo:rustc-check-cfg=cfg(have_mako)");
    println!("cargo:rustc-check-cfg=cfg(have_rocksdb)");

    let mako_build = mako_build_dir();
    if mako_build.is_some() {
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
    if let Some(build) = mako_build {
        let yaml_dir = build.join("third-party/yaml-cpp");
        let yaml_library = yaml_dir.join("libyaml-cpp.so");
        println!("cargo:rerun-if-changed={}", yaml_library.display());
        if yaml_library.is_file() {
            println!("cargo:rustc-link-search=native={}", yaml_dir.display());
            println!("cargo:rustc-link-arg=-Wl,-rpath,{}", yaml_dir.display());
        }
        emit_trailing_native_link_group(&build);
    }
}

/// Repeat the trailing native link group that `mrx-masstree` emits for its own
/// link targets.
///
/// libmako.a calls into two Cargo staticlibs (native sharding and the Raft
/// runtime, the `rust-staticlib` rows of mako-local's archive manifest). Each
/// carries its own Rust std and a `panic = "abort"` runtime, so they must come
/// after this executable's own std: `cargo:rustc-link-arg` places them there,
/// while `rustc-link-lib=static=` would put them first and make every Rust
/// panic abort the process. The group repeats the whole closure because the
/// Raft runtime calls back into libmako.a. `cargo:rustc-link-arg` does not
/// propagate from a dependency's build script, hence the repetition here; the
/// full rationale is in crates/mako-local/build.rs.
///
/// The manifest is read from the copy CMake publishes in the build tree, not
/// from crates/mako-local: this package is also built from an isolated copy
/// of its own directory (the mutation runners), where sibling crates are not
/// at `../`. A build tree without that copy or without one of the archives is
/// skipped here; mrx-masstree's build script owns the closure and reports what
/// is missing.
fn emit_trailing_native_link_group(build: &Path) {
    let manifest = build.join("generated/mako_local_native_link_archives.txt");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let Ok(rows) = std::fs::read_to_string(&manifest) else {
        return;
    };
    let mut archives = Vec::new();
    let mut has_rust_staticlib = false;
    for line in rows.lines().map(str::trim) {
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut fields = line.split_whitespace();
        let (Some(_name), Some(relative)) = (fields.next(), fields.next()) else {
            panic!("malformed row in {}: {line:?}", manifest.display());
        };
        has_rust_staticlib |= fields.next() == Some("rust-staticlib");
        let path = build.join(relative);
        println!("cargo:rerun-if-changed={}", path.display());
        if !path.is_file() {
            return;
        }
        // The linker resolves a path argument against its own working
        // directory, so a relative MAKO_BUILD_DIR must not leak into it.
        archives.push(path.canonicalize().unwrap_or(path));
    }
    if !has_rust_staticlib {
        return;
    }
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    for path in &archives {
        println!("cargo:rustc-link-arg={}", path.display());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    // What the staticlibs need from the platform, repeated after the group
    // because GNU ld drops an --as-needed library nothing before it used.
    for library in ["rocksdb", "pthread", "dl", "m", "rt", "util", "gcc_s"] {
        println!("cargo:rustc-link-arg=-l{library}");
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
        .map(|v| {
            PathBuf::from(&home)
                .join(".linuxbrew/opt")
                .join(v)
                .join("lib")
        })
        .find(|p| p.join("libc++.so.1").exists() || p.join("libc++.so").exists())
}
