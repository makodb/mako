use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};
use walkdir::WalkDir;

const DEFAULT_SPEC_PATH: &str = "cargo-native/targets.toml";
const DEFAULT_OUT_DIR: &str = "target/native-build";

#[derive(Debug)]
struct ExportOptions {
    build_dir: PathBuf,
    out_path: PathBuf,
    queries: Vec<String>,
    all_link_targets: bool,
}

#[derive(Debug)]
struct BuildOptions {
    spec_path: PathBuf,
    out_dir: PathBuf,
    target: Option<String>,
    all: bool,
}

#[derive(Debug)]
enum Action {
    ExportSpec(ExportOptions),
    List { spec_path: PathBuf },
    Build(BuildOptions),
    Help,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum TargetKind {
    StaticLibrary,
    SharedLibrary,
    Executable,
}

impl TargetKind {
    fn as_str(&self) -> &'static str {
        match self {
            Self::StaticLibrary => "static",
            Self::SharedLibrary => "shared",
            Self::Executable => "executable",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct SourceSpec {
    path: String,
    flags: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct TargetSpec {
    name: String,
    kind: TargetKind,
    output: String,
    sources: Vec<SourceSpec>,
    deps: Vec<String>,
    link_args: Vec<String>,
    ar_flags: Vec<String>,
    extra_members: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct GeneratedFrom {
    build_dir: String,
    generated_at_unix: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct NativeBuildSpec {
    version: u32,
    generated_from: GeneratedFrom,
    targets: Vec<TargetSpec>,
}

#[derive(Debug, Deserialize)]
struct CompileCommandEntry {
    directory: Option<String>,
    command: Option<String>,
    arguments: Option<Vec<String>>,
    file: String,
    output: Option<String>,
}

#[derive(Debug, Clone)]
struct CompileUnit {
    source_abs: PathBuf,
    object_token: String,
    object_abs: PathBuf,
    flags: Vec<String>,
}

#[derive(Debug, Clone)]
struct LinkTargetCandidate {
    target_name: String,
    link_txt_path: PathBuf,
}

#[derive(Debug)]
struct ExportedTargetDraft {
    name: String,
    kind: TargetKind,
    output: String,
    sources: Vec<SourceSpec>,
    deps: Vec<String>,
    link_args: Vec<String>,
    ar_flags: Vec<String>,
    extra_members: Vec<String>,
}

#[derive(Debug)]
enum LinkPlan {
    RustLink {
        output_name: String,
        link_args: Vec<String>,
    },
    StaticArchive {
        output_name: String,
        ar_flags: Vec<String>,
        extra_members: Vec<PathBuf>,
    },
}

fn main() {
    if let Err(err) = run() {
        eprintln!("error: {err}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let mut args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        print_help();
        return Ok(());
    }

    let action = parse_action(&mut args)?;
    let repo_root = repo_root()?;

    match action {
        Action::Help => {
            print_help();
            Ok(())
        }
        Action::ExportSpec(opts) => export_spec(&repo_root, &opts),
        Action::List { spec_path } => {
            let spec = load_spec(&repo_root, &spec_path)?;
            println!("spec: {}", absolutize(&repo_root, &spec_path).display());
            println!("targets: {}", spec.targets.len());
            for target in &spec.targets {
                println!(
                    "{}\t{}\t{}\tdeps={}",
                    target.kind.as_str(),
                    target.name,
                    target.output,
                    target.deps.len()
                );
            }
            Ok(())
        }
        Action::Build(opts) => build_from_spec(&repo_root, &opts),
    }
}

fn parse_action(args: &mut Vec<String>) -> Result<Action, String> {
    let cmd = args.remove(0);

    match cmd.as_str() {
        "help" | "-h" | "--help" => Ok(Action::Help),
        "export-spec" => parse_export_spec(args),
        "list" => parse_list(args),
        "build" => parse_build(args),
        other => Err(format!(
            "unknown command '{}'. Run `cargo mako-native help`.",
            other
        )),
    }
}

fn parse_export_spec(args: &mut Vec<String>) -> Result<Action, String> {
    let mut build_dir: Option<PathBuf> = None;
    let mut out_path = PathBuf::from(DEFAULT_SPEC_PATH);
    let mut queries: Vec<String> = Vec::new();
    let mut all_link_targets = false;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--build-dir" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --build-dir".to_string())?;
                build_dir = Some(PathBuf::from(value));
                i += 2;
            }
            "--out" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --out".to_string())?;
                out_path = PathBuf::from(value);
                i += 2;
            }
            "--target" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --target".to_string())?;
                queries.push(value.clone());
                i += 2;
            }
            "--all-link-targets" => {
                all_link_targets = true;
                i += 1;
            }
            other => {
                return Err(format!(
                    "unknown export-spec option '{}'. Run `cargo mako-native help`.",
                    other
                ));
            }
        }
    }
    args.clear();

    let build_dir =
        build_dir.ok_or_else(|| "export-spec requires --build-dir <path>".to_string())?;

