use serde::Deserialize;
use std::collections::{BTreeMap, HashSet};
use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

const DEFAULT_MAKO_BUILD_DIR_REL: &str = "build";
const DEFAULT_MAKO_CMAKE_TARGET: &str = "test_rpc";

#[derive(Debug, Deserialize)]
struct CompileCommandEntry {
    directory: Option<String>,
    command: Option<String>,
    arguments: Option<Vec<String>>,
    file: String,
    output: Option<String>,
}

#[derive(Debug)]
struct CompileUnit {
    source: PathBuf,
    object_token: String,
    object_abs: PathBuf,
    object_rel: String,
    flags: Vec<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ArtifactKind {
    Executable,
    SharedLibrary,
    StaticLibrary,
}

#[derive(Debug)]
struct StaticArchivePlan {
    ar_bin: String,
    ar_flags: Vec<String>,
    extra_members: Vec<PathBuf>,
    ranlib_bin: Option<String>,
}

#[derive(Debug)]
enum LinkPlan {
    RustLld {
        link_args: Vec<String>,
    },
    StaticArchive(StaticArchivePlan),
}

#[derive(Debug)]
struct TargetSpec {
    target_name: String,
    target_obj_dir_rel: String,
    artifact_kind: ArtifactKind,
    output_name: String,
    output_token: String,
    output_path: PathBuf,
    link_work_dir: PathBuf,
    compile_units: Vec<CompileUnit>,
    link_plan: LinkPlan,
    compile_commands_path: PathBuf,
    link_txt_path: PathBuf,
}

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
    let mut flattened: Vec<String> = Vec::new();
    for arg in args {
        if arg.starts_with("-Wl,") {
            flattened.extend(expand_wl_forwarded_args(arg));
        } else {
            flattened.push(arg.to_string());
        }
    }

