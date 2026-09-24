use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn run(command: &mut Command) {
    let status = command.status().expect("run native build tool");
    assert!(status.success(), "native build failed: {command:?}");
}

fn main() {
    let target = env::var("CARGO_CFG_TARGET_OS").expect("target OS");
    assert_eq!(target, "linux", "SRPC's native reactor requires Linux");
    let arch = env::var("CARGO_CFG_TARGET_ARCH").expect("target architecture");
    assert!(
        matches!(arch.as_str(), "x86_64" | "aarch64"),
        "unsupported fiber architecture: {arch}"
    );
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo output directory"));
    let cc = env::var_os("CC").unwrap_or_else(|| "cc".into());
    let ar = env::var_os("AR").unwrap_or_else(|| "ar".into());
    // This same reviewed list supplies CMake's production library.
    println!("cargo:rerun-if-changed=scripts/native-kernel-sources.txt");
    let mut sources = Vec::new();
    for line in include_str!("scripts/native-kernel-sources.txt").lines() {
        let mut fields = line.split_whitespace();
        let selector = fields.next().expect("native source architecture");
        let source = fields.next().expect("native source path");
        assert!(fields.next().is_none(), "invalid native source record");
        assert!(
            matches!(selector, "all" | "x86_64" | "aarch64"),
            "invalid native source architecture"
        );
        if selector == "all" || selector == arch {
            sources.push(source);
        }
    }
    let mut objects = Vec::new();
    for source in sources {
        println!("cargo:rerun-if-changed={source}");
        let stem = Path::new(source).file_stem().expect("source stem");
        let object = out.join(stem).with_extension("o");
        run(Command::new(&cc)
            .args([
                "-std=gnu11",
                "-O2",
                "-g",
                "-fPIC",
                "-DREUSE_FIBER",
                "-I.",
                "-c",
            ])
            .arg(source)
            .arg("-o")
            .arg(&object));
        objects.push(object);
    }
    for header in [
        "misc/srpc_timing.h",
        "misc/srpc_rand.h",
        "rpc/srpc_connect.h",
        "rpc/srpc_server.h",
        "reactor/srpc_fiber.h",
        "reactor/srpc_epoll.h",
    ] {
        println!("cargo:rerun-if-changed={header}");
    }
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=AR");
    let archive = out.join("libsrpc_native.a");
    if archive.exists() {
        std::fs::remove_file(&archive).expect("replace native archive");
    }
    run(Command::new(ar).arg("crs").arg(&archive).args(objects));
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=srpc_native");
}