    Ok(Action::ExportSpec(ExportOptions {
        build_dir,
        out_path,
        queries,
        all_link_targets,
    }))
}

fn parse_list(args: &mut Vec<String>) -> Result<Action, String> {
    let mut spec_path = PathBuf::from(DEFAULT_SPEC_PATH);
    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--spec" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --spec".to_string())?;
                spec_path = PathBuf::from(value);
                i += 2;
            }
            other => {
                return Err(format!(
                    "unknown list option '{}'. Run `cargo mako-native help`.",
                    other
                ));
            }
        }
    }
    args.clear();

    Ok(Action::List { spec_path })
}

fn parse_build(args: &mut Vec<String>) -> Result<Action, String> {
    let mut spec_path = PathBuf::from(DEFAULT_SPEC_PATH);
    let mut out_dir = PathBuf::from(DEFAULT_OUT_DIR);
    let mut target: Option<String> = None;
    let mut all = false;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--spec" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --spec".to_string())?;
                spec_path = PathBuf::from(value);
                i += 2;
            }
            "--out-dir" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --out-dir".to_string())?;
                out_dir = PathBuf::from(value);
                i += 2;
            }
            "--target" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --target".to_string())?;
                target = Some(value.clone());
                i += 2;
            }
            "--all" => {
                all = true;
                i += 1;
            }
            other => {
                return Err(format!(
                    "unknown build option '{}'. Run `cargo mako-native help`.",
                    other
                ));
            }
        }
    }
    args.clear();

    if !all && target.is_none() {
        return Err("build requires either --target <name> or --all".to_string());
    }

    Ok(Action::Build(BuildOptions {
        spec_path,
        out_dir,
        target,
        all,
    }))
}

fn export_spec(repo_root: &Path, opts: &ExportOptions) -> Result<(), String> {
    let build_dir = absolutize(repo_root, &opts.build_dir);
    let compile_commands_path = build_dir.join("compile_commands.json");
    if !compile_commands_path.is_file() {
        return Err(format!("missing {}", compile_commands_path.display()));
    }

    let all_candidates = discover_link_targets(&build_dir)?;
    if all_candidates.is_empty() {
        return Err(format!(
            "no link.txt targets found under {}",
            build_dir.display()
        ));
    }

    let selected = if opts.all_link_targets {
        all_candidates
    } else if !opts.queries.is_empty() {
        select_candidates(&all_candidates, &build_dir, &opts.queries)?
    } else {
        select_candidates(
            &all_candidates,
            &build_dir,
            &[
                "rrr".to_string(),
                "memdb".to_string(),
                "rpcbench".to_string(),
            ],
        )?
    };

    let entries = load_compile_commands(&compile_commands_path)?;

    let mut drafts: Vec<ExportedTargetDraft> = Vec::new();
    for candidate in &selected {
        drafts.push(export_target_draft(
            repo_root, &build_dir, &entries, candidate,
        )?);
    }

    resolve_internal_deps(&mut drafts);

    let mut targets = drafts
        .into_iter()
        .map(|draft| TargetSpec {
            name: draft.name,
            kind: draft.kind,
            output: draft.output,
            sources: draft.sources,
            deps: draft.deps,
            link_args: draft.link_args,
            ar_flags: draft.ar_flags,
            extra_members: draft.extra_members,
        })
        .collect::<Vec<_>>();

    targets.sort_by(|a, b| a.name.cmp(&b.name));

    let generated_at_unix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| format!("failed to compute timestamp: {e}"))?
        .as_secs();

    let spec = NativeBuildSpec {
        version: 1,
        generated_from: GeneratedFrom {
            build_dir: normalize_path(&build_dir),
            generated_at_unix,
        },
        targets,
    };

    let out_path = absolutize(repo_root, &opts.out_path);
    if let Some(parent) = out_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("failed to create {}: {e}", parent.display()))?;
    }

    let serialized = toml::to_string_pretty(&spec)
        .map_err(|e| format!("failed to serialize spec to TOML: {e}"))?;
    fs::write(&out_path, serialized)
        .map_err(|e| format!("failed to write {}: {e}", out_path.display()))?;

    println!(
        "exported native build spec with {} target(s) to {}",
        spec.targets.len(),
        out_path.display()
    );

    Ok(())
}

fn resolve_internal_deps(drafts: &mut [ExportedTargetDraft]) {
    let mut output_to_target: HashMap<String, String> = HashMap::new();
    for target in drafts.iter() {
        output_to_target.insert(target.output.clone(), target.name.clone());
    }

    for target in drafts.iter_mut() {
        let mut deps: Vec<String> = Vec::new();
        let mut filtered: Vec<String> = Vec::new();

        for arg in &target.link_args {
            let basename = Path::new(arg)
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or(arg);

            if let Some(dep_name) = output_to_target.get(basename) {
                if dep_name != &target.name && !deps.iter().any(|d| d == dep_name) {
                    deps.push(dep_name.clone());
                }
                continue;
            }
            filtered.push(arg.clone());
        }

        target.deps = deps;
        target.link_args = filtered;
    }
}

