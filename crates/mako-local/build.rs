//! Locate the existing CMake build that owns the C++ implementation.
//!
//! We deliberately do not compile STO or MassTrans from Cargo: doing so with
//! different generated config or compile definitions creates incompatible
//! template/layout instantiations in one process.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const DEFAULT_BUILD_DIRS: [&str; 4] = ["build_mrx", "build_c22", "build", "build_docker"];

fn main() {
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=MAKO_LOCAL_REQUIRE_NATIVE");
    println!("cargo:rerun-if-env-changed=MAKO_LOCAL_FAKE_ABI");
    println!("cargo:rerun-if-env-changed=LIBCXX_DIR");
    println!("cargo:rerun-if-env-changed=PYTHON");
    println!("cargo:rustc-check-cfg=cfg(have_mako)");

    if fake_abi_requested() {
        assert!(
            !native_is_required(),
            "MAKO_LOCAL_FAKE_ABI=1 and MAKO_LOCAL_REQUIRE_NATIVE=1 are mutually exclusive"
        );
        return;
    }

    watch_default_build_candidates();
    let Some(build) = find_build_dir() else {
        if native_is_required() || std::env::var_os("MAKO_BUILD_DIR").is_some() {
            panic!(
                "mako-local native tests require a CMake build containing libmako.a; \
                 set MAKO_BUILD_DIR to a valid build tree"
            );
        }
        println!(
            "cargo:warning=no mako build directory found; mako-local compiles, \
             but native integration tests are skipped. Set MAKO_BUILD_DIR."
        );
        return;
    };

    verify_native_fingerprint(&build);
    warn_about_newer_inputs(&build);

    println!(
        "cargo:rerun-if-changed={}",
        build.join("libmako.a").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        build.join("CMakeCache.txt").display()
    );
    let archives: [(&str, &str); 4] = [
        ("mako", "libmako.a"),
        ("cluster", "libcluster.a"),
        ("masstree", "src/masstree/libmasstree.a"),
        ("rrr", "src/rrr/librrr.a"),
    ];
    for (lib, rel) in archives {
        let path = build.join(rel);
        if let (Some(dir), true) = (path.parent(), path.exists()) {
            println!("cargo:rustc-link-search=native={}", dir.display());
            println!("cargo:rustc-link-lib=static={lib}");
        }
    }

    // Mako's in-tree yaml-cpp is built against the same libc++ as libmako.
    // The distro library uses libstdc++ and therefore has different mangled
    // std::string symbols. Put the CMake copy first in the search path.
    let yaml_dir = build.join("third-party/yaml-cpp");
    if yaml_dir.join("libyaml-cpp.so").exists() {
        println!("cargo:rustc-link-search=native={}", yaml_dir.display());
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", yaml_dir.display());
    }
    for lib in [
        "rocksdb",
        "yaml-cpp",
        "lz4",
        "numa",
        "event",
        "event_pthreads",
        "pthread",
        "dl",
    ] {
        println!("cargo:rustc-link-lib=dylib={lib}");
    }

    let libcxx = libcxx_dir(&build).unwrap_or_else(|| {
        panic!(
            "could not derive libmako's libc++ directory from {}; \
             set LIBCXX_DIR to the exact toolchain library directory",
            build.join("CMakeCache.txt").display()
        )
    });
    println!("cargo:rustc-link-search=native={}", libcxx.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", libcxx.display());
    println!("cargo:rustc-link-lib=dylib=c++");
    println!("cargo:rustc-link-lib=dylib=c++abi");
    println!("cargo:rustc-cfg=have_mako");
}

fn find_build_dir() -> Option<PathBuf> {
    if let Some(dir) = std::env::var_os("MAKO_BUILD_DIR") {
        let path = PathBuf::from(dir);
        let path = if path.is_absolute() {
            path
        } else {
            repository_root().join(path)
        };
        return canonical_existing_build_dir(path);
    }
    DEFAULT_BUILD_DIRS
        .iter()
        .map(|dir| repository_root().join(dir))
        .find_map(canonical_existing_build_dir)
}

fn watch_default_build_candidates() {
    for directory in DEFAULT_BUILD_DIRS {
        println!(
            "cargo:rerun-if-changed={}",
            repository_root()
                .join(directory)
                .join("libmako.a")
                .display()
        );
    }
}

fn canonical_existing_build_dir(path: PathBuf) -> Option<PathBuf> {
    path.join("libmako.a").exists().then(|| {
        path.canonicalize()
            .unwrap_or_else(|error| panic!("cannot canonicalize {}: {error}", path.display()))
    })
}

fn native_is_required() -> bool {
    std::env::var("MAKO_LOCAL_REQUIRE_NATIVE").is_ok_and(|value| value == "1")
}

fn fake_abi_requested() -> bool {
    std::env::var("MAKO_LOCAL_FAKE_ABI").is_ok_and(|value| value == "1")
}

fn repository_root() -> &'static Path {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("mako-local must remain inside the repository's crates directory")
}

