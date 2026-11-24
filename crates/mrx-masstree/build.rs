//! Link the native Masstree runtime without a transaction engine.
use std::path::{Path, PathBuf};
fn main() {
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=LIBCXX_DIR");
    println!("cargo:rustc-check-cfg=cfg(have_mako)");
    let Some(build) = find_build_dir() else {
        assert!(
            std::env::var_os("MAKO_BUILD_DIR").is_none(),
            "MAKO_BUILD_DIR must contain libmako_mtree_boundary.a"
        );
        println!("cargo:warning=Masstree integration requires MAKO_BUILD_DIR");
        return;
    };
    for (name, relative) in [
        ("mako_mtree_boundary", "libmako_mtree_boundary.a"),
        ("masstree", "src/masstree/libmasstree.a"),
    ] {
        let archive = build.join(relative);
        assert!(
            archive.is_file(),
            "missing native archive {}",
            archive.display()
        );
        println!("cargo:rerun-if-changed={}", archive.display());
        println!(
            "cargo:rustc-link-search=native={}",
            archive.parent().unwrap().display()
        );
        println!("cargo:rustc-link-lib=static={name}");
    }
    if let Some(directory) = libcxx_dir() {
        println!("cargo:rustc-link-search=native={}", directory.display());
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
    }
    for library in ["c++", "c++abi", "lz4", "numa", "pthread", "dl"] {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
    println!("cargo:rustc-cfg=have_mako");
}

fn find_build_dir() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("MAKO_BUILD_DIR") {
        let p = PathBuf::from(d);
        return p.join("libmako_mtree_boundary.a").exists().then_some(p);
    }
    // crates/mrx-masstree -> repository root
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent()?.parent()?;
    ["build_c22", "build", "build_docker"]
        .iter()
        .map(|d| root.join(d))
        .find(|p| p.join("libmako_mtree_boundary.a").exists())
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