fn export_target_draft(
    repo_root: &Path,
    build_dir: &Path,
    compile_entries: &[CompileCommandEntry],
    candidate: &LinkTargetCandidate,
) -> Result<ExportedTargetDraft, String> {
    let target_name = candidate.target_name.clone();
    let target_obj_dir_rel = target_obj_dir_rel_from_link_txt(&candidate.link_txt_path, build_dir)?;
    let link_work_dir = infer_link_work_dir(&candidate.link_txt_path, build_dir);
    let compile_units = collect_target_compile_units(
        compile_entries,
        build_dir,
        &target_name,
        &target_obj_dir_rel,
    )?;

    let link_commands = parse_link_commands(&candidate.link_txt_path)?;
    let first = link_commands
        .first()
        .ok_or_else(|| format!("empty link.txt: {}", candidate.link_txt_path.display()))?;

    let plan = if is_ar_tool(&first[0]) {
        parse_archive_plan(
            &target_name,
            first,
            &link_commands[1..],
            &compile_units,
            &link_work_dir,
        )?
    } else {
        parse_rust_link_plan(&target_name, first, &compile_units, &link_work_dir)
    };

    let sources = compile_units
        .iter()
        .map(|unit| SourceSpec {
            path: rel_or_abs(repo_root, &unit.source_abs),
            flags: unit
                .flags
                .iter()
                .map(|flag| rewrite_token_for_spec(flag, repo_root))
                .collect(),
        })
        .collect::<Vec<_>>();

    let draft = match plan {
        LinkPlan::RustLink {
            output_name,
            link_args,
        } => {
            let kind = if output_name.ends_with(".a") {
                TargetKind::StaticLibrary
            } else if is_shared_library_name(&output_name) {
                TargetKind::SharedLibrary
            } else {
                TargetKind::Executable
            };
            ExportedTargetDraft {
                name: target_name,
                kind,
                output: output_name,
                sources,
                deps: Vec::new(),
                link_args: link_args
                    .into_iter()
                    .map(|arg| rewrite_token_for_spec(&arg, repo_root))
                    .collect(),
                ar_flags: Vec::new(),
                extra_members: Vec::new(),
            }
        }
        LinkPlan::StaticArchive {
            output_name,
            ar_flags,
            extra_members,
        } => ExportedTargetDraft {
            name: target_name,
            kind: TargetKind::StaticLibrary,
            output: output_name,
            sources,
            deps: Vec::new(),
            link_args: Vec::new(),
            ar_flags: ar_flags
                .into_iter()
                .map(|arg| rewrite_token_for_spec(&arg, repo_root))
                .collect(),
            extra_members: extra_members
                .into_iter()
                .map(|p| rel_or_abs(repo_root, &p))
                .collect(),
        },
    };

    Ok(draft)
}

