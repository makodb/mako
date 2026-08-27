//! Link against the C++ side when a mako build is available.
//!
//! The `mtx_*` implementation is compiled into `libmako.a` by CMake, not
//! here. A build script that tried to compile masstree itself would be a
//! second, divergent source of truth for a 30-minute C++ build — so this
//! only *locates* an existing archive.
//!
//! When no build directory is present the crate still compiles; the
//! symbols simply go unresolved until something links them, and the tests
//! that need a real tree are gated behind `cfg(have_mako)`. That is the
//! difference between "this machine has not built the C++ yet" (fine) and
//! "the tests silently passed without exercising masstree" (not fine).
//!
//! Point `MAKO_BUILD_DIR` at a build tree to override the search.

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rustc-check-cfg=cfg(have_mako)");

    let Some(build) = find_build_dir() else {
        println!(
            "cargo:warning=no mako build directory found; \
             mrx-masstree will compile but its masstree tests are skipped. \
             Set MAKO_BUILD_DIR to enable them."
        );
        return;
    };

    println!("cargo:rerun-if-changed={}", build.join("libmako.a").display());

    // Whole-archive is deliberately NOT used: `libmako.a` is the entire
    // mako static library and only the members mtx_* actually reaches
    // should be pulled in.
    //
    // ORDER MATTERS, and it is the usual static-archive rule rather than
    // anything specific to this project: an archive only resolves symbols
    // that are already undefined when the linker reaches it. mtree_abi.cc
    // (in libmako) pulls in masstree and srpc, so those must come after.
    let archives: [(&str, &str); 4] = [
        ("mako", "libmako.a"),
        ("cluster", "libcluster.a"),
        ("masstree", "src/masstree/libmasstree.a"),
        ("srpc", "src/srpc/libsrpc.a"),
    ];
    for (lib, rel) in archives {
        let path = build.join(rel);
        if let (Some(dir), true) = (path.parent(), path.exists()) {
            println!("cargo:rustc-link-search=native={}", dir.display());
            println!("cargo:rustc-link-lib=static={lib}");
        }
    }
    for lib in ["rocksdb", "yaml-cpp", "numa", "event", "event_pthreads", "pthread", "dl"] {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }

    // The C++ is built against a specific libc++, and the system one in
    // /usr/lib shadows it if the keg is not searched first. That
    // shadowing is what produced a long run of "__hash_memory undefined"
    // link failures on this machine, so the order matters.
    if let Some(keg) = libcxx_dir() {
        println!("cargo:rustc-link-search=native={}", keg.display());
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", keg.display());
    }
    println!("cargo:rustc-link-lib=dylib=c++");
    println!("cargo:rustc-link-lib=dylib=c++abi");

    println!("cargo:rustc-cfg=have_mako");
}

fn find_build_dir() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("MAKO_BUILD_DIR") {
        let p = PathBuf::from(d);
        return p.join("libmako.a").exists().then_some(p);
    }
    // crates/mrx-masstree -> repository root
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent()?.parent()?;
    ["build_c22", "build", "build_docker"]
        .iter()
        .map(|d| root.join(d))
        .find(|p| p.join("libmako.a").exists())
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
