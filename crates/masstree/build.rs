use std::{env, path::PathBuf};

const ENABLE: &str = "MAKO_MTREE_NATIVE_INTEGRATION";
const LIB_DIRS: &str = "MAKO_MTREE_NATIVE_LIB_DIRS";
const LIBS: &str = "MAKO_MTREE_NATIVE_LIBS";

fn main() {
    println!("cargo:rustc-check-cfg=cfg(mtree_native_integration)");
    for variable in [ENABLE, LIB_DIRS, LIBS] {
        println!("cargo:rerun-if-env-changed={variable}");
    }

    if env::var(ENABLE).ok().as_deref() != Some("1") {
        return;
    }

    println!("cargo:rustc-cfg=mtree_native_integration");
    let directories = env::var_os(LIB_DIRS)
        .unwrap_or_else(|| panic!("{LIB_DIRS} must contain the CMake native library directories"));
    for directory in env::split_paths(&directories) {
        emit_search_path(directory);
    }

    let libraries = env::var(LIBS).unwrap_or_else(|_| {
        panic!(
            "{LIBS} must be a comma-separated rustc link-lib list, for example \
             static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread"
        )
    });
    for library in libraries
        .split(',')
        .map(str::trim)
        .filter(|lib| !lib.is_empty())
    {
        println!("cargo:rustc-link-lib={library}");
    }
}

fn emit_search_path(directory: PathBuf) {
    assert!(
        directory.is_dir(),
        "native link-search directory does not exist: {}",
        directory.display()
    );
    println!("cargo:rustc-link-search=native={}", directory.display());
}