fn build_from_spec(repo_root: &Path, opts: &BuildOptions) -> Result<(), String> {
    let spec = load_spec(repo_root, &opts.spec_path)?;
    let out_dir = absolutize(repo_root, &opts.out_dir);
    fs::create_dir_all(&out_dir)
        .map_err(|e| format!("failed to create {}: {e}", out_dir.display()))?;

    let rustc = env::var("RUSTC_BIN").unwrap_or_else(|_| "rustc".to_string());
    let ar = pick_tool("MAKO_NATIVE_AR", &["llvm-ar", "ar"], "archiver")?;

    let mut by_name: HashMap<String, TargetSpec> = HashMap::new();
    for target in spec.targets {
        by_name.insert(target.name.clone(), target);
    }

    let mut built: HashMap<String, PathBuf> = HashMap::new();
    let mut stack: HashSet<String> = HashSet::new();

    if opts.all {
        let mut names = by_name.keys().cloned().collect::<Vec<_>>();
        names.sort();
        for name in names {
            let _ = build_target_recursive(
                repo_root, &out_dir, &name, &by_name, &mut built, &mut stack, &rustc, &ar,
            )?;
        }
    } else if let Some(target_name) = &opts.target {
        let output = build_target_recursive(
            repo_root,
            &out_dir,
            target_name,
            &by_name,
            &mut built,
            &mut stack,
            &rustc,
            &ar,
        )?;
        println!("built target '{}': {}", target_name, output.display());
    }

    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn build_target_recursive(
    repo_root: &Path,
    out_dir: &Path,
    name: &str,
    by_name: &HashMap<String, TargetSpec>,
    built: &mut HashMap<String, PathBuf>,
    stack: &mut HashSet<String>,
    rustc: &str,
    ar: &str,
) -> Result<PathBuf, String> {
    if let Some(path) = built.get(name) {
        return Ok(path.clone());
    }

    if stack.contains(name) {
        return Err(format!("cycle detected while building target '{name}'"));
    }

    let target = by_name
        .get(name)
        .ok_or_else(|| format!("unknown target '{name}'"))?
        .clone();

    stack.insert(name.to_string());

    let mut dep_outputs = Vec::new();
    for dep in &target.deps {
        dep_outputs.push(build_target_recursive(
            repo_root, out_dir, dep, by_name, built, stack, rustc, ar,
        )?);
    }

    let obj_dir = out_dir.join("obj").join(&target.name);
    fs::create_dir_all(&obj_dir)
        .map_err(|e| format!("failed to create {}: {e}", obj_dir.display()))?;

    let mut object_paths = Vec::new();
    for (idx, src) in target.sources.iter().enumerate() {
        let src_abs = absolutize(repo_root, Path::new(&src.path));
        if !src_abs.is_file() {
            return Err(format!(
                "source file for target '{}' does not exist: {}",
                target.name,
                src_abs.display()
            ));
        }
        let stem = src_abs
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("obj");
        let obj_path = obj_dir.join(format!("{idx:04}_{stem}.o"));

        let expanded_flags: Vec<String> = src
            .flags
            .iter()
            .map(|flag| expand_token_from_spec(flag, repo_root))
            .collect();
        let flag_refs: Vec<&str> = expanded_flags.iter().map(String::as_str).collect();
        fragile_driver::compile_unit_with_flags_in_dir(&src_abs, &obj_path, &flag_refs, repo_root)
            .map_err(|e| {
                format!(
                    "fragile compile failed for target '{}' unit '{}':\n{}",
                    target.name,
                    src_abs.display(),
                    e
                )
            })?;
        object_paths.push(obj_path);
    }

    let output_path = target_output_path(out_dir, &target);
    if let Some(parent) = output_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("failed to create {}: {e}", parent.display()))?;
    }

    match target.kind {
        TargetKind::StaticLibrary => {
            let mut cmd = Command::new(ar);
            cmd.current_dir(repo_root);
            if target.ar_flags.is_empty() {
                cmd.arg("rcs");
            } else {
                for flag in &target.ar_flags {
                    cmd.arg(expand_token_from_spec(flag, repo_root));
                }
            }
            cmd.arg(&output_path);
            for obj in &object_paths {
                cmd.arg(obj);
            }
            for extra in &target.extra_members {
                cmd.arg(absolutize(repo_root, Path::new(extra)));
            }
            run_checked(cmd, &format!("archive {}", target.name))?;
        }
        TargetKind::SharedLibrary | TargetKind::Executable => {
            rust_lld_link_target(
                repo_root,
                out_dir,
                rustc,
                &target,
                &object_paths,
                &dep_outputs,
                &output_path,
            )?;
        }
    }

    stack.remove(name);
    built.insert(name.to_string(), output_path.clone());
    Ok(output_path)
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
        if arg == "-rdynamic" {
            normalized.push("--export-dynamic".to_string());
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

fn add_rust_lld_arg(cmd: &mut Command, arg: &str) {
    cmd.arg("-C");
    cmd.arg(format!("link-arg={arg}"));
}

fn rust_lld_link_target(
    repo_root: &Path,
    out_dir: &Path,
    rustc: &str,
    target: &TargetSpec,
    object_paths: &[PathBuf],
    dep_outputs: &[PathBuf],
    output_path: &Path,
) -> Result<(), String> {
    let link_src_dir = out_dir.join("link-src");
    fs::create_dir_all(&link_src_dir)
        .map_err(|e| format!("failed to create {}: {e}", link_src_dir.display()))?;
    let link_src = link_src_dir.join(format!("{}_link.rs", target.name.replace('/', "_")));
    fs::write(&link_src, "#![no_main]\n")
        .map_err(|e| format!("failed to write link source {}: {e}", link_src.display()))?;

    let mut all_link_args: Vec<String> = Vec::new();
    all_link_args.push(format!("-Wl,-rpath,{}", out_dir.display()));
    all_link_args.push(format!("-Wl,-rpath,{}", out_dir.join("lib").display()));
    all_link_args.extend(dep_outputs.iter().map(|path| path.display().to_string()));
    all_link_args.extend(
        target
            .link_args
            .iter()
            .map(|arg| expand_token_from_spec(arg, repo_root)),
    );

    let mut link_cmd = Command::new(rustc);
    link_cmd.current_dir(repo_root);
    link_cmd.arg("--edition");
    link_cmd.arg("2021");
    if matches!(target.kind, TargetKind::SharedLibrary) {
        link_cmd.arg("--crate-type");
        link_cmd.arg("cdylib");
    }
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
    link_cmd.arg(output_path);

    if matches!(target.kind, TargetKind::Executable) {
        let dynamic_linker = env::var("RUST_LLD_DYNAMIC_LINKER")
            .unwrap_or_else(|_| "/lib64/ld-linux-x86-64.so.2".to_string());
        add_rust_lld_arg(&mut link_cmd, "-dynamic-linker");
        add_rust_lld_arg(&mut link_cmd, &dynamic_linker);

        let (crt_prefix, crt_suffix) = rust_lld_crt_objects();
        for crt in &crt_prefix {
            add_rust_lld_arg(&mut link_cmd, &crt.display().to_string());
        }

        for obj in object_paths {
            add_rust_lld_arg(&mut link_cmd, &obj.display().to_string());
        }

        for arg in normalize_link_args_for_rust_lld(all_link_args.iter().map(String::as_str)) {
            add_rust_lld_arg(&mut link_cmd, &arg);
        }

        for c_lib in [
            "-lgcc_s",
            "-lutil",
            "-lrt",
            "-lpthread",
            "-lm",
            "-ldl",
            "-lc",
        ] {
            add_rust_lld_arg(&mut link_cmd, c_lib);
        }

        for crt in &crt_suffix {
            add_rust_lld_arg(&mut link_cmd, &crt.display().to_string());
        }
    } else {
        for obj in object_paths {
            add_rust_lld_arg(&mut link_cmd, &obj.display().to_string());
        }

        for arg in normalize_link_args_for_rust_lld(all_link_args.iter().map(String::as_str)) {
            add_rust_lld_arg(&mut link_cmd, &arg);
        }

        for c_lib in [
            "-lgcc_s",
            "-lutil",
            "-lrt",
            "-lpthread",
            "-lm",
            "-ldl",
            "-lc",
        ] {
            add_rust_lld_arg(&mut link_cmd, c_lib);
        }
    }

    run_checked(link_cmd, &format!("link {}", target.name))
}

fn target_output_path(out_dir: &Path, target: &TargetSpec) -> PathBuf {
    match target.kind {
        TargetKind::Executable => out_dir.join("bin").join(&target.output),
        TargetKind::SharedLibrary | TargetKind::StaticLibrary => {
            out_dir.join("lib").join(&target.output)
        }
    }
}

fn load_spec(repo_root: &Path, spec_path: &Path) -> Result<NativeBuildSpec, String> {
    let path = absolutize(repo_root, spec_path);
    let raw = fs::read_to_string(&path)
        .map_err(|e| format!("failed to read spec {}: {e}", path.display()))?;
    toml::from_str(&raw).map_err(|e| format!("failed to parse spec {}: {e}", path.display()))
}

fn pick_tool(env_name: &str, candidates: &[&str], label: &str) -> Result<String, String> {
    if let Ok(value) = env::var(env_name) {
        if !value.trim().is_empty() {
            return Ok(value);
        }
    }

    for candidate in candidates {
        let mut cmd = Command::new(candidate);
        cmd.arg("--version")
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        if cmd.status().is_ok_and(|status| status.success()) {
            return Ok((*candidate).to_string());
        }
    }

    Err(format!(
        "failed to locate {}. Set {} environment variable.",
        label, env_name
    ))
}

fn select_candidates(
    all: &[LinkTargetCandidate],
    build_dir: &Path,
    queries: &[String],
) -> Result<Vec<LinkTargetCandidate>, String> {
    let mut selected = Vec::new();
    for query in queries {
        let q = query.trim();
        if q.is_empty() {
            continue;
        }
        let mut matches = Vec::new();
        for candidate in all {
            let id = link_target_id(&candidate.link_txt_path, build_dir)
                .unwrap_or_else(|| candidate.target_name.clone());
            if candidate.target_name == q || id == q || id.ends_with(&format!("/{q}")) {
                matches.push(candidate.clone());
            }
        }
        match matches.len() {
            0 => {
                return Err(format!(
                    "target query '{}' not found under {}",
                    q,
                    build_dir.display()
                ));
            }
            1 => selected.push(matches.remove(0)),
            _ => {
                let sample = matches
                    .iter()
                    .take(8)
                    .map(|entry| {
                        link_target_id(&entry.link_txt_path, build_dir)
                            .unwrap_or_else(|| entry.link_txt_path.display().to_string())
                    })
                    .collect::<Vec<_>>()
                    .join("\n");
                return Err(format!(
                    "target query '{}' is ambiguous. Matching link targets:\n{}",
                    q, sample
                ));
            }
        }
    }

    let mut uniq = BTreeMap::new();
    for entry in selected {
        let key = entry.link_txt_path.clone();
        uniq.insert(key, entry);
    }
    Ok(uniq.into_values().collect())
}

fn discover_link_targets(build_dir: &Path) -> Result<Vec<LinkTargetCandidate>, String> {
    let mut out = Vec::new();
    for entry in WalkDir::new(build_dir) {
        let entry = entry.map_err(|e| format!("failed to walk {}: {e}", build_dir.display()))?;
        if !entry.file_type().is_file() {
            continue;
        }
        if entry.file_name() != OsStr::new("link.txt") {
            continue;
        }
        let Some(parent_name) = entry
            .path()
            .parent()
            .and_then(|p| p.file_name())
            .and_then(|s| s.to_str())
        else {
            continue;
        };
        if !parent_name.ends_with(".dir") {
            continue;
        }
        let target_name = parent_name
            .strip_suffix(".dir")
            .unwrap_or(parent_name)
            .to_string();
        out.push(LinkTargetCandidate {
            target_name,
            link_txt_path: canonical_or_self(entry.path().to_path_buf()),
        });
    }

    out.sort_by(|a, b| {
        a.target_name
            .cmp(&b.target_name)
            .then_with(|| a.link_txt_path.cmp(&b.link_txt_path))
    });
    Ok(out)
}

fn load_compile_commands(path: &Path) -> Result<Vec<CompileCommandEntry>, String> {
    let raw =
        fs::read_to_string(path).map_err(|e| format!("failed to read {}: {e}", path.display()))?;
    serde_json::from_str(&raw)
        .map_err(|e| format!("failed to parse compile_commands {}: {e}", path.display()))
}

fn parse_command_tokens(entry: &CompileCommandEntry) -> Result<Vec<String>, String> {
    if let Some(args) = &entry.arguments {
        if !args.is_empty() {
            return Ok(args.clone());
        }
    }

    if let Some(command) = &entry.command {
        if !command.trim().is_empty() {
            return shell_words::split(command)
                .map_err(|e| format!("failed to parse compile command '{}': {e}", command));
        }
    }

    Err(format!(
        "compile_commands entry missing command/arguments for source '{}'",
        entry.file
    ))
}

fn collect_target_compile_units(
    entries: &[CompileCommandEntry],
    build_dir: &Path,
    target_name: &str,
    target_obj_dir_rel: &str,
) -> Result<Vec<CompileUnit>, String> {
    let prefix = format!("{}/", target_obj_dir_rel.trim_end_matches('/'));
    let mut selected: BTreeMap<String, &CompileCommandEntry> = BTreeMap::new();

    for entry in entries {
        let Some(output) = entry.output.as_ref() else {
            continue;
        };

        let output_rel = output_rel_to_build(output, build_dir);
        if !output_rel.ends_with(".o") || !output_rel.starts_with(&prefix) {
            continue;
        }

        selected.insert(output_rel, entry);
    }

    if selected.is_empty() {
        return Err(format!(
            "no compile units matched target '{}' with prefix '{}'",
            target_name, prefix
        ));
    }

    let mut units = Vec::new();
    for (_out_rel, entry) in selected {
        let compile_dir = entry
            .directory
            .as_deref()
            .map(PathBuf::from)
            .unwrap_or_else(|| build_dir.to_path_buf());
        let source_abs = canonical_or_self(resolve_path(&compile_dir, &entry.file));
        let tokens = parse_command_tokens(entry)?;
        let flags = sanitize_compile_flags(&tokens, &source_abs, &compile_dir);

        let object_token = entry
            .output
            .as_ref()
            .expect("selected entry must have output")
            .clone();
        let object_abs = canonical_or_self(resolve_path(build_dir, &object_token));

        units.push(CompileUnit {
            source_abs,
            object_token,
            object_abs,
            flags,
        });
    }

    Ok(units)
}

fn sanitize_compile_flags(tokens: &[String], source_abs: &Path, compile_dir: &Path) -> Vec<String> {
    let mut out = Vec::new();
    let mut i = 1usize; // skip compiler binary

    while i < tokens.len() {
        let tok = &tokens[i];

        if matches!(tok.as_str(), "-c" | "--compile") {
            i += 1;
            continue;
        }

        if matches!(
            tok.as_str(),
            "-o" | "--output" | "-MF" | "-MT" | "-MQ" | "-MJ"
        ) {
            i += 2;
            continue;
        }

        if matches!(tok.as_str(), "-MD" | "-MMD" | "-MP") {
            i += 1;
            continue;
        }

        if tok.starts_with("-o") && tok != "-o" {
            i += 1;
            continue;
        }

        if is_same_source_token(tok, source_abs, compile_dir) {
            i += 1;
            continue;
        }

        out.push(tok.clone());
        i += 1;
    }

    out
}

fn is_same_source_token(token: &str, source_abs: &Path, compile_dir: &Path) -> bool {
    if token.starts_with('-') {
        return false;
    }
    let token_abs = resolve_path(compile_dir, token);
    paths_equivalent(&token_abs, source_abs)
}

fn parse_link_commands(link_txt_path: &Path) -> Result<Vec<Vec<String>>, String> {
    let raw = fs::read_to_string(link_txt_path)
        .map_err(|e| format!("failed to read {}: {e}", link_txt_path.display()))?;

    let mut commands = Vec::new();
    for (idx, line) in raw.lines().enumerate() {
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        let tokens = shell_words::split(trimmed).map_err(|e| {
            format!(
                "failed to parse link command line {} in {}: {}",
                idx + 1,
                link_txt_path.display(),
                e
            )
        })?;
        if !tokens.is_empty() {
            commands.push(tokens);
        }
    }

    if commands.is_empty() {
        return Err(format!("empty link.txt: {}", link_txt_path.display()));
    }

    Ok(commands)
}

fn parse_rust_link_plan(
    target_name: &str,
    link_tokens: &[String],
    compile_units: &[CompileUnit],
    link_work_dir: &Path,
) -> LinkPlan {
    let object_tokens: HashSet<String> = compile_units
        .iter()
        .map(|u| normalize_rel_token(&u.object_token))
        .collect();
    let object_abs: HashSet<PathBuf> = compile_units.iter().map(|u| u.object_abs.clone()).collect();

    let mut output_name = target_name.to_string();
    let mut link_args = Vec::new();

    let mut i = 1usize; // skip linker driver
    while i < link_tokens.len() {
        let tok = &link_tokens[i];

        if tok == "-o" && i + 1 < link_tokens.len() {
            output_name = output_name_from_token(&link_tokens[i + 1], target_name);
            i += 2;
            continue;
        }

        if let Some(rest) = tok.strip_prefix("-o") {
            if !rest.is_empty() {
                output_name = output_name_from_token(rest, target_name);
                i += 1;
                continue;
            }
        }

        if tok.starts_with("-Wl,--dependency-file=") {
            i += 1;
            continue;
        }

        if object_tokens.contains(&normalize_rel_token(tok)) {
            i += 1;
            continue;
        }

        if tok.ends_with(".o") {
            let tok_abs = canonical_or_self(resolve_path(link_work_dir, tok));
            if object_abs.contains(&tok_abs) {
                i += 1;
                continue;
            }
        }

        if tok == "-L" && i + 1 < link_tokens.len() {
            link_args.push("-L".to_string());
            link_args.push(absolutize_link_token(
                &link_tokens[i + 1],
                link_work_dir,
                true,
            ));
            i += 2;
            continue;
        }

        if let Some(rest) = tok.strip_prefix("-L") {
            if !rest.is_empty() {
                link_args.push(format!(
                    "-L{}",
                    absolutize_link_token(rest, link_work_dir, true)
                ));
                i += 1;
                continue;
            }
        }

        link_args.push(absolutize_link_token(tok, link_work_dir, false));
        i += 1;
    }

    LinkPlan::RustLink {
        output_name,
        link_args,
    }
}

fn parse_archive_plan(
    target_name: &str,
    link_tokens: &[String],
    _trailing_commands: &[Vec<String>],
    compile_units: &[CompileUnit],
    link_work_dir: &Path,
) -> Result<LinkPlan, String> {
    if link_tokens.is_empty() {
        return Err("archive command is empty".to_string());
    }

    let mut idx = 1usize;
    let mut ar_flags = Vec::new();
    while idx < link_tokens.len() {
        let tok = &link_tokens[idx];
        let looks_like_flag =
            tok.starts_with('-') || (idx == 1 && tok.chars().all(|ch| ch.is_ascii_alphabetic()));
        if !looks_like_flag {
            break;
        }
        ar_flags.push(tok.clone());
        idx += 1;
    }

    if idx >= link_tokens.len() {
        return Err(format!(
            "archive target '{}' has no output token",
            target_name
        ));
    }

    let output_token = link_tokens[idx].clone();
    let output_name = output_name_from_token(&output_token, target_name);
    idx += 1;

    let object_tokens: HashSet<String> = compile_units
        .iter()
        .map(|u| normalize_rel_token(&u.object_token))
        .collect();
    let object_abs: HashSet<PathBuf> = compile_units.iter().map(|u| u.object_abs.clone()).collect();

    let mut extra_members = Vec::new();
    for tok in &link_tokens[idx..] {
        if !tok.ends_with(".o") && !tok.ends_with(".obj") {
            continue;
        }
        if object_tokens.contains(&normalize_rel_token(tok)) {
            continue;
        }
        let tok_abs = canonical_or_self(resolve_path(link_work_dir, tok));
        if object_abs.contains(&tok_abs) {
            continue;
        }
        if !extra_members.iter().any(|p| p == &tok_abs) {
            extra_members.push(tok_abs);
        }
    }

    Ok(LinkPlan::StaticArchive {
        output_name,
        ar_flags,
        extra_members,
    })
}

fn target_obj_dir_rel_from_link_txt(
    link_txt_path: &Path,
    build_dir: &Path,
) -> Result<String, String> {
    let parent = link_txt_path
        .parent()
        .ok_or_else(|| format!("invalid link.txt path {}", link_txt_path.display()))?;
    let rel = parent.strip_prefix(build_dir).map_err(|_| {
        format!(
            "link.txt '{}' is not under build dir '{}'",
            link_txt_path.display(),
            build_dir.display()
        )
    })?;
    Ok(normalize_path(rel))
}

fn infer_link_work_dir(link_txt_path: &Path, build_dir: &Path) -> PathBuf {
    let Some(target_dir) = link_txt_path.parent() else {
        return build_dir.to_path_buf();
    };
    let Some(cmakefiles_dir) = target_dir.parent() else {
        return build_dir.to_path_buf();
    };
    if cmakefiles_dir
        .file_name()
        .and_then(|s| s.to_str())
        .is_some_and(|s| s == "CMakeFiles")
    {
        if let Some(work_dir) = cmakefiles_dir.parent() {
            return work_dir.to_path_buf();
        }
    }
    build_dir.to_path_buf()
}

fn link_target_id(link_txt_path: &Path, build_dir: &Path) -> Option<String> {
    let rel = link_txt_path.strip_prefix(build_dir).ok()?;
    let mut value = normalize_path(rel);
    if !value.ends_with("/link.txt") {
        return None;
    }
    value.truncate(value.len() - "/link.txt".len());
    if value.ends_with(".dir") {
        value.truncate(value.len() - ".dir".len());
    }
    Some(value)
}

fn is_ar_tool(token: &str) -> bool {
    let base = command_basename(token);
    base == "ar" || base.ends_with("-ar") || base.contains("llvm-ar") || base.contains("gcc-ar")
}

fn command_basename(token: &str) -> String {
    Path::new(token)
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_else(|| token.to_ascii_lowercase())
}

fn is_shared_library_name(name: &str) -> bool {
    name.ends_with(".so") || name.contains(".so.") || name.ends_with(".dylib")
}

fn output_name_from_token(token: &str, fallback: &str) -> String {
    Path::new(token)
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_string)
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| fallback.to_string())
}