    let mut normalized: Vec<String> = Vec::new();
    let mut idx = 0usize;
    while idx < flattened.len() {
        let arg = flattened[idx].as_str();
        if arg.is_empty() {
            idx += 1;
            continue;
        }

        if arg == "-Xlinker" {
            if idx + 1 < flattened.len() {
                normalized.push(flattened[idx + 1].clone());
                idx += 2;
                continue;
            }
            idx += 1;
            continue;
        }
        if let Some(linker_arg) = arg.strip_prefix("-Xlinker=") {
            if !linker_arg.is_empty() {
                normalized.push(linker_arg.to_string());
            }
            idx += 1;
            continue;
        }

        if arg == "-rdynamic" {
            normalized.push("--export-dynamic".to_string());
            idx += 1;
            continue;
        }
        if arg == "-pthread" {
            normalized.push("-lpthread".to_string());
            idx += 1;
            continue;
        }

        if arg.starts_with("--dependency-file=") {
            idx += 1;
            continue;
        }
        if arg == "--dependency-file" {
            idx += if idx + 1 < flattened.len() { 2 } else { 1 };
            continue;
        }

        if matches!(
            arg,
            "-I" | "-isystem" | "-iquote" | "-include" | "-imacros" | "-MF" | "-MT" | "-MQ" | "-x"
        ) {
            idx += if idx + 1 < flattened.len() { 2 } else { 1 };
            continue;
        }

        let is_compiler_only = arg.starts_with("-D")
            || arg.starts_with("-U")
            || arg.starts_with("-I")
            || arg.starts_with("-isystem")
            || arg.starts_with("-iquote")
            || arg.starts_with("-include")
            || arg.starts_with("-imacros")
            || arg.starts_with("-std=")
            || arg.starts_with("-stdlib=")
            || arg.starts_with("-march=")
            || arg.starts_with("-mtune=")
            || arg.starts_with("-mcpu=")
            || arg.starts_with("-mfpmath=")
            || arg.starts_with("-f")
            || (arg.starts_with("-W") && !arg.starts_with("--"))
            || arg == "-c"
            || arg == "-S"
            || arg == "-E"
            || arg == "-pipe"
            || arg == "-ansi"
            || arg == "-pedantic"
            || arg == "-g"
            || (arg.starts_with("-O") && arg.len() > 1);
        if is_compiler_only {
            idx += 1;
            continue;
        }

        normalized.push(arg.to_string());
        idx += 1;
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

fn canonical_or_self(path: PathBuf) -> PathBuf {
    path.canonicalize().unwrap_or(path)
}

fn resolve_path(base: &Path, value: &str) -> PathBuf {
    let candidate = PathBuf::from(value);
    if candidate.is_absolute() {
        candidate
    } else {
        base.join(candidate)
    }
}

fn normalize_path(value: &Path) -> String {
    value.to_string_lossy().replace('\\', "/")
}

fn normalize_rel_token(value: &str) -> String {
    value.replace('\\', "/")
}

fn command_basename(token: &str) -> String {
    Path::new(token)
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_else(|| token.to_ascii_lowercase())
}

fn is_ar_tool(token: &str) -> bool {
    let base = command_basename(token);
    base == "ar" || base.ends_with("-ar") || base.contains("llvm-ar") || base.contains("gcc-ar")
}

fn is_ranlib_tool(token: &str) -> bool {
    let base = command_basename(token);
    base == "ranlib"
        || base.ends_with("-ranlib")
        || base.contains("llvm-ranlib")
        || base.contains("gcc-ranlib")
}

fn is_shared_library_name(name: &str) -> bool {
    name.ends_with(".so") || name.contains(".so.") || name.ends_with(".dylib")
}

fn paths_equivalent(a: &Path, b: &Path) -> bool {
    canonical_or_self(a.to_path_buf()) == canonical_or_self(b.to_path_buf())
}

fn parse_command_tokens(entry: &CompileCommandEntry) -> Result<Vec<String>, String> {
    if let Some(arguments) = &entry.arguments {
        if !arguments.is_empty() {
            return Ok(arguments.clone());
        }
    }

    if let Some(command) = &entry.command {
        if !command.trim().is_empty() {
            return shell_words::split(command)
                .map_err(|e| format!("failed to split compile command '{}': {e}", command));
        }
    }

    Err(format!(
        "compile_commands entry missing command/arguments for source '{}'",
        entry.file
    ))
}

fn is_same_source_token(token: &str, source_abs: &Path, compile_dir: &Path) -> bool {
    if token.starts_with('-') {
        return false;
    }
    let token_abs = resolve_path(compile_dir, token);
    paths_equivalent(&token_abs, source_abs)
}

fn sanitize_compile_flags(tokens: &[String], source_abs: &Path, compile_dir: &Path) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut i = 1usize; // skip compiler binary argv[0]

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

fn sanitize_rel_for_filename(value: &str) -> String {
    value
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || matches!(ch, '.' | '_' | '-') {
                ch
            } else {
                '_'
            }
        })
        .collect()
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
            "no compile_commands entries matched target '{}' with object-prefix '{}'",
            target_name, prefix
        ));
    }

    let mut units = Vec::with_capacity(selected.len());
    for (idx, (output_rel, entry)) in selected.into_iter().enumerate() {
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
            .expect("selected entries must include output")
            .clone();
        let object_abs = canonical_or_self(resolve_path(build_dir, &object_token));
        let object_rel = format!("obj/{idx:03}_{}", sanitize_rel_for_filename(&output_rel));

        units.push(CompileUnit {
            source: source_abs,
            object_token,
            object_abs,
            object_rel,
            flags,
        });
    }

    Ok(units)
}

fn parse_link_commands(link_txt_path: &Path) -> Result<Vec<Vec<String>>, String> {
    let raw = fs::read_to_string(link_txt_path)
        .map_err(|e| format!("failed to read {}: {e}", link_txt_path.display()))?;
    let mut commands = Vec::new();

    for (idx, line) in raw.lines().enumerate() {
        let command = line.trim();
        if command.is_empty() {
            continue;
        }
        let tokens = shell_words::split(command).map_err(|e| {
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

fn output_name_from_token(token: &str, fallback: &str) -> String {
    Path::new(token)
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_string)
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| fallback.to_string())
}

fn collect_link_txt_files_recursive(root: &Path, out: &mut Vec<PathBuf>) -> io::Result<()> {
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_link_txt_files_recursive(&path, out)?;
            continue;
        }
        if path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| name == "link.txt")
        {
            out.push(path);
        }
    }
    Ok(())
}

fn infer_target_name_from_link_txt(link_txt_path: &Path) -> String {
    link_txt_path
        .parent()
        .and_then(|p| p.file_name())
        .and_then(|s| s.to_str())
        .map(|s| s.strip_suffix(".dir").unwrap_or(s).to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| DEFAULT_MAKO_CMAKE_TARGET.to_string())
}

