//! Link the native Masstree runtime without a transaction engine.
use std::path::{Path, PathBuf};
#[allow(dead_code)]
#[path = "../native-build-support/native_allocator.rs"]
mod native_allocator;
fn main() {
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=LIBCXX_DIR");
    println!("cargo:rustc-check-cfg=cfg(have_mako)");
    let Some(build) = find_build_dir() else {
        assert!(
            std::env::var_os("MAKO_BUILD_DIR").is_none(),
            "MAKO_BUILD_DIR must contain libmako_native_storage.a"
        );
        println!("cargo:warning=Masstree integration requires MAKO_BUILD_DIR");
        return;
    };
    let archive = build.join("libmako_native_storage.a");
    println!("cargo:rerun-if-changed={}", archive.display());
    println!("cargo:rustc-link-search=native={}", build.display());
    println!("cargo:rustc-link-lib=static=mako_native_storage");
    let cache_path = build.join("CMakeCache.txt");
    let contract_path = build.join(native_allocator::CONTRACT_RELATIVE_PATH);
    println!("cargo:rerun-if-changed={}", cache_path.display());
    println!("cargo:rerun-if-changed={}", contract_path.display());
    let cache = std::fs::read_to_string(&cache_path).expect("native storage CMake cache");
    let configured = native_allocator::NativeAllocator::from_cmake_cache(&cache)
        .unwrap_or_else(|error| panic!("native allocator: {error}"));
    let contract = std::fs::read_to_string(contract_path).expect("native allocator contract");
    let allocator = native_allocator::ResolvedAllocator::from_contract(&contract)
        .and_then(|allocator| allocator.validate_mode(configured))
        .unwrap_or_else(|error| panic!("native allocator contract: {error}"));
    allocator
        .validate_library_path()
        .expect("native allocator library");
    if let Some(directory) = allocator.library_directory() {
        println!("cargo:rustc-link-search=native={}", directory.display());
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
    }
    if let Some(library) = configured.link_library() {
        println!("cargo:rustc-link-lib=dylib={library}");
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
        return p.join("libmako_native_storage.a").exists().then_some(p);
    }
    // crates/mrx-masstree -> repository root
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent()?.parent()?;
    ["build", "build_native", "build_docker"]
        .iter()
        .map(|d| root.join(d))
        .find(|p| p.join("libmako_native_storage.a").exists())
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