fn token_looks_like_path(token: &str) -> bool {
    token.contains('/')
        || token.contains('\\')
        || token.ends_with(".a")
        || token.ends_with(".so")
        || token.ends_with(".dylib")
        || token.ends_with(".o")
}

fn absolutize_link_token(token: &str, link_work_dir: &Path, force_path: bool) -> String {
    if token.is_empty() {
        return token.to_string();
    }
    if !force_path {
        if token.starts_with('-') {
            return token.to_string();
        }
        if !token_looks_like_path(token) {
            return token.to_string();
        }
    }

    let path = PathBuf::from(token);
    if path.is_absolute() {
        token.to_string()
    } else {
        normalize_path(&resolve_path(link_work_dir, token))
    }
}

fn output_rel_to_build(output: &str, build_dir: &Path) -> String {
    let output_path = PathBuf::from(output);
    if output_path.is_absolute() {
        if let Ok(rel) = output_path.strip_prefix(build_dir) {
            return normalize_path(rel);
        }
        return normalize_path(&output_path);
    }
    normalize_rel_token(output)
}

fn normalize_rel_token(value: &str) -> String {
    value.replace('\\', "/")
}

fn resolve_path(base: &Path, value: &str) -> PathBuf {
    let candidate = PathBuf::from(value);
    if candidate.is_absolute() {
        candidate
    } else {
        base.join(candidate)
    }
}

