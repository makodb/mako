//! Re-derive native availability for this crate's integration tests.
//!
//! A dependency's `cfg` and `rustc-link-arg` output is package-local.  The
//! final `mako-cache` test executables therefore need their own availability
//! flags and runtime paths even though `mako-local` and `mrx-rocks` own the
//! actual native link declarations.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    for variable in [
        "MAKO_BUILD_DIR",
        "MAKO_LOCAL_REQUIRE_NATIVE",
        "ROCKSDB_LIB_DIR",
        "LIBCXX_DIR",
    ] {
        println!("cargo:rerun-if-env-changed={variable}");
    }
    println!("cargo:rustc-check-cfg=cfg(have_mako)");
    println!("cargo:rustc-check-cfg=cfg(have_rocksdb)");

    match mako_build_dir() {
        Some(build) => configure_mako(&build),
        None if native_is_required() || std::env::var_os("MAKO_BUILD_DIR").is_some() => {
            panic!(
                "mako-cache native tests require a CMake build containing libmako.a; \
                 set MAKO_BUILD_DIR to a valid build tree"
            );
        }
        None => println!(
            "cargo:warning=no mako build directory found; mako-cache compiles, \
             but native integration tests are skipped. Set MAKO_BUILD_DIR."
        ),
    }

    if let Some(rocksdb) = rocksdb_dir() {
        println!("cargo:rustc-link-search=native={}", rocksdb.display());
        println!("cargo:rustc-cfg=have_rocksdb");
    } else {
        println!(
            "cargo:warning=librocksdb not found; mako-cache compiles, but \
             RocksDB integration tests are skipped. Set ROCKSDB_LIB_DIR."
        );
    }
}

fn configure_mako(build: &Path) {
    let cache_path = build.join("CMakeCache.txt");
    println!(
        "cargo:rerun-if-changed={}",
        build.join("libmako.a").display()
    );
    println!("cargo:rerun-if-changed={}", cache_path.display());

    let cache = std::fs::read_to_string(&cache_path).unwrap_or_else(|error| {
        panic!(
            "cannot read Mako's CMake cache at {}: {error}",
            cache_path.display()
        )
    });

    // libmako's std::string symbols must resolve against the yaml-cpp built by
    // this exact CMake tree, not a distro copy built against libstdc++.
    let yaml = cmake_value(&cache, "YAML_CPP_BINARY_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| build.join("third-party/yaml-cpp"));
    if yaml.join("libyaml-cpp.so").exists() {
        println!("cargo:rustc-link-search=native={}", yaml.display());
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", yaml.display());
    } else {
        panic!(
            "the CMake yaml-cpp shared library is missing from {}; rebuild \
             the Mako CMake tree before running mako-cache",
            yaml.display()
        );
    }

    let libcxx = libcxx_dir(&cache).unwrap_or_else(|| {
        panic!(
            "could not derive libmako's libc++ directory from {}; set \
             LIBCXX_DIR to the exact toolchain library directory",
            cache_path.display()
        )
    });
    println!("cargo:rustc-link-search=native={}", libcxx.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", libcxx.display());
    println!("cargo:rustc-cfg=have_mako");
}

fn mako_build_dir() -> Option<PathBuf> {
    if let Ok(dir) = std::env::var("MAKO_BUILD_DIR") {
        let path = PathBuf::from(dir);
        return path.join("libmako.a").exists().then_some(path);
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent()?.parent()?;
    ["build_mrx", "build_c22", "build", "build_docker"]
        .iter()
        .map(|dir| root.join(dir))
        .find(|path| path.join("libmako.a").exists())
}

fn native_is_required() -> bool {
    std::env::var("MAKO_LOCAL_REQUIRE_NATIVE").is_ok_and(|value| value == "1")
}

fn rocksdb_dir() -> Option<PathBuf> {
    if let Ok(dir) = std::env::var("ROCKSDB_LIB_DIR") {
        let path = PathBuf::from(dir);
        return has_rocksdb(&path).then_some(path);
    }
    [
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib64",
        "/usr/lib",
        "/usr/local/lib",
    ]
    .iter()
    .map(Path::new)
    .find(|path| has_rocksdb(path))
    .map(PathBuf::from)
}

fn has_rocksdb(path: &Path) -> bool {
    path.join("librocksdb.so").exists() || path.join("librocksdb.a").exists()
}

fn libcxx_dir(cache: &str) -> Option<PathBuf> {
    if let Ok(dir) = std::env::var("LIBCXX_DIR") {
        let path = PathBuf::from(dir);
        return has_libcxx(&path).then_some(path);
    }

    // Prefer the exact -L directory CMake selected. This also works when the
    // configured compiler is a wrapper rather than the toolchain driver.
    if let Some(flags) = cmake_value(cache, "CMAKE_EXE_LINKER_FLAGS") {
        if let Some(path) = flags
            .split_whitespace()
            .filter_map(|flag| flag.strip_prefix("-L"))
            .map(PathBuf::from)
            .find(|path| has_libcxx(path))
        {
            return Some(path);
        }
    }

    let compiler = PathBuf::from(cmake_value(cache, "CMAKE_CXX_COMPILER")?);
    compiler_libcxx_dir(&compiler)
}

fn compiler_libcxx_dir(compiler: &Path) -> Option<PathBuf> {
    for library in ["libc++.so.1", "libc++.so"] {
        let output = Command::new(compiler)
            .arg(format!("--print-file-name={library}"))
            .output()
            .ok()?;
        if !output.status.success() {
            continue;
        }
        let reported = String::from_utf8(output.stdout).ok()?;
        let path = PathBuf::from(reported.trim());
        if path == Path::new(library) || !path.is_file() {
            continue;
        }
        let path = path.canonicalize().ok()?;
        return path.parent().map(Path::to_path_buf);
    }
    None
}

fn cmake_value<'a>(cache: &'a str, key: &str) -> Option<&'a str> {
    cache.lines().find_map(|line| {
        let (entry, value) = line.split_once('=')?;
        let (name, _) = entry.split_once(':')?;
        (name == key).then_some(value)
    })
}

fn has_libcxx(path: &Path) -> bool {
    path.join("libc++.so.1").exists() || path.join("libc++.so").exists()
}
