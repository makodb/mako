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
    let directories: Vec<_> = env::split_paths(&directories).collect();
    for directory in &directories {
        emit_search_path(directory.clone());
    }

    let libraries = env::var(LIBS).unwrap_or_else(|_| {
        panic!(
            "{LIBS} must be a comma-separated rustc link-lib list, for example \
             static:-bundle=mako_native_storage,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread"
        )
    });
    for library in libraries
        .split(',')
        .map(str::trim)
        .filter(|lib| !lib.is_empty())
    {
        println!("cargo:rustc-link-lib={library}");
        // CMake builds this archive outside Cargo. A search path or link-lib
        // directive alone does not invalidate executables after C++ changes.
        if let Some((kind, name)) = library.split_once('=') {
            if kind.split(':').next() == Some("static") {
                let archive = directories
                    .iter()
                    .map(|directory| directory.join(format!("lib{name}.a")))
                    .find(|path| path.is_file())
                    .unwrap_or_else(|| panic!("native archive {name} was not built"));
                println!("cargo:rerun-if-changed={}", archive.display());
            }
        }
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