fn rel_or_abs(repo_root: &Path, path: &Path) -> String {
    if let Ok(rel) = path.strip_prefix(repo_root) {
        normalize_path(rel)
    } else {
        normalize_path(path)
    }
}

fn rewrite_token_for_spec(token: &str, repo_root: &Path) -> String {
    let marker = "${REPO_ROOT}";
    let root = normalize_path(repo_root);
    if token.contains(&root) {
        token.replace(&root, marker)
    } else {
        token.to_string()
    }
}

fn expand_token_from_spec(token: &str, repo_root: &Path) -> String {
    token.replace("${REPO_ROOT}", &normalize_path(repo_root))
}

fn normalize_path(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

fn canonical_or_self(path: PathBuf) -> PathBuf {
    path.canonicalize().unwrap_or(path)
}

fn paths_equivalent(a: &Path, b: &Path) -> bool {
    canonical_or_self(a.to_path_buf()) == canonical_or_self(b.to_path_buf())
}

fn repo_root() -> Result<PathBuf, String> {
    let manifest_dir = PathBuf::from(
        env::var("CARGO_MANIFEST_DIR").map_err(|e| format!("missing CARGO_MANIFEST_DIR: {e}"))?,
    );
    manifest_dir.parent().map(Path::to_path_buf).ok_or_else(|| {
        format!(
            "failed to resolve repo root from {}",
            manifest_dir.display()
        )
    })
}

fn absolutize(repo_root: &Path, path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        repo_root.join(path)
    }
}