fn target_obj_dir_rel_from_link_txt(link_txt_path: &Path, build_dir: &Path) -> Result<String, String> {
    let parent = link_txt_path
        .parent()
        .ok_or_else(|| format!("invalid link.txt path (no parent): {}", link_txt_path.display()))?;
    let rel = parent.strip_prefix(build_dir).map_err(|_| {
        format!(
            "link.txt '{}' is not under MAKO_BUILD_DIR '{}'",
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

fn link_txt_id(path: &Path, build_dir: &Path) -> Option<String> {
    let rel = path.strip_prefix(build_dir).ok()?;
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

fn resolve_link_txt_path(build_dir: &Path, target_query: &str) -> Result<PathBuf, String> {
    if let Ok(explicit) = env::var("MAKO_CMAKE_LINK_TXT") {
        let explicit = explicit.trim();
        if explicit.is_empty() {
            return Err("MAKO_CMAKE_LINK_TXT was set but empty".to_string());
        }

        let base = resolve_path(build_dir, explicit);
        let candidates = vec![
            base.clone(),
            base.join("link.txt"),
            PathBuf::from(format!("{}.dir/link.txt", base.display())),
        ];
        if let Some(found) = first_existing_path(&candidates) {
            return Ok(canonical_or_self(found));
        }

        return Err(format!(
            "MAKO_CMAKE_LINK_TXT does not point to a readable link.txt under MAKO_BUILD_DIR:\n{}",
            candidates
                .iter()
                .map(|p| format!("  - {}", p.display()))
                .collect::<Vec<String>>()
                .join("\n")
        ));
    }

    let query = target_query.trim().trim_end_matches('/');
    let query_no_dir = query.strip_suffix(".dir").unwrap_or(query);
    let query_norm = normalize_rel_token(query_no_dir);

    let direct_candidates = vec![
        build_dir
            .join("CMakeFiles")
            .join(format!("{query_no_dir}.dir"))
            .join("link.txt"),
        build_dir.join(format!("{query_no_dir}.dir")).join("link.txt"),
        build_dir.join(query_no_dir).join("link.txt"),
    ];
    if let Some(found) = first_existing_path(&direct_candidates) {
        return Ok(canonical_or_self(found));
    }

    let mut all = Vec::new();
    collect_link_txt_files_recursive(build_dir, &mut all)
        .map_err(|e| format!("failed to search for link.txt under {}: {e}", build_dir.display()))?;

    let mut matches = Vec::new();
    for link_txt in all {
        let Some(parent_dir_name) = link_txt
            .parent()
            .and_then(|p| p.file_name())
            .and_then(|s| s.to_str())
        else {
            continue;
        };
        let parent_target = parent_dir_name.strip_suffix(".dir").unwrap_or(parent_dir_name);

        let Some(link_id) = link_txt_id(&link_txt, build_dir) else {
            continue;
        };

        if query_norm.contains('/') {
            if link_id == query_norm || link_id.ends_with(&format!("/{query_norm}")) {
                matches.push(link_txt);
            }
        } else if parent_target == query_norm {
            matches.push(link_txt);
        }
    }

    match matches.len() {
        0 => Err(format!(
            "could not find link.txt for target query '{}' under {}\nset MAKO_CMAKE_LINK_TXT explicitly for nested/ambiguous targets",
            target_query,
            build_dir.display()
        )),
        1 => Ok(canonical_or_self(matches.remove(0))),
        _ => {
            let sample = matches
                .iter()
                .take(8)
                .map(|p| format!("  - {}", p.display()))
                .collect::<Vec<String>>()
                .join("\n");
            Err(format!(
                "target query '{}' matched multiple link.txt files under {}\nset MAKO_CMAKE_LINK_TXT to disambiguate.\n{}",
                target_query,
                build_dir.display(),
                sample
            ))
        }
    }
}

fn token_looks_like_path(token: &str) -> bool {
    token.contains('/')
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

    let candidate = PathBuf::from(token);
    if candidate.is_absolute() {
        return token.to_string();
    }

    normalize_path(&resolve_path(link_work_dir, token))
}

fn extract_link_spec(
    target_name: &str,
    link_tokens: &[String],
    compile_units: &[CompileUnit],
    link_work_dir: &Path,
) -> (String, String, Vec<String>) {
    let object_tokens: HashSet<String> = compile_units
        .iter()
        .map(|unit| unit.object_token.clone())
        .collect();
    let object_abs: HashSet<PathBuf> = compile_units
        .iter()
        .map(|unit| unit.object_abs.clone())
        .collect();

    let mut output_token = target_name.to_string();
    let mut output_name = target_name.to_string();
    let mut link_args: Vec<String> = Vec::new();

    let mut i = 1usize; // skip linker driver argv[0]
    while i < link_tokens.len() {
        let tok = &link_tokens[i];

        if tok == "-o" && i + 1 < link_tokens.len() {
            output_token = link_tokens[i + 1].clone();
            output_name = output_name_from_token(&output_token, target_name);
            i += 2;
            continue;
        }

        if let Some(rest) = tok.strip_prefix("-o") {
            if !rest.is_empty() {
                output_token = rest.to_string();
                output_name = output_name_from_token(rest, target_name);
                i += 1;
                continue;
            }
        }

        if tok.starts_with("-Wl,--dependency-file=") {
            i += 1;
            continue;
        }

        if object_tokens.contains(tok) {
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
            link_args.push(absolutize_link_token(&link_tokens[i + 1], link_work_dir, true));
            i += 2;
            continue;
        }

        if let Some(dir_token) = tok.strip_prefix("-L") {
            if !dir_token.is_empty() {
                let resolved = absolutize_link_token(dir_token, link_work_dir, true);
                link_args.push(format!("-L{resolved}"));
                i += 1;
                continue;
            }
        }

        link_args.push(absolutize_link_token(tok, link_work_dir, false));
        i += 1;
    }

    (output_token, output_name, link_args)
}

fn parse_archive_plan(
    target_name: &str,
    link_tokens: &[String],
    trailing_commands: &[Vec<String>],
    compile_units: &[CompileUnit],
    link_work_dir: &Path,
) -> Result<(String, String, StaticArchivePlan), String> {
    if link_tokens.is_empty() {
        return Err("archive link command is empty".to_string());
    }

    let ar_bin = link_tokens[0].clone();
    let mut idx = 1usize;
    let mut ar_flags: Vec<String> = Vec::new();

    while idx < link_tokens.len() {
        let tok = &link_tokens[idx];
        let looks_like_ar_flags =
            tok.starts_with('-') || (idx == 1 && tok.chars().all(|ch| ch.is_ascii_alphabetic()));
        if !looks_like_ar_flags {
            break;
        }
        ar_flags.push(tok.clone());
        idx += 1;
    }

    if idx >= link_tokens.len() {
        return Err(format!(
            "archive target '{}' has invalid link command (missing archive output token)",
            target_name
        ));
    }

    let output_token = link_tokens[idx].clone();
    let output_name = output_name_from_token(&output_token, target_name);
    idx += 1;

    let local_object_tokens: HashSet<String> = compile_units
        .iter()
        .map(|unit| unit.object_token.clone())
        .collect();
    let local_object_abs: HashSet<PathBuf> = compile_units
        .iter()
        .map(|unit| unit.object_abs.clone())
        .collect();

    let mut extra_members: Vec<PathBuf> = Vec::new();
    for tok in &link_tokens[idx..] {
        if !tok.ends_with(".o") && !tok.ends_with(".obj") {
            continue;
        }
        if local_object_tokens.contains(tok) {
            continue;
        }
        let tok_abs = canonical_or_self(resolve_path(link_work_dir, tok));
        if local_object_abs.contains(&tok_abs) {
            continue;
        }
        if !extra_members.iter().any(|existing| existing == &tok_abs) {
            extra_members.push(tok_abs);
        }
    }

    let ranlib_bin = trailing_commands
        .iter()
        .filter_map(|command| command.first())
        .find(|tool| is_ranlib_tool(tool))
        .cloned();

    Ok((
        output_token,
        output_name,
        StaticArchivePlan {
            ar_bin,
            ar_flags,
            extra_members,
            ranlib_bin,
        },
    ))
}

fn load_target_spec(build_dir: &Path, target_query: &str) -> Result<TargetSpec, String> {
    let compile_commands_path = build_dir.join("compile_commands.json");
    if !compile_commands_path.is_file() {
        return Err(format!(
            "missing compile_commands.json at {} (configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)",
            compile_commands_path.display()
        ));
    }

    let link_txt_path = resolve_link_txt_path(build_dir, target_query)?;
    let target_name = infer_target_name_from_link_txt(&link_txt_path);
    let target_obj_dir_rel = target_obj_dir_rel_from_link_txt(&link_txt_path, build_dir)?;
    let link_work_dir = infer_link_work_dir(&link_txt_path, build_dir);

    let compile_commands_raw = fs::read_to_string(&compile_commands_path)
        .map_err(|e| format!("failed to read {}: {e}", compile_commands_path.display()))?;
    let entries: Vec<CompileCommandEntry> = serde_json::from_str(&compile_commands_raw).map_err(
        |e| {
            format!(
                "failed to parse compile_commands.json at {}: {e}",
                compile_commands_path.display()
            )
        },
    )?;

    let compile_units = collect_target_compile_units(
        &entries,
        build_dir,
        &target_name,
        &target_obj_dir_rel,
    )?;
    let link_commands = parse_link_commands(&link_txt_path)?;
    let link_tokens = &link_commands[0];
    let (artifact_kind, output_token, output_name, link_plan) = if is_ar_tool(&link_tokens[0]) {
        let (output_token, output_name, archive_plan) = parse_archive_plan(
            &target_name,
            link_tokens,
            &link_commands[1..],
            &compile_units,
            &link_work_dir,
        )?;
        (
            ArtifactKind::StaticLibrary,
            output_token,
            output_name,
            LinkPlan::StaticArchive(archive_plan),
        )
    } else {
        let (output_token, output_name, link_args) =
            extract_link_spec(&target_name, link_tokens, &compile_units, &link_work_dir);
        let kind = if output_name.ends_with(".a") {
            ArtifactKind::StaticLibrary
        } else if is_shared_library_name(&output_name) {
            ArtifactKind::SharedLibrary
        } else {
            ArtifactKind::Executable
        };
        (
            kind,
            output_token,
            output_name,
            LinkPlan::RustLld { link_args },
        )
    };
    let output_path = canonical_or_self(resolve_path(&link_work_dir, &output_token));

    Ok(TargetSpec {
        target_name,
        target_obj_dir_rel,
        artifact_kind,
        output_name,
        output_token,
        output_path,
        link_work_dir,
        compile_units,
        link_plan,
        compile_commands_path,
        link_txt_path,
    })
}

fn env_flag(name: &str, default_value: bool) -> bool {
    match env::var(name) {
        Ok(value) => {
            let lowered = value.trim().to_ascii_lowercase();
            match lowered.as_str() {
                "" => default_value,
                "0" | "false" | "no" | "off" => false,
                _ => true,
            }
        }
        Err(_) => default_value,
    }
}

fn bridge_compile_jobs(unit_count: usize) -> usize {
    if unit_count <= 1 {
        return 1;
    }

    let from_env = env::var("MAKO_BRIDGE_JOBS")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .filter(|jobs| *jobs > 0);
    let detected = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    let jobs = from_env.unwrap_or(detected);
    jobs.clamp(1, unit_count)
}

fn bridge_compile_thread_stack_size() -> usize {
    let mb = env::var("MAKO_BRIDGE_THREAD_STACK_MB")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .filter(|size| *size > 0)
        .unwrap_or(32);
    mb * 1024 * 1024
}

fn object_defines_global_main_symbol(obj_path: &Path) -> bool {
    let output = match Command::new("nm").arg("-g").arg(obj_path).output() {
        Ok(output) => output,
        Err(_) => return false,
    };
    if !output.status.success() {
        return false;
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    for line in stdout.lines() {
        let cols: Vec<&str> = line.split_whitespace().collect();
        if cols.len() < 2 {
            continue;
        }
        if cols[cols.len() - 1] != "main" {
            continue;
        }
        let sym_type = cols[cols.len() - 2];
        if sym_type != "U" {
            return true;
        }
    }
    false
}

fn compile_units_define_global_main(obj_root: &Path, compile_units: &[CompileUnit]) -> bool {
    for unit in compile_units {
        let obj_path = obj_root.join(&unit.object_rel);
        if obj_path.is_file() && object_defines_global_main_symbol(&obj_path) {
            return true;
        }
    }
    false
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
    println!("cargo:rerun-if-env-changed=MAKO_CMAKE_TARGET");
    println!("cargo:rerun-if-env-changed=MAKO_CMAKE_LINK_TXT");
    println!("cargo:rerun-if-env-changed=MAKO_CMAKE_STAGE_TO_BUILD");
    println!("cargo:rerun-if-env-changed=MAKO_CMAKE_STAGE_LIBS");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=README.md");

    let manifest_dir =
        PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR must be set"));
    let repo_root = if manifest_dir.join("CMakeLists.txt").is_file() {
        // Root package mode: Cargo manifest lives at repo root.
        manifest_dir.clone()
    } else {
        // Subcrate mode: Cargo manifest lives under repo root.
        let parent = manifest_dir
            .parent()
            .expect("cargo-mako-cmake-bridge must live under repo root");
        if parent.join("CMakeLists.txt").is_file() {
            parent.to_path_buf()
        } else {
            panic!(
                "failed to locate repo root from CARGO_MANIFEST_DIR={}",
                manifest_dir.display()
            );
        }
    };

    let build_dir = env::var("MAKO_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root.join(DEFAULT_MAKO_BUILD_DIR_REL));
    let target_query = env::var("MAKO_CMAKE_TARGET")
        .ok()
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| DEFAULT_MAKO_CMAKE_TARGET.to_string());

    let spec = load_target_spec(&build_dir, &target_query)
        .unwrap_or_else(|err| panic!("failed to load CMake target spec: {err}"));

    println!("cargo:rerun-if-changed={}", spec.compile_commands_path.display());
    println!("cargo:rerun-if-changed={}", spec.link_txt_path.display());
    for unit in &spec.compile_units {
        println!("cargo:rerun-if-changed={}", unit.source.display());
    }

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR must be set"));
    let obj_root = out_dir.join("mako_obj");
    let artifact_root = out_dir.join("mako_artifact");
    let linked_artifact = artifact_root.join(&spec.output_name);

    if let Err(err) = fs::create_dir_all(&obj_root) {
        panic!("failed to create object root {}: {err}", obj_root.display());
    }
    if let Err(err) = fs::create_dir_all(&artifact_root) {
        panic!(
            "failed to create artifact root {}: {err}",
            artifact_root.display()
        );
    }

    for unit in &spec.compile_units {
        let out_obj = obj_root.join(&unit.object_rel);
        if let Some(parent) = out_obj.parent() {
            if let Err(err) = fs::create_dir_all(parent) {
                panic!("failed to create object parent {}: {err}", parent.display());
            }
        }
    }

    let compile_jobs = bridge_compile_jobs(spec.compile_units.len());
    let reuse_existing_objects = env_flag("MAKO_BRIDGE_REUSE_OBJECTS", true);
    if compile_jobs <= 1 {
        for unit in &spec.compile_units {
            let out_obj = obj_root.join(&unit.object_rel);
            if reuse_existing_objects && out_obj.is_file() {
                continue;
            }
            let flag_refs: Vec<&str> = unit.flags.iter().map(String::as_str).collect();
            if let Err(err) = fragile_driver::compile_unit_with_flags_in_dir(
                &unit.source,
                &out_obj,
                &flag_refs,
                &build_dir,
            ) {
                panic!(
                    "mako cargo bridge compile failed via fragile-driver for target '{}' unit '{}':\n{}",
                    spec.target_name,
                    unit.source.display(),
                    err
                );
            }
        }
    } else {
        let first_error: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
        let next_idx = AtomicUsize::new(0);
        let compile_units = &spec.compile_units;
        let target_name = &spec.target_name;
        let build_dir_ref = &build_dir;
        let obj_root_ref = &obj_root;
        let thread_stack_size = bridge_compile_thread_stack_size();

        std::thread::scope(|scope| {
            for _ in 0..compile_jobs {
                let first_error = Arc::clone(&first_error);
                let next_idx = &next_idx;
                let compile_units = compile_units;
                let target_name = target_name;
                let build_dir = build_dir_ref;
                let obj_root = obj_root_ref;
                let reuse_existing_objects = reuse_existing_objects;
                let builder = std::thread::Builder::new().stack_size(thread_stack_size);
                builder
                    .spawn_scoped(scope, move || loop {
                        if first_error.lock().expect("compile error mutex poisoned").is_some() {
                            break;
                        }

                        let idx = next_idx.fetch_add(1, Ordering::Relaxed);
                        if idx >= compile_units.len() {
                            break;
                        }
                        let unit = &compile_units[idx];
                        let out_obj = obj_root.join(&unit.object_rel);
                        if reuse_existing_objects && out_obj.is_file() {
                            continue;
                        }
                        let flag_refs: Vec<&str> = unit.flags.iter().map(String::as_str).collect();
                        if let Err(err) = fragile_driver::compile_unit_with_flags_in_dir(
                            &unit.source,
                            &out_obj,
                            &flag_refs,
                            build_dir,
                        ) {
                            let mut slot = first_error.lock().expect("compile error mutex poisoned");
                            if slot.is_none() {
                                *slot = Some(format!(
                                    "mako cargo bridge compile failed via fragile-driver for target '{}' unit '{}':\n{}",
                                    target_name,
                                    unit.source.display(),
                                    err
                                ));
                            }
                            break;
                        }
                    })
                    .expect("failed to spawn bridge compile worker");
            }
        });

        let compile_error = {
            let mut guard = first_error.lock().expect("compile error mutex poisoned");
            guard.take()
        };
        if let Some(err) = compile_error {
            panic!("{err}");
        }
    }

    match &spec.link_plan {
        LinkPlan::RustLld { link_args } => {
            let rustc = env::var("RUSTC_BIN").unwrap_or_else(|_| "rustc".to_string());
            let link_src = out_dir.join("mako_bridge_link.rs");
            if let Err(err) = fs::write(&link_src, "#![no_main]\n") {
                panic!(
                    "failed to write bridge link source {}: {err}",
                    link_src.display()
                );
            }

            let mut all_link_args: Vec<String> = Vec::new();
            all_link_args.push(format!("-Wl,-rpath,{}", build_dir.display()));
            all_link_args.push(format!("-Wl,-rpath,{}", build_dir.join("lib").display()));
            all_link_args.extend(link_args.iter().cloned());
            let normalized_link_args =
                normalize_link_args_for_rust_lld(all_link_args.iter().map(String::as_str));

            let mut gtest_main_stub_obj: Option<PathBuf> = None;
            if spec.artifact_kind == ArtifactKind::Executable {
                let references_gtest_main = normalized_link_args
                    .iter()
                    .any(|arg| arg.contains("gtest_main"));
                let has_main = compile_units_define_global_main(&obj_root, &spec.compile_units);
                if references_gtest_main && !has_main {
                    let stub_src = out_dir.join("mako_gtest_main_stub.cc");
                    let stub_obj = obj_root.join("gtest_main_stub.o");
                    if let Err(err) = fs::write(
                        &stub_src,
                        "#include <gtest/gtest.h>\nint main(int argc, char** argv) {\n  ::testing::InitGoogleTest(&argc, argv);\n  return RUN_ALL_TESTS();\n}\n",
                    ) {
                        panic!(
                            "failed to write gtest main stub source {}: {err}",
                            stub_src.display()
                        );
                    }
                    if !reuse_existing_objects || !stub_obj.is_file() {
                        let stub_flag_refs: Vec<&str> = spec
                            .compile_units
                            .first()
                            .map(|unit| unit.flags.iter().map(String::as_str).collect())
                            .unwrap_or_default();
                        if let Err(err) = fragile_driver::compile_unit_with_flags_in_dir(
                            &stub_src,
                            &stub_obj,
                            &stub_flag_refs,
                            &build_dir,
                        ) {
                            panic!(
                                "failed to compile gtest main stub for target '{}' (query '{}'): {}",
                                spec.target_name, target_query, err
                            );
                        }
                    }
                    gtest_main_stub_obj = Some(stub_obj);
                }
            }

            let mut link_cmd = Command::new(&rustc);
            link_cmd.current_dir(&spec.link_work_dir);
            link_cmd.arg("--edition");
            link_cmd.arg("2021");
            if spec.artifact_kind == ArtifactKind::SharedLibrary {
                link_cmd.arg("--crate-type");
                link_cmd.arg("dylib");
                // Preserve externally-consumed symbols (e.g., gtest_main's `main`) in shared libs.
                link_cmd.arg("-C");
                link_cmd.arg("link-dead-code");
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
            link_cmd.arg(&linked_artifact);

            if spec.artifact_kind == ArtifactKind::Executable {
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

                for unit in &spec.compile_units {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={}", obj_root.join(&unit.object_rel).display()));
                }
                if let Some(stub_obj) = &gtest_main_stub_obj {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={}", stub_obj.display()));
                }

                for arg in &normalized_link_args {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={arg}"));
                }

                for c_lib in ["-lgcc_s", "-lutil", "-lrt", "-lpthread", "-lm", "-ldl", "-lc"] {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={c_lib}"));
                }

                for crt in &crt_suffix {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={}", crt.display()));
                }
            } else {
                for unit in &spec.compile_units {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={}", obj_root.join(&unit.object_rel).display()));
                }

                for arg in &normalized_link_args {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={arg}"));
                }

                for c_lib in ["-lgcc_s", "-lutil", "-lrt", "-lpthread", "-lm", "-ldl", "-lc"] {
                    link_cmd.arg("-C");
                    link_cmd.arg(format!("link-arg={c_lib}"));
                }
            }

            if let Err(err) = run_checked(link_cmd, "link") {
                panic!(
                    "mako cargo bridge rust-lld link failed for target '{}' (query '{}'):\n{}",
                    spec.target_name, target_query, err
                );
            }
        }
        LinkPlan::StaticArchive(plan) => {
            if let Some(parent) = linked_artifact.parent() {
                if let Err(err) = fs::create_dir_all(parent) {
                    panic!(
                        "failed to create archive artifact parent {}: {err}",
                        parent.display()
                    );
                }
            }

            let mut archive_cmd = Command::new(&plan.ar_bin);
            archive_cmd.current_dir(&spec.link_work_dir);
            if plan.ar_flags.is_empty() {
                archive_cmd.arg("qc");
            } else {
                archive_cmd.args(&plan.ar_flags);
            }
            archive_cmd.arg(&linked_artifact);
            for unit in &spec.compile_units {
                archive_cmd.arg(obj_root.join(&unit.object_rel));
            }
            for extra in &plan.extra_members {
                archive_cmd.arg(extra);
            }
            if let Err(err) = run_checked(archive_cmd, "archive") {
                panic!(
                    "mako cargo bridge archive step failed for target '{}' (query '{}'):\n{}",
                    spec.target_name, target_query, err
                );
            }

            if let Some(ranlib_bin) = &plan.ranlib_bin {
                let mut ranlib_cmd = Command::new(ranlib_bin);
                ranlib_cmd.current_dir(&spec.link_work_dir);
                ranlib_cmd.arg(&linked_artifact);
                if let Err(err) = run_checked(ranlib_cmd, "ranlib") {
                    panic!(
                        "mako cargo bridge ranlib step failed for target '{}' (query '{}'):\n{}",
                        spec.target_name, target_query, err
                    );
                }
            }
        }
    }

    let dist_dir = manifest_dir.join("dist");
    if let Err(err) = fs::create_dir_all(&dist_dir) {
        panic!("failed to create dist dir {}: {err}", dist_dir.display());
    }
    let dist_bin = dist_dir.join(&spec.output_name);
    if let Err(err) = fs::copy(&linked_artifact, &dist_bin) {
        panic!(
            "failed to copy linked binary from {} to {}: {err}",
            linked_artifact.display(),
            dist_bin.display()
        );
    }

    let stage_libs = env_flag("MAKO_CMAKE_STAGE_LIBS", false);
    let should_stage = env_flag("MAKO_CMAKE_STAGE_TO_BUILD", true)
        && (spec.artifact_kind == ArtifactKind::Executable || stage_libs);
    if should_stage {
        if let Some(parent) = spec.output_path.parent() {
            if let Err(err) = fs::create_dir_all(parent) {
                panic!(
                    "failed to create build output parent {}: {err}",
                    parent.display()
                );
            }
        }
        if let Err(err) = fs::copy(&linked_artifact, &spec.output_path) {
            panic!(
                "failed to stage linked binary from {} to {}: {err}",
                linked_artifact.display(),
                spec.output_path.display()
            );
        }
    }

    println!(
        "cargo:warning=mako cargo bridge built target '{}' (query '{}', obj-prefix '{}') as {} (output token '{}', staged path {})",
        spec.target_name,
        target_query,
        spec.target_obj_dir_rel,
        dist_bin.display(),
        spec.output_token,
        if should_stage {
            spec.output_path.display().to_string()
        } else {
            "(staging skipped)".to_string()
        }
    );
}