fn verify_native_fingerprint(build: &Path) {
    let root = repository_root();
    let script = root.join("scripts/mako_local_fingerprint.py");
    let manifest = build.join("generated/mako_local_build_manifest.json");
    let compile_commands = build.join("compile_commands.json");
    let cache = build.join("CMakeCache.txt");
    for dependency in [&script, &manifest, &compile_commands, &cache] {
        println!("cargo:rerun-if-changed={}", dependency.display());
    }

    let out_dir = PathBuf::from(
        std::env::var_os("OUT_DIR").expect("Cargo did not set OUT_DIR for mako-local"),
    );
    // This path is intentionally never created. Cargo treats the missing
    // rerun dependency as dirty, so every required-native Cargo invocation
    // recomputes the content identity instead of trusting source mtimes.
    // Fake/compile-only builds return before reaching this gate.
    println!(
        "cargo:rerun-if-changed={}",
        out_dir
            .join("mako_local_fingerprint_verify_always")
            .display()
    );
    let rust_out = out_dir.join("mako_local_build_identity.rs");
    let dependency_list = out_dir.join("mako_local_fingerprint_dependencies.txt");
    let python = std::env::var_os("PYTHON").unwrap_or_else(|| "python3".into());
    let output = Command::new(&python)
        .arg(&script)
        .arg("verify")
        .arg("--source-root")
        .arg(root)
        .arg("--build-dir")
        .arg(build)
        .arg("--manifest")
        .arg(&manifest)
        .arg("--rust-out")
        .arg(&rust_out)
        .arg("--dependency-list")
        .arg(&dependency_list)
        .current_dir(root)
        .output()
        .unwrap_or_else(|error| {
            panic!(
                "could not run {} to verify libmako.a: {error}",
                PathBuf::from(&python).display()
            )
        });
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        panic!(
            "mako-local refused native archive {} because its content/configuration identity \n\
             could not be verified. Rebuild with `cmake --build {} --target mako`.\n\
             verifier stdout:\n{}\nverifier stderr:\n{}",
            build.join("libmako.a").display(),
            build.display(),
            stdout.trim(),
            stderr.trim()
        );
    }

    let dependencies = fs::read_to_string(&dependency_list).unwrap_or_else(|error| {
        panic!(
            "fingerprint verifier did not leave a readable dependency list {}: {error}",
            dependency_list.display()
        )
    });
    for dependency in dependencies.lines().filter(|line| !line.is_empty()) {
        println!("cargo:rerun-if-changed={dependency}");
    }
}

fn warn_about_newer_inputs(build: &Path) {
    let archive = build.join("libmako.a");
    let archive_time = std::fs::metadata(&archive)
        .and_then(|metadata| metadata.modified())
        .unwrap_or_else(|error| panic!("cannot inspect {}: {error}", archive.display()));
    let root = repository_root();
    let inputs = [
        "src/mako/storage/mako_local_abi.h",
        "src/mako/storage/mako_local_abi.cc",
        "src/mako/sto/thread_registration.hh",
        "src/mako/sto/thread_registration.cc",
        "src/mako/sto/MassTrans.hh",
        "src/mako/sto/Transaction.hh",
        "src/mako/sto/Transaction.cc",
    ];
    for input in inputs {
        let path = root.join(input);
        println!("cargo:rerun-if-changed={}", path.display());
        let Ok(source_time) = std::fs::metadata(&path).and_then(|metadata| metadata.modified())
        else {
            continue;
        };
        if source_time > archive_time {
            println!(
                "cargo:warning={} is newer than {}, but their verified content fingerprint agrees; \
                 modification times are advisory only",
                path.display(),
                archive.display()
            );
        }
    }
}

fn libcxx_dir(build: &Path) -> Option<PathBuf> {
    if let Ok(dir) = std::env::var("LIBCXX_DIR") {
        let path = PathBuf::from(dir);
        return has_libcxx(&path).then_some(path);
    }

    let cache = std::fs::read_to_string(build.join("CMakeCache.txt")).ok()?;

    // Prefer the exact -L directory CMake placed in its executable linker
    // flags. This remains correct when the compiler path is a wrapper.
    if let Some(flags) = cmake_value(&cache, "CMAKE_EXE_LINKER_FLAGS") {
        if let Some(path) = flags
            .split_whitespace()
            .filter_map(|flag| flag.strip_prefix("-L"))
            .map(PathBuf::from)
            .find(|path| has_libcxx(path))
        {
            return Some(path);
        }
    }

    let compiler = PathBuf::from(cmake_value(&cache, "CMAKE_CXX_COMPILER")?);
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
    path.join("libc++.so.1").exists() || path.join("libc++.so").exists()
}
