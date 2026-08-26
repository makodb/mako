//! Give the final benchmark executable the runtime search paths of the exact
//! native Mako build it measures.
//!
//! Dependency build-script link arguments are package-local.  The benchmark
//! is a separate package, so it must repeat the two rpaths used by
//! `mako-cache`: Mako's in-tree yaml-cpp and the configured libc++.

use std::path::{Path, PathBuf};

fn main() {
    for variable in ["MAKO_BUILD_DIR", "LIBCXX_DIR"] {
        println!("cargo:rerun-if-env-changed={variable}");
    }

    let build = std::env::var_os("MAKO_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| panic!("mako-cache-bench requires MAKO_BUILD_DIR"));
    if !build.join("libmako.a").is_file() {
        panic!(
            "MAKO_BUILD_DIR={} does not contain libmako.a",
            build.display()
        );
    }

    let cache_path = build.join("CMakeCache.txt");
    let cache = std::fs::read_to_string(&cache_path)
        .unwrap_or_else(|error| panic!("cannot read {}: {error}", cache_path.display()));

    let yaml = cmake_value(&cache, "YAML_CPP_BINARY_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| build.join("third-party/yaml-cpp"));
    if !yaml.join("libyaml-cpp.so").is_file() {
        panic!(
            "native benchmark requires Mako's yaml-cpp at {}",
            yaml.display()
        );
    }
    println!("cargo:rustc-link-search=native={}", yaml.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", yaml.display());

    let libcxx = libcxx_dir(&cache).unwrap_or_else(|| {
        panic!(
            "cannot derive libc++ from {}; set LIBCXX_DIR",
            cache_path.display()
        )
    });
    println!("cargo:rustc-link-search=native={}", libcxx.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", libcxx.display());

    // mrx-masstree's native archive contains the snapshot implementation,
    // whose object file references LZ4 even though this benchmark never calls
    // that path. Declare the final binary's dependency directly instead of
    // relying on mako-local's production-only link metadata to supply it.
    println!("cargo:rustc-link-lib=dylib=lz4");
}

fn libcxx_dir(cache: &str) -> Option<PathBuf> {
    if let Some(value) = std::env::var_os("LIBCXX_DIR") {
        let path = PathBuf::from(value);
        return has_libcxx(&path).then_some(path);
    }
    if let Some(flags) = cmake_value(cache, "CMAKE_EXE_LINKER_FLAGS") {
        if let Some(path) = flags
            .split_ascii_whitespace()
            .filter_map(|flag| flag.strip_prefix("-L"))
            .map(PathBuf::from)
            .find(|path| has_libcxx(path))
        {
            return Some(path);
        }
    }
    let compiler = PathBuf::from(cmake_value(cache, "CMAKE_CXX_COMPILER")?);
    let path = compiler.parent()?.parent()?.join("lib");
    has_libcxx(&path).then_some(path)
}

fn cmake_value<'a>(cache: &'a str, key: &str) -> Option<&'a str> {
    cache.lines().find_map(|line| {
        let (entry, value) = line.split_once('=')?;
        let (name, _) = entry.split_once(':')?;
        (name == key).then_some(value)
    })
}

fn has_libcxx(path: &Path) -> bool {
    path.join("libc++.so.1").is_file() || path.join("libc++.so").is_file()
}
