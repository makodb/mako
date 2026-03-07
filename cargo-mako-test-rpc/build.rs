use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const DEFAULT_MAKO_BUILD_DIR_REL: &str = "build";
const TARGET_NAME: &str = "test_rpc";
const OUTPUT_NAME: &str = "test_rpc";

struct CompileUnit {
    source_rel: &'static str,
    object_rel: &'static str,
}

static COMPILE_UNITS: &[CompileUnit] = &[
    CompileUnit {
        source_rel: "test/benchmark_service.cc",
        object_rel: "obj/000_CMakeFiles_test_rpc.dir_test_benchmark_service.cc.o",
    },
    CompileUnit {
        source_rel: "test/test_rpc.cc",
        object_rel: "obj/001_CMakeFiles_test_rpc.dir_test_test_rpc.cc.o",
    },
];

static STATIC_LINK_ARGS: &[&str] = &[
    "librrr.a",
    "libmemdb.a",
    "-lpthread",
    "-lpthread",
    "-lnuma",
    "-lrt",
    "-Wl,--whole-archive",
    "-Wl,--as-needed",
    "-L/usr/lib/x86_64-linux-gnu",
    "-lrte_node",
    "-lrte_graph",
    "-lrte_pipeline",
    "-lrte_table",
    "-lrte_pdump",
    "-lrte_port",
    "-lrte_fib",
    "-lrte_pdcp",
    "-lrte_ipsec",
    "-lrte_vhost",
    "-lrte_stack",
    "-lrte_security",
    "-lrte_sched",
    "-lrte_reorder",
    "-lrte_rib",
    "-lrte_mldev",
    "-lrte_regexdev",
    "-lrte_rawdev",
    "-lrte_power",
    "-lrte_pcapng",
    "-lrte_member",
    "-lrte_lpm",
    "-lrte_latencystats",
    "-lrte_jobstats",
    "-lrte_ip_frag",
    "-lrte_gso",
    "-lrte_gro",
    "-lrte_gpudev",
    "-lrte_dispatcher",
    "-lrte_eventdev",
    "-lrte_efd",
    "-lrte_dmadev",
    "-lrte_distributor",
    "-lrte_cryptodev",
    "-lrte_compressdev",
    "-lrte_cfgfile",
    "-lrte_bpf",
    "-lrte_bitratestats",
    "-lrte_bbdev",
    "-lrte_acl",
    "-lrte_timer",
    "-lrte_hash",
    "-lrte_metrics",
    "-lrte_cmdline",
    "-lrte_pci",
    "-lrte_ethdev",
    "-lrte_meter",
    "-lrte_net",
    "-lrte_mbuf",
    "-lrte_mempool",
    "-lrte_rcu",
    "-lrte_ring",
    "-lrte_eal",
    "-lrte_telemetry",
    "-lrte_argparse",
    "-lrte_kvargs",
    "-lrte_log",
    "-lbsd",
    "-Wl,--no-whole-archive",
    "-lpthread",
    "-lnuma",
    "-ldl",
    "-lgflags",
    "-levent_pthreads",
    "-pthread",
    "-L/usr/lib/x86_64-linux-gnu",
    "-levent",
    "-lz",
    "-lrt",
    "-lcrypt",
    "-laio",
    "-ldl",
    "-lssl",
    "-lcrypto",
    "-L/usr/lib/x86_64-linux-gnu",
    "-levent",
    "lib/libgtest_main.so.1.11.0",
    "-lpthread",
    "librrr.a",
    "-lpthread",
    "-lnuma",
    "-lrt",
    "-lrte_node",
    "-lrte_graph",
    "-lrte_pipeline",
    "-lrte_table",
    "-lrte_pdump",
    "-lrte_port",
    "-lrte_fib",
    "-lrte_pdcp",
    "-lrte_ipsec",
    "-lrte_vhost",
    "-lrte_stack",
    "-lrte_security",
    "-lrte_sched",
    "-lrte_reorder",
    "-lrte_rib",
    "-lrte_mldev",
    "-lrte_regexdev",
    "-lrte_rawdev",
    "-lrte_power",
    "-lrte_pcapng",
    "-lrte_member",
    "-lrte_lpm",
    "-lrte_latencystats",
    "-lrte_jobstats",
    "-lrte_ip_frag",
    "-lrte_gso",
    "-lrte_gro",
    "-lrte_gpudev",
    "-lrte_dispatcher",
    "-lrte_eventdev",
    "-lrte_efd",
    "-lrte_dmadev",
    "-lrte_distributor",
    "-lrte_cryptodev",
    "-lrte_compressdev",
    "-lrte_cfgfile",
    "-lrte_bpf",
    "-lrte_bitratestats",
    "-lrte_bbdev",
    "-lrte_acl",
    "-lrte_timer",
    "-lrte_hash",
    "-lrte_metrics",
    "-lrte_cmdline",
    "-lrte_pci",
    "-lrte_ethdev",
    "-lrte_meter",
    "-lrte_net",
    "-lrte_mbuf",
    "-lrte_mempool",
    "-lrte_rcu",
    "-lrte_ring",
    "-lrte_eal",
    "-lrte_telemetry",
    "-lrte_argparse",
    "-lrte_kvargs",
    "-lrte_log",
    "-lbsd",
    "-ldl",
    "-lgflags",
    "-levent_pthreads",
    "-lz",
    "-lcrypt",
    "-laio",
    "-lssl",
    "-lcrypto",
    "-lpthread",
    "lib/libgtest.so.1.11.0",
];