fn render_command(cmd: &Command) -> String {
    let mut parts = vec![cmd.get_program().to_string_lossy().to_string()];
    parts.extend(cmd.get_args().map(shell_escape));
    parts.join(" ")
}

fn shell_escape(value: &OsStr) -> String {
    let s = value.to_string_lossy();
    if s.chars()
        .all(|ch| ch.is_ascii_alphanumeric() || "-._/:=+".contains(ch))
    {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

fn run_checked(mut cmd: Command, stage: &str) -> Result<(), String> {
    println!("+ {}", render_command(&cmd));
    let output = cmd
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("{stage}: failed to spawn command: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "{stage}: command failed with status {}\nstdout:\n{}\nstderr:\n{}",
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    Ok(())
}

fn print_help() {
    println!(
        "mako-native-build: CMake-free Cargo-native target builder\n\n\
Usage:\n\
  cargo mako-native help\n\
  cargo mako-native export-spec --build-dir <cmake-build-dir> [--out cargo-native/targets.toml] [--all-link-targets] [--target <name> ...]\n\
  cargo mako-native list [--spec cargo-native/targets.toml]\n\
  cargo mako-native build [--spec cargo-native/targets.toml] [--out-dir target/native-build] (--target <name> | --all)\n\n\
Notes:\n\
  - `export-spec` reads compile/link metadata once from an existing build tree.\n\
  - `build` uses only the checked-in spec and does not invoke CMake.\n\
  - default export slice is: rrr, memdb, rpcbench.\n\n\
Env:\n\
  MAKO_NATIVE_AR       override archiver (default: llvm-ar then ar)\n\
  RUSTC_BIN            override rustc binary used for rust-lld link stage\n\
  RUST_LLD_NATIVE_DIRS colon-separated extra native lib search dirs\n\
  RUST_LLD_GCC_CRT_DIR optional gcc crt dir containing crtbeginS.o/crtendS.o\n\
  RUST_LLD_DYNAMIC_LINKER optional dynamic linker path for executable links\n"
    );
}