fn run_checked(mut cmd: Command, stage: &str) -> Result<(), String> {
    let output = cmd
        .output()
        .map_err(|e| format!("{stage}: failed to spawn command: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "{stage}: command failed\nstatus: {}\nstdout:\n{}\nstderr:\n{}",
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    Ok(())
}

fn validate_build_dir_artifacts(build_dir: &Path) -> Result<(), String> {
    let required = [
        "librrr.a",
        "libmemdb.a",
        "lib/libgtest_main.so.1.11.0",
        "lib/libgtest.so.1.11.0",
    ];
    let mut missing = Vec::new();
    for rel in required {
        let path = build_dir.join(rel);
        if !path.exists() {
            missing.push(path.display().to_string());
        }
    }
    if missing.is_empty() {
        return Ok(());
    }
    Err(format!(
        "MAKO_BUILD_DIR is missing required C++ build artifacts for target '{}':\n{}\n\
         Build the C++ targets in MAKO_BUILD_DIR first (e.g. CMake/Ninja for rrr/memdb/gtest outputs),\n\
         then rerun cargo build.",
        TARGET_NAME,
        missing.join("\n")
    ))
}

fn expand_wl_forwarded_args(arg: &str) -> Vec<String> {
    if let Some(rest) = arg.strip_prefix("-Wl,") {
        return rest
            .split(',')
            .filter(|part| !part.is_empty())
            .map(|part| part.to_string())
            .collect();
    }
    vec![arg.to_string()]
}

fn normalize_link_args_for_rust_lld<'a>(args: impl IntoIterator<Item = &'a str>) -> Vec<String> {
    let mut normalized: Vec<String> = Vec::new();
    for arg in args {
        if matches!(arg, "-fPIC" | "-DNDEBUG") {
            continue;
        }
        if arg == "-pthread" {
            normalized.push("-lpthread".to_string());
            continue;
        }
        if arg.starts_with("-Wl,") {
            for token in expand_wl_forwarded_args(arg) {
                if token.starts_with("--dependency-file=") {
                    continue;
                }
                normalized.push(token);
            }
            continue;
        }
        normalized.push(arg.to_string());
    }
    normalized
}

fn default_native_search_dirs() -> Vec<String> {
    let mut dirs: Vec<String> = vec![
        "/usr/lib/x86_64-linux-gnu".to_string(),
        "/lib/x86_64-linux-gnu".to_string(),
        "/usr/lib/gcc/x86_64-linux-gnu/14".to_string(),
    ];

    if let Ok(extra) = env::var("RUST_LLD_NATIVE_DIRS") {
        for dir in extra.split(':').map(|v| v.trim()).filter(|v| !v.is_empty()) {
            if !dirs.iter().any(|existing| existing == dir) {
                dirs.push(dir.to_string());
            }
        }
    }

    dirs
}

fn first_existing_path(candidates: &[PathBuf]) -> Option<PathBuf> {
    for path in candidates {
        if path.is_file() {
            return Some(path.clone());
        }
    }
    None
}

fn find_system_crt(name: &str) -> Option<PathBuf> {
    first_existing_path(&[
        PathBuf::from(format!("/usr/lib/x86_64-linux-gnu/{name}")),
        PathBuf::from(format!("/lib/x86_64-linux-gnu/{name}")),
    ])
}

fn discover_gcc_crt_dir() -> Option<PathBuf> {
    if let Ok(path) = env::var("RUST_LLD_GCC_CRT_DIR") {
        let candidate = PathBuf::from(path);
        if candidate.join("crtbeginS.o").is_file() && candidate.join("crtendS.o").is_file() {
            return Some(candidate);
        }
    }

    for dir in default_native_search_dirs() {
        let candidate = PathBuf::from(dir);
        if candidate.join("crtbeginS.o").is_file() && candidate.join("crtendS.o").is_file() {
            return Some(candidate);
        }
    }

    let gcc_root = Path::new("/usr/lib/gcc/x86_64-linux-gnu");
    if let Ok(entries) = fs::read_dir(gcc_root) {
        for entry in entries.flatten() {
            let candidate = entry.path();
            if candidate.join("crtbeginS.o").is_file() && candidate.join("crtendS.o").is_file() {
                return Some(candidate);
            }
        }
    }

    None
}

fn rust_lld_crt_objects() -> (Vec<PathBuf>, Vec<PathBuf>) {
    let mut prefix = Vec::new();
    let mut suffix = Vec::new();

    if let Some(path) = find_system_crt("Scrt1.o") {
        prefix.push(path);
    }
    if let Some(path) = find_system_crt("crti.o") {
        prefix.push(path);
    }
    if let Some(path) = find_system_crt("crtn.o") {
        suffix.push(path);
    }

    if let Some(gcc_dir) = discover_gcc_crt_dir() {
        prefix.push(gcc_dir.join("crtbeginS.o"));
        suffix.insert(0, gcc_dir.join("crtendS.o"));
    }

    (prefix, suffix)
}

fn compile_flags(repo_root: &Path) -> Vec<String> {
    let masstree_config = repo_root.join("src/mako/masstree/config.h");
    let masstree_support = repo_root.join("src/mako/masstree_btree.h");
    let config_perf = repo_root.join("src/mako/config/config-perf.h");

    vec![
        format!("-DMASSTREE_CONFIG_H=\"{}\"", masstree_config.display()),
        format!("-DMASSTREE_SUPPORT_HEADER=\"{}\"", masstree_support.display()),
        format!("-I{}", repo_root.join("src").display()),
        format!("-I{}", repo_root.join("src/rrr").display()),
        format!("-I{}", repo_root.join("src/memdb").display()),
        format!("-I{}", repo_root.join("src/mako").display()),
        format!("-I{}", repo_root.join("test").display()),
        format!("-I{}", repo_root.join("third-party/rusty-cpp/include").display()),
        "-isystem".to_string(),
        repo_root
            .join("third-party/erpc/third_party/googletest/googletest/include")
            .display()
            .to_string(),
        "-isystem".to_string(),
        repo_root
            .join("third-party/erpc/third_party/googletest/googletest")
            .display()
            .to_string(),
        "-std=gnu++23".to_string(),
        "-w".to_string(),
        "-Wreturn-type".to_string(),
        "-Isrc/mako".to_string(),
        format!("-I{}", repo_root.join("third-party/lz4").display()),
        "-Isrc".to_string(),
        format!("-I{}", repo_root.display()),
        format!("-DCONFIG_H=\"{}\"", config_perf.display()),
        "-DRUSTYCPP_DISABLE_ARC_LOG".to_string(),
        "-DREUSE_CORO".to_string(),
        format!("-I{}", repo_root.join("third-party/erpc/src").display()),
        "-DERPC_FAKE=true".to_string(),
        "-march=native".to_string(),
        "-DERPC_LOG_LEVEL=6".to_string(),
        "-DERPC_TESTING=false".to_string(),
        "-DGFLAGS_IS_A_DLL=0".to_string(),
        "-O2".to_string(),
        "-g".to_string(),
        "-DFAIL_NEW_VERSION".to_string(),
        "-include".to_string(),
        "src/mako/masstree/config.h".to_string(),
        "-DREAD_MY_WRITES=OFF".to_string(),
        "-DHASHTABLE=OFF".to_string(),
        "-DSTO_OPACITY=OFF".to_string(),
        format!(
            "-I{}",
            repo_root
                .join("third-party/erpc/third_party/asio/include")
                .display()
        ),
        "-fno-omit-frame-pointer".to_string(),
    ]
}

fn main() {
    println!("cargo:rerun-if-env-changed=FRAGILEC_BUILD_ID");
    println!("cargo:rerun-if-env-changed=FRAGILEC_KEEP_RS");
    println!("cargo:rerun-if-env-changed=FRAGILEC_PARSER_BACKEND");
    println!("cargo:rerun-if-env-changed=FRAGILEC_TRANSPILE_STAGE_TIMING_PATH");
    println!("cargo:rerun-if-env-changed=RUSTC_BIN");
    println!("cargo:rerun-if-env-changed=RUST_LLD_NATIVE_DIRS");
    println!("cargo:rerun-if-env-changed=RUST_LLD_GCC_CRT_DIR");
    println!("cargo:rerun-if-env-changed=RUST_LLD_DYNAMIC_LINKER");
    println!("cargo:rerun-if-env-changed=MAKO_BUILD_DIR");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=README.md");

    let manifest_dir = PathBuf::from(
        env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR must be set"),
    );
    let repo_root = manifest_dir
        .parent()
        .expect("cargo-mako-test-rpc must live under repo root")
        .to_path_buf();
    let build_dir = env::var("MAKO_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root.join(DEFAULT_MAKO_BUILD_DIR_REL));
    if let Err(err) = validate_build_dir_artifacts(&build_dir) {
        panic!("{err}");
    }

    let all_compile_flags = compile_flags(&repo_root);
    let compile_flag_refs: Vec<&str> = all_compile_flags.iter().map(String::as_str).collect();

    let rustc = env::var("RUSTC_BIN").unwrap_or_else(|_| "rustc".to_string());
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR must be set"));
    let obj_root = out_dir.join("mako_obj");
    let bin_root = out_dir.join("mako_bin");
    let linked_bin = bin_root.join(OUTPUT_NAME);
    let link_src = out_dir.join("mako_bridge_link.rs");

    if let Err(err) = fs::create_dir_all(&obj_root) {
        panic!("failed to create object root {}: {err}", obj_root.display());
    }
    if let Err(err) = fs::create_dir_all(&bin_root) {
        panic!("failed to create binary root {}: {err}", bin_root.display());
    }
    if let Err(err) = fs::write(&link_src, "#![no_main]\n") {
        panic!(
            "failed to write bridge link source {}: {err}",
            link_src.display()
        );
    }

    for unit in COMPILE_UNITS {
        let source_path = repo_root.join(unit.source_rel);
        println!("cargo:rerun-if-changed={}", source_path.display());

        let out_obj = obj_root.join(unit.object_rel);
        if let Some(parent) = out_obj.parent() {
            if let Err(err) = fs::create_dir_all(parent) {
                panic!("failed to create object parent {}: {err}", parent.display());
            }
        }

        if let Err(err) = fragile_driver::compile_unit_with_flags_in_dir(
            &source_path,
            &out_obj,
            &compile_flag_refs,
            &build_dir,
        ) {
            panic!(
                "mako cargo bridge compile failed via fragile-driver for target '{}' unit '{}':\n{}",
                TARGET_NAME,
                source_path.display(),
                err
            );
        }
    }

    let mut link_args: Vec<String> = Vec::new();
    link_args.push(format!("-Wl,-rpath,{}", build_dir.join("lib").display()));
    link_args.extend(STATIC_LINK_ARGS.iter().map(|arg| (*arg).to_string()));

    let mut link_cmd = Command::new(&rustc);
    link_cmd.current_dir(&build_dir);
    link_cmd.arg("--edition");
    link_cmd.arg("2021");
    link_cmd.arg("-C");
    link_cmd.arg("linker=rust-lld");
    link_cmd.arg("-C");
    link_cmd.arg("panic=abort");

    for native_dir in default_native_search_dirs() {
        link_cmd.arg("-L");
        link_cmd.arg(format!("native={native_dir}"));
    }

    link_cmd.arg(&link_src);
    link_cmd.arg("-o");
    link_cmd.arg(&linked_bin);

    let dynamic_linker = env::var("RUST_LLD_DYNAMIC_LINKER")
        .unwrap_or_else(|_| "/lib64/ld-linux-x86-64.so.2".to_string());
    link_cmd.arg("-C");
    link_cmd.arg("link-arg=-dynamic-linker");
    link_cmd.arg("-C");
    link_cmd.arg(format!("link-arg={dynamic_linker}"));

    let (crt_prefix, crt_suffix) = rust_lld_crt_objects();
    for crt in &crt_prefix {
        link_cmd.arg("-C");
        link_cmd.arg(format!("link-arg={}", crt.display()));
    }

    for unit in COMPILE_UNITS {
        link_cmd.arg("-C");
        link_cmd.arg(format!("link-arg={}", obj_root.join(unit.object_rel).display()));
    }

    for arg in normalize_link_args_for_rust_lld(link_args.iter().map(String::as_str)) {
        link_cmd.arg("-C");
        link_cmd.arg(format!("link-arg={arg}"));
    }

    // Keep core C/POSIX runtime link flags explicit for raw rust-lld usage.
    for c_lib in ["-lgcc_s", "-lutil", "-lrt", "-lpthread", "-lm", "-ldl", "-lc"] {
        link_cmd.arg("-C");
        link_cmd.arg(format!("link-arg={c_lib}"));
    }

    for crt in &crt_suffix {
        link_cmd.arg("-C");
        link_cmd.arg(format!("link-arg={}", crt.display()));
    }

    if let Err(err) = run_checked(link_cmd, "link") {
        panic!(
            "mako cargo bridge rust-lld link failed for target '{}':\n{}",
            TARGET_NAME, err
        );
    }

    let dist_dir = manifest_dir.join("dist");
    if let Err(err) = fs::create_dir_all(&dist_dir) {
        panic!("failed to create dist dir {}: {err}", dist_dir.display());
    }
    let dist_bin = dist_dir.join(OUTPUT_NAME);
    if let Err(err) = fs::copy(&linked_bin, &dist_bin) {
        panic!(
            "failed to copy linked binary from {} to {}: {err}",
            linked_bin.display(),
            dist_bin.display()
        );
    }
    println!(
        "cargo:warning=mako cargo bridge built '{}' at {}",
        OUTPUT_NAME,
        dist_bin.display()
    );
}
