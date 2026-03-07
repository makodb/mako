use std::collections::BTreeSet;
use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::SystemTime;

use walkdir::{DirEntry, WalkDir};

const CMAKE_ARGS_STAMP: &str = ".mako-cmake-args";

#[derive(Debug, Clone)]
struct CommonOptions {
    build_dir: PathBuf,
    cmake_args: Vec<String>,
    force_configure: bool,
    skip_configure: bool,
}

#[derive(Debug)]
enum Action {
    EnsureCmake(CommonOptions),
    ListTargets {
        common: CommonOptions,
        include_cmake_targets: bool,
    },
    BuildTarget {
        common: CommonOptions,
        target: String,
        link_txt: Option<PathBuf>,
        stage_to_build: bool,
        bridge_only: bool,
        cmake_fallback: bool,
        cargo_args: Vec<String>,
    },
    BuildCmakeTarget {
        common: CommonOptions,
        target: String,
    },
    BuildAllTargets {
        common: CommonOptions,
        stage_to_build: bool,
        cmake_fallback: bool,
        keep_going: bool,
        cargo_args: Vec<String>,
    },
    BuildCtest(CommonOptions),
    Ctest(CommonOptions),
    Help,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum LinkTargetKind {
    StaticLibrary,
    SharedLibrary,
    Executable,
}

impl LinkTargetKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::StaticLibrary => "static",
            Self::SharedLibrary => "shared",
            Self::Executable => "executable",
        }
    }
}

#[derive(Debug, Clone)]
struct LinkTarget {
    target_name: String,
    output_name: String,
    kind: LinkTargetKind,
    link_txt_path: PathBuf,
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
        Action::EnsureCmake(common) => {
            ensure_cmake_configured(&repo_root, &common)?;
            Ok(())
        }
        Action::ListTargets {
            common,
            include_cmake_targets,
        } => {
            ensure_cmake_configured(&repo_root, &common)?;
            let build_dir_abs = absolutize(&repo_root, &common.build_dir);
            let link_targets = discover_link_targets(&build_dir_abs)?;
            print_link_targets(&build_dir_abs, &link_targets);
            if include_cmake_targets {
                let cmake_targets = discover_cmake_targets(&common.build_dir)?;
                println!("cmake targets discovered: {}", cmake_targets.len());
                for target in cmake_targets {
                    println!("cmake\t{target}");
                }
            }
            Ok(())
        }
        Action::BuildTarget {
            common,
            target,
            link_txt,
            stage_to_build,
            bridge_only,
            cmake_fallback,
            cargo_args,
        } => {
            ensure_cmake_configured(&repo_root, &common)?;
            let build_dir_abs = absolutize(&repo_root, &common.build_dir);
            let resolved = if let Some(path) = link_txt.as_deref() {
                Some(resolve_explicit_link_txt(
                    &repo_root,
                    &common.build_dir,
                    path,
                )?)
            } else {
                let link_targets = discover_link_targets(&build_dir_abs)?;
                find_matching_link_target(&build_dir_abs, &link_targets, &target)?
                    .map(|entry| entry.link_txt_path)
            };

            if let Some(link_txt_path) = resolved {
                let bridge_result = run_bridge_target_build(
                    &repo_root,
                    &common.build_dir,
                    &target,
                    Some(&link_txt_path),
                    stage_to_build,
                    &cargo_args,
                );
                match bridge_result {
                    Ok(()) => Ok(()),
                    Err(err) if cmake_fallback => {
                        eprintln!(
                            "mako-xtask: bridge build failed for target '{}': {}\nfalling back to cmake --build --target {}",
                            target, err, target
                        );
                        run_cmake_build_target(&common.build_dir, &target)
                    }
                    Err(err) => Err(err),
                }
            } else if bridge_only {
                Err(format!(
                    "target '{}' is not a link.txt-backed build target in {}. Use `cargo mako build-cmake-target` for custom/utility CMake targets.",
                    target,
                    build_dir_abs.display()
                ))
            } else {
                println!(
                    "mako-xtask: target '{}' is not link.txt-backed; using cmake --build --target",
                    target
                );
                run_cmake_build_target(&common.build_dir, &target)
            }
        }
        Action::BuildCmakeTarget { common, target } => {
            ensure_cmake_configured(&repo_root, &common)?;
            run_cmake_build_target(&common.build_dir, &target)
        }
        Action::BuildAllTargets {
            common,
            stage_to_build,
            cmake_fallback,
            keep_going,
            cargo_args,
        } => {
            ensure_cmake_configured(&repo_root, &common)?;
            let build_dir_abs = absolutize(&repo_root, &common.build_dir);
            let targets = discover_link_targets(&build_dir_abs)?;
            if targets.is_empty() {
                return Err(format!(
                    "no link.txt targets discovered under {}",
                    build_dir_abs.display()
                ));
            }

            println!("link targets discovered: {}", targets.len());
            let mut failures: Vec<String> = Vec::new();

            for (idx, entry) in targets.iter().enumerate() {
                println!(
                    "==> [{}/{}] building {} target: {} ({})",
                    idx + 1,
                    targets.len(),
                    entry.kind.as_str(),
                    entry.target_name,
                    entry.output_name
                );
                if let Err(err) = run_bridge_target_build(
                    &repo_root,
                    &common.build_dir,
                    &entry.target_name,
                    Some(&entry.link_txt_path),
                    stage_to_build,
                    &cargo_args,
                ) {
                    if cmake_fallback {
                        eprintln!(
                            "mako-xtask: bridge build failed for target '{}' ({}): {}\nfalling back to cmake --build --target {}",
                            entry.target_name,
                            entry.kind.as_str(),
                            err,
                            entry.target_name
                        );
                        match run_cmake_build_target(&common.build_dir, &entry.target_name) {
                            Ok(()) => {
                                continue;
                            }
                            Err(cmake_err) => {
                                let summary = format!(
                                    "{} [{}]: bridge failed: {}; cmake fallback failed: {}",
                                    entry.target_name,
                                    link_target_id(&entry.link_txt_path, &build_dir_abs)
                                        .unwrap_or_else(|| entry
                                            .link_txt_path
                                            .display()
                                            .to_string()),
                                    err,
                                    cmake_err
                                );
                                if keep_going {
                                    eprintln!("mako-xtask: build failed: {summary}");
                                    failures.push(summary);
                                    continue;
                                }
                                return Err(summary);
                            }
                        }
                    }

                    let summary = format!(
                        "{} [{}]: {}",
                        entry.target_name,
                        link_target_id(&entry.link_txt_path, &build_dir_abs)
                            .unwrap_or_else(|| entry.link_txt_path.display().to_string()),
                        err
                    );
                    if keep_going {
                        eprintln!("mako-xtask: build failed: {summary}");
                        failures.push(summary);
                    } else {
                        return Err(summary);
                    }
                }
            }

            if failures.is_empty() {
                println!("built link targets: {}", targets.len());
                Ok(())
            } else {
                Err(format!(
                    "{} target(s) failed during bridge build:\n{}",
                    failures.len(),
                    failures.join("\n")
                ))
            }
        }
        Action::BuildCtest(common) => {
            ensure_cmake_configured(&repo_root, &common)?;
            run_script(
                &repo_root,
                "scripts/cargo_bridge_build_ctest_targets.sh",
                &common.build_dir,
            )
        }
        Action::Ctest(common) => {
            ensure_cmake_configured(&repo_root, &common)?;
            run_script(
                &repo_root,
                "scripts/cargo_bridge_build_ctest_targets.sh",
                &common.build_dir,
            )?;
            run_ctest(&common.build_dir)
        }
    }
}

fn parse_action(args: &mut Vec<String>) -> Result<Action, String> {
    let cmd = args.remove(0);

    match cmd.as_str() {
        "help" | "-h" | "--help" => Ok(Action::Help),
        "ensure-cmake" => {
            let common = parse_common_options(args)?;
            reject_remaining(args, "ensure-cmake")?;
            Ok(Action::EnsureCmake(common))
        }
        "list-targets" => parse_list_targets(args),
        "build-target" => parse_build_target(args),
        "build-cmake-target" => parse_build_cmake_target(args),
        "build-all-targets" | "build-all-exec" => parse_build_all_targets(args),
        "build-ctest" => {
            let common = parse_common_options(args)?;
            reject_remaining(args, "build-ctest")?;
            Ok(Action::BuildCtest(common))
        }
        "ctest" => {
            let common = parse_common_options(args)?;
            reject_remaining(args, "ctest")?;
            Ok(Action::Ctest(common))
        }
        other => Err(format!(
            "unknown command '{}'. Run `cargo mako help` for usage.",
            other
        )),
    }
}

fn parse_list_targets(args: &mut Vec<String>) -> Result<Action, String> {
    let common = parse_common_options(args)?;
    let mut include_cmake_targets = false;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--include-cmake-targets" => {
                include_cmake_targets = true;
                i += 1;
            }
            other => {
                return Err(format!(
                    "unknown list-targets option '{}'. Run `cargo mako help`.",
                    other
                ));
            }
        }
    }

    args.clear();

    Ok(Action::ListTargets {
        common,
        include_cmake_targets,
    })
}

fn parse_build_target(args: &mut Vec<String>) -> Result<Action, String> {
    let mut common = parse_common_options(args)?;

    if args.is_empty() {
        return Err("build-target requires <target>".to_string());
    }

    let target = args.remove(0);
    if target.starts_with('-') {
        return Err("build-target requires <target> before flags".to_string());
    }

    let mut link_txt: Option<PathBuf> = None;
    let mut stage_to_build = true;
    let mut bridge_only = false;
    let mut cmake_fallback = true;
    let mut cargo_args: Vec<String> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--build-dir" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --build-dir".to_string())?;
                common.build_dir = PathBuf::from(value);
                i += 2;
            }
            "--cmake-arg" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --cmake-arg".to_string())?;
                common.cmake_args.push(value.clone());
                i += 2;
            }
            "--force-configure" => {
                common.force_configure = true;
                i += 1;
            }
            "--skip-configure" => {
                common.skip_configure = true;
                i += 1;
            }
            "--link-txt" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --link-txt".to_string())?;
                link_txt = Some(PathBuf::from(value));
                i += 2;
            }
            "--no-stage" => {
                stage_to_build = false;
                i += 1;
            }
            "--bridge-only" => {
                bridge_only = true;
                i += 1;
            }
            "--cmake-fallback" => {
                cmake_fallback = true;
                i += 1;
            }
            "--no-cmake-fallback" => {
                cmake_fallback = false;
                i += 1;
            }
            "--cargo-arg" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --cargo-arg".to_string())?;
                cargo_args.push(value.clone());
                i += 2;
            }
            "--" => {
                cargo_args.extend(args[i + 1..].iter().cloned());
                i = args.len();
            }
            other => {
                return Err(format!(
                    "unknown build-target option '{}'. Run `cargo mako help`.",
                    other
                ));
            }
        }
    }

    args.clear();

    Ok(Action::BuildTarget {
        common,
        target,
        link_txt,
        stage_to_build,
        bridge_only,
        cmake_fallback,
        cargo_args,
    })
}

fn parse_build_cmake_target(args: &mut Vec<String>) -> Result<Action, String> {
    let mut common = parse_common_options(args)?;

    if args.is_empty() {
        return Err("build-cmake-target requires <target>".to_string());
    }

    let target = args.remove(0);
    if target.starts_with('-') {
        return Err("build-cmake-target requires <target> before flags".to_string());
    }

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--build-dir" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --build-dir".to_string())?;
                common.build_dir = PathBuf::from(value);
                i += 2;
            }
            "--cmake-arg" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --cmake-arg".to_string())?;
                common.cmake_args.push(value.clone());
                i += 2;
            }
            "--force-configure" => {
                common.force_configure = true;
                i += 1;
            }
            "--skip-configure" => {
                common.skip_configure = true;
                i += 1;
            }
            other => {
                return Err(format!(
                    "unknown build-cmake-target option '{}'. Run `cargo mako help`.",
                    other
                ));
            }
        }
    }

    args.clear();

    Ok(Action::BuildCmakeTarget { common, target })
}

fn parse_build_all_targets(args: &mut Vec<String>) -> Result<Action, String> {
    let mut common = parse_common_options(args)?;
    let mut stage_to_build = true;
    let mut cmake_fallback = true;
    let mut keep_going = false;
    let mut cargo_args: Vec<String> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--build-dir" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --build-dir".to_string())?;
                common.build_dir = PathBuf::from(value);
                i += 2;
            }
            "--cmake-arg" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --cmake-arg".to_string())?;
                common.cmake_args.push(value.clone());
                i += 2;
            }
            "--force-configure" => {
                common.force_configure = true;
                i += 1;
            }
            "--skip-configure" => {
                common.skip_configure = true;
                i += 1;
            }
            "--no-stage" => {
                stage_to_build = false;
                i += 1;
            }
            "--bridge-only" => {
                cmake_fallback = false;
                i += 1;
            }
            "--keep-going" => {
                keep_going = true;
                i += 1;
            }
            "--cargo-arg" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --cargo-arg".to_string())?;
                cargo_args.push(value.clone());
                i += 2;
            }
            "--" => {
                cargo_args.extend(args[i + 1..].iter().cloned());
                i = args.len();
            }
            other => {
                return Err(format!(
                    "unknown build-all-targets option '{}'. Run `cargo mako help`.",
                    other
                ));
            }
        }
    }

    args.clear();

    Ok(Action::BuildAllTargets {
        common,
        stage_to_build,
        cmake_fallback,
        keep_going,
        cargo_args,
    })
}

fn parse_common_options(args: &mut Vec<String>) -> Result<CommonOptions, String> {
    let mut build_dir = PathBuf::from("build");
    let mut cmake_args = cmake_args_from_env()?;
    let mut force_configure = false;
    let mut skip_configure = false;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--build-dir" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --build-dir".to_string())?;
                build_dir = PathBuf::from(value);
                i += 2;
            }
            "--cmake-arg" => {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| "missing value for --cmake-arg".to_string())?;
                cmake_args.push(value.clone());
                i += 2;
            }
            "--force-configure" => {
                force_configure = true;
                i += 1;
            }
            "--skip-configure" => {
                skip_configure = true;
                i += 1;
            }
            _ => {
                break;
            }
        }
    }

    args.drain(0..i);

    Ok(CommonOptions {
        build_dir,
        cmake_args,
        force_configure,
        skip_configure,
    })
}

fn reject_remaining(args: &[String], subcmd: &str) -> Result<(), String> {
    if args.is_empty() {
        return Ok(());
    }
    Err(format!("unexpected arguments for {}: {:?}", subcmd, args))
}

fn cmake_args_from_env() -> Result<Vec<String>, String> {
    let raw = match env::var("MAKO_CMAKE_ARGS") {
        Ok(value) => value,
        Err(_) => return Ok(Vec::new()),
    };

    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Ok(Vec::new());
    }

    shell_words::split(trimmed).map_err(|e| format!("failed to parse MAKO_CMAKE_ARGS: {e}"))
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

fn ensure_cmake_configured(repo_root: &Path, common: &CommonOptions) -> Result<(), String> {
    if common.skip_configure {
        return Ok(());
    }

    let build_dir = absolutize(repo_root, &common.build_dir);
    let reason = cmake_reconfigure_reason(
        repo_root,
        &build_dir,
        &common.cmake_args,
        common.force_configure,
    )?;

    if let Some(reason) = reason {
        println!(
            "mako-xtask: running CMake configure for {} ({})",
            build_dir.display(),
            reason
        );

        fs::create_dir_all(&build_dir)
            .map_err(|e| format!("failed to create build dir {}: {}", build_dir.display(), e))?;

        let mut cmd = Command::new("cmake");
        cmd.current_dir(repo_root);
        cmd.arg("-S").arg(repo_root);
        cmd.arg("-B").arg(&build_dir);
        cmd.arg("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
        cmd.args(&common.cmake_args);
        run_checked(cmd, "cmake configure")?;

        let stamp_path = build_dir.join(CMAKE_ARGS_STAMP);
        let stamp_value = cmake_args_stamp_value(&common.cmake_args);
        fs::write(&stamp_path, stamp_value).map_err(|e| {
            format!(
                "failed to write CMake args stamp {}: {}",
                stamp_path.display(),
                e
            )
        })?;
    } else {
        println!(
            "mako-xtask: CMake configure is up to date for {}",
            build_dir.display()
        );
    }

    Ok(())
}

fn cmake_reconfigure_reason(
    repo_root: &Path,
    build_dir: &Path,
    cmake_args: &[String],
    force_configure: bool,
) -> Result<Option<String>, String> {
    if force_configure {
        return Ok(Some("forced by --force-configure".to_string()));
    }

    let cache_path = build_dir.join("CMakeCache.txt");
    if !cache_path.is_file() {
        return Ok(Some("missing CMakeCache.txt".to_string()));
    }

    let compile_commands = build_dir.join("compile_commands.json");
    if !compile_commands.is_file() {
        return Ok(Some("missing compile_commands.json".to_string()));
    }

    let stamp_path = build_dir.join(CMAKE_ARGS_STAMP);
    let expected = cmake_args_stamp_value(cmake_args);
    let actual = fs::read_to_string(&stamp_path).unwrap_or_default();
    if actual != expected {
        return Ok(Some("cmake args changed".to_string()));
    }

    let cache_mtime = file_mtime(&cache_path)?;
    let newest_input = newest_cmake_input_mtime(repo_root)?;
    if newest_input > cache_mtime {
        return Ok(Some("cmake input files changed".to_string()));
    }

    Ok(None)
}

fn cmake_args_stamp_value(args: &[String]) -> String {
    let mut lines = Vec::with_capacity(args.len() + 1);
    lines.push("# mako xtask cmake args stamp".to_string());
    lines.extend(args.iter().cloned());
    lines.join("\n")
}

fn newest_cmake_input_mtime(repo_root: &Path) -> Result<SystemTime, String> {
    let mut newest = SystemTime::UNIX_EPOCH;

    for entry in WalkDir::new(repo_root)
        .into_iter()
        .filter_entry(|e| should_walk_entry(repo_root, e))
    {
        let entry = entry.map_err(|e| format!("failed to walk repo tree: {e}"))?;
        if !entry.file_type().is_file() {
            continue;
        }
        let path = entry.path();
        let file_name = path.file_name().and_then(|v| v.to_str()).unwrap_or("");
        let is_cmake = file_name == "CMakeLists.txt"
            || path
                .extension()
                .and_then(|v| v.to_str())
                .is_some_and(|ext| ext.eq_ignore_ascii_case("cmake"));
        if !is_cmake {
            continue;
        }

        let mtime = file_mtime(path)?;
        if mtime > newest {
            newest = mtime;
        }
    }

    Ok(newest)
}

fn should_walk_entry(repo_root: &Path, entry: &DirEntry) -> bool {
    if !entry.file_type().is_dir() {
        return true;
    }

    let name = entry.file_name().to_string_lossy();
    if name == ".git" || name == "target" || name == ".cargo" {
        return false;
    }

    if name.starts_with("build") {
        return false;
    }

    if entry.path() == repo_root {
        return true;
    }

    true
}

fn run_bridge_target_build(
    repo_root: &Path,
    build_dir: &Path,
    target: &str,
    link_txt: Option<&Path>,
    stage_to_build: bool,
    cargo_args: &[String],
) -> Result<(), String> {
    let mut cmd = Command::new(cargo_bin());
    cmd.current_dir(repo_root);
    cmd.arg("build");
    cmd.arg("-p");
    cmd.arg("mako-cmake-bridge");
    cmd.args(cargo_args);

    cmd.env("MAKO_BUILD_DIR", absolutize(repo_root, build_dir));
    cmd.env("MAKO_CMAKE_TARGET", target);
    cmd.env(
        "MAKO_CMAKE_STAGE_TO_BUILD",
        if stage_to_build { "1" } else { "0" },
    );
    if let Some(link_txt) = link_txt {
        cmd.env("MAKO_CMAKE_LINK_TXT", absolutize(repo_root, link_txt));
    } else {
        cmd.env_remove("MAKO_CMAKE_LINK_TXT");
    }

    run_checked(cmd, "cargo build target bridge")
}

fn run_script(repo_root: &Path, script_rel: &str, build_dir: &Path) -> Result<(), String> {
    let script_path = repo_root.join(script_rel);
    if !script_path.is_file() {
        return Err(format!("missing script {}", script_path.display()));
    }

    let mut cmd = Command::new(&script_path);
    cmd.current_dir(repo_root);
    cmd.arg(absolutize(repo_root, build_dir));
    run_checked(cmd, &format!("run {}", script_rel))
}

fn run_ctest(build_dir: &Path) -> Result<(), String> {
    let build_dir = absolute_from_cwd(build_dir)
        .map_err(|e| format!("failed to resolve build dir {}: {e}", build_dir.display()))?;
    let mut cmd = Command::new("ctest");
    cmd.current_dir(&build_dir);
    cmd.arg("--output-on-failure");
    run_checked(cmd, "ctest")
}

fn run_cmake_build_target(build_dir: &Path, target: &str) -> Result<(), String> {
    let build_dir = absolute_from_cwd(build_dir)
        .map_err(|e| format!("failed to resolve build dir {}: {e}", build_dir.display()))?;
    let mut cmd = Command::new("cmake");
    cmd.arg("--build");
    cmd.arg(&build_dir);
    cmd.arg("--target");
    cmd.arg(target);
    run_checked(cmd, "cmake --build target")
}

fn run_capture(mut cmd: Command, stage: &str) -> Result<String, String> {
    let rendered = render_command(&cmd);
    println!("+ {rendered}");
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

    String::from_utf8(output.stdout).map_err(|e| format!("{stage}: stdout was not utf-8: {e}"))
}

fn discover_cmake_targets(build_dir: &Path) -> Result<Vec<String>, String> {
    let build_dir = absolute_from_cwd(build_dir)
        .map_err(|e| format!("failed to resolve build dir {}: {e}", build_dir.display()))?;

    let mut cmd = Command::new("cmake");
    cmd.arg("--build");
    cmd.arg(&build_dir);
    cmd.arg("--target");
    cmd.arg("help");

    let stdout = run_capture(cmd, "cmake --build --target help")?;
    let mut targets = BTreeSet::new();

    for raw in stdout.lines() {
        let line = raw.trim_start();
        if !line.starts_with("...") {
            continue;
        }
        let name = line.trim_start_matches('.').trim();
        if !name.is_empty() {
            targets.insert(name.to_string());
        }
    }

    Ok(targets.into_iter().collect())
}

fn discover_link_targets(build_dir: &Path) -> Result<Vec<LinkTarget>, String> {
    if !build_dir.is_dir() {
        return Err(format!(
            "build directory does not exist: {}",
            build_dir.display()
        ));
    }

    let mut out: Vec<LinkTarget> = Vec::new();
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

        let link_txt_path = canonical_or_self(entry.path());
        let target_name = parent_name
            .strip_suffix(".dir")
            .unwrap_or(parent_name)
            .to_string();
        let output_name = infer_output_name_from_link_txt(&link_txt_path, &target_name)?;
        let kind = classify_link_target_kind(&output_name);

        out.push(LinkTarget {
            target_name,
            output_name,
            kind,
            link_txt_path,
        });
    }

    out.sort_by(|a, b| {
        a.kind
            .cmp(&b.kind)
            .then_with(|| a.target_name.cmp(&b.target_name))
            .then_with(|| a.link_txt_path.cmp(&b.link_txt_path))
    });

    Ok(out)
}

fn parse_link_command_tokens(link_txt_path: &Path) -> Result<Vec<String>, String> {
    let raw = fs::read_to_string(link_txt_path)
        .map_err(|e| format!("failed to read {}: {e}", link_txt_path.display()))?;

    for (idx, raw_line) in raw.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() {
            continue;
        }

        return shell_words::split(line).map_err(|e| {
            format!(
                "failed to parse line {} in {}: {}",
                idx + 1,
                link_txt_path.display(),
                e
            )
        });
    }

    Err(format!("empty link.txt: {}", link_txt_path.display()))
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

fn infer_output_name_from_link_txt(link_txt_path: &Path, fallback: &str) -> Result<String, String> {
    let tokens = parse_link_command_tokens(link_txt_path)?;
    if tokens.is_empty() {
        return Ok(fallback.to_string());
    }

    let mut i = 0usize;
    while i < tokens.len() {
        let tok = &tokens[i];
        if tok == "-o" {
            if let Some(next) = tokens.get(i + 1) {
                return Ok(output_name_from_token(next, fallback));
            }
            break;
        }
        if let Some(rest) = tok.strip_prefix("-o") {
            if !rest.is_empty() {
                return Ok(output_name_from_token(rest, fallback));
            }
        }
        i += 1;
    }

    if is_ar_tool(&tokens[0]) {
        let mut idx = 1usize;
        while idx < tokens.len() {
            let tok = &tokens[idx];
            let looks_like_flag = tok.starts_with('-')
                || (idx == 1 && tok.chars().all(|ch| ch.is_ascii_alphabetic()));
            if !looks_like_flag {
                return Ok(output_name_from_token(tok, fallback));
            }
            idx += 1;
        }
    }

    Ok(fallback.to_string())
}

fn output_name_from_token(token: &str, fallback: &str) -> String {
    Path::new(token)
        .file_name()
        .and_then(|s| s.to_str())
        .map(str::to_string)
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| fallback.to_string())
}

fn classify_link_target_kind(output_name: &str) -> LinkTargetKind {
    if output_name.ends_with(".a") {
        LinkTargetKind::StaticLibrary
    } else if output_name.ends_with(".so")
        || output_name.contains(".so.")
        || output_name.ends_with(".dylib")
    {
        LinkTargetKind::SharedLibrary
    } else {
        LinkTargetKind::Executable
    }
}

fn canonical_or_self(path: &Path) -> PathBuf {
    path.canonicalize().unwrap_or_else(|_| path.to_path_buf())
}

fn find_matching_link_target(
    build_dir_abs: &Path,
    link_targets: &[LinkTarget],
    query: &str,
) -> Result<Option<LinkTarget>, String> {
    let q = normalize_rel_token(query.trim().trim_end_matches('/'));
    if q.is_empty() {
        return Err("empty target query".to_string());
    }

    let mut matches: Vec<LinkTarget> = Vec::new();
    for entry in link_targets {
        let id = link_target_id(&entry.link_txt_path, build_dir_abs);
        let name_match = entry.target_name == q || entry.output_name == q;
        let id_match = id
            .as_ref()
            .is_some_and(|v| v == &q || v.ends_with(&format!("/{q}")));
        if name_match || id_match {
            matches.push(entry.clone());
        }
    }

    if matches.is_empty() {
        return Ok(None);
    }

    if matches.len() == 1 {
        return Ok(matches.pop());
    }

    let mut rendered: Vec<String> = matches
        .iter()
        .take(8)
        .map(|entry| {
            let id = link_target_id(&entry.link_txt_path, build_dir_abs)
                .unwrap_or_else(|| entry.link_txt_path.display().to_string());
            format!(
                "  - {} ({}, output: {})",
                entry.target_name, id, entry.output_name
            )
        })
        .collect();
    if matches.len() > 8 {
        rendered.push(format!("  ... and {} more", matches.len() - 8));
    }

    Err(format!(
        "target query '{}' matched multiple link targets in {}. Pass --link-txt to disambiguate.\n{}",
        q,
        build_dir_abs.display(),
        rendered.join("\n")
    ))
}

fn resolve_explicit_link_txt(
    repo_root: &Path,
    build_dir: &Path,
    path: &Path,
) -> Result<PathBuf, String> {
    let value = absolutize(repo_root, path);
    let candidates = [
        value.clone(),
        value.join("link.txt"),
        PathBuf::from(format!("{}.dir/link.txt", value.display())),
    ];

    for candidate in &candidates {
        if candidate.is_file() {
            let resolved = canonical_or_self(candidate);
            let build_dir_abs = absolutize(repo_root, build_dir);
            if resolved.starts_with(&build_dir_abs) {
                return Ok(resolved);
            }
            return Err(format!(
                "--link-txt path must be under MAKO build dir {}: {}",
                build_dir_abs.display(),
                resolved.display()
            ));
        }
    }

    Err(format!(
        "failed to resolve --link-txt '{}' as a readable link.txt. Checked:\n{}",
        path.display(),
        candidates
            .iter()
            .map(|p| format!("  - {}", p.display()))
            .collect::<Vec<String>>()
            .join("\n")
    ))
}

fn link_target_id(link_txt_path: &Path, build_dir_abs: &Path) -> Option<String> {
    let rel = link_txt_path.strip_prefix(build_dir_abs).ok()?;
    let mut value = normalize_rel_token(&rel.to_string_lossy());
    if !value.ends_with("/link.txt") {
        return None;
    }
    value.truncate(value.len() - "/link.txt".len());
    if value.ends_with(".dir") {
        value.truncate(value.len() - ".dir".len());
    }
    Some(value)
}

fn normalize_rel_token(value: &str) -> String {
    value.replace('\\', "/")
}

fn print_link_targets(build_dir_abs: &Path, link_targets: &[LinkTarget]) {
    println!("link targets discovered: {}", link_targets.len());
    for entry in link_targets {
        let id = link_target_id(&entry.link_txt_path, build_dir_abs)
            .unwrap_or_else(|| entry.link_txt_path.display().to_string());
        println!(
            "{}\t{}\t{}\t{}",
            entry.kind.as_str(),
            entry.target_name,
            entry.output_name,
            id
        );
    }
}

fn run_checked(mut cmd: Command, stage: &str) -> Result<(), String> {
    let rendered = render_command(&cmd);
    println!("+ {rendered}");
    let status = cmd
        .stdin(Stdio::null())
        .status()
        .map_err(|e| format!("{stage}: failed to spawn command: {e}"))?;
    if !status.success() {
        return Err(format!("{stage}: command failed with status {}", status));
    }
    Ok(())
}

fn render_command(cmd: &Command) -> String {
    let mut parts: Vec<String> = Vec::new();
    parts.push(cmd.get_program().to_string_lossy().to_string());
    parts.extend(cmd.get_args().map(shell_escape));
    parts.join(" ")
}

fn shell_escape(value: &OsStr) -> String {
    let s = value.to_string_lossy();
    if s.chars()
        .all(|ch| ch.is_ascii_alphanumeric() || "-._/:=".contains(ch))
    {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

fn file_mtime(path: &Path) -> Result<SystemTime, String> {
    fs::metadata(path)
        .and_then(|m| m.modified())
        .map_err(|e| format!("failed to read modified time for {}: {}", path.display(), e))
}

fn absolutize(repo_root: &Path, path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        repo_root.join(path)
    }
}

fn absolute_from_cwd(path: &Path) -> Result<PathBuf, std::io::Error> {
    if path.is_absolute() {
        Ok(path.to_path_buf())
    } else {
        Ok(env::current_dir()?.join(path))
    }
}

fn cargo_bin() -> String {
    env::var("CARGO").unwrap_or_else(|_| "cargo".to_string())
}

fn print_help() {
    println!(
        "mako-xtask: Cargo entrypoint for CMake-backed Mako builds\n\n\
Usage:\n\
  cargo mako help\n\
  cargo mako ensure-cmake [common options]\n\
  cargo mako list-targets [common options] [--include-cmake-targets]\n\
  cargo mako build-target [common options] <target> [--link-txt PATH] [--no-stage] [--bridge-only] [--no-cmake-fallback] [--cargo-arg ARG ...] [-- <extra cargo args>]\n\
  cargo mako build-cmake-target [common options] <target>\n\
  cargo mako build-all-targets [common options] [--no-stage] [--bridge-only] [--keep-going] [--cargo-arg ARG ...] [-- <extra cargo args>]\n\
  cargo mako build-ctest [common options]\n\
  cargo mako ctest [common options]\n\n\
Common options:\n\
  --build-dir DIR          CMake build directory (default: build)\n\
  --cmake-arg ARG          extra arg passed to cmake configure (repeatable)\n\
  --force-configure        force cmake reconfigure\n\
  --skip-configure         do not run cmake configure checks\n\n\
Target-build options:\n\
  --link-txt PATH          explicit CMake link.txt for nested/ambiguous targets\n\
  --no-stage               disable MAKO_CMAKE_STAGE_TO_BUILD\n\
  --bridge-only            fail if target is not link.txt-backed\n\
  --cmake-fallback         enable bridge-failure fallback to cmake (default)\n\
  --no-cmake-fallback      disable bridge-failure fallback to cmake\n\
  --cargo-arg ARG          extra cargo arg passed to bridge build (repeatable)\n\n\
Build-all options:\n\
  --bridge-only            do not use cmake fallback on bridge failures\n\
  --keep-going             continue building remaining link targets after failures\n\n\
Env:\n\
  MAKO_CMAKE_ARGS          shell-quoted cmake args appended before --cmake-arg\n\n\
Examples:\n\
  cargo mako list-targets --build-dir build_clean_cargo\n\
  cargo mako build-target test_rpc --build-dir build_clean_cargo\n\
  cargo mako build-target gflags_unittest --build-dir build_clean_cargo --link-txt build_clean_cargo/third-party/erpc/third_party/gflags/test/CMakeFiles/gflags_unittest.dir/link.txt\n\
  cargo mako build-target borrow_check_all --build-dir build_clean_cargo\n\
  cargo mako build-all-targets --build-dir build_clean_cargo\n\
  cargo mako build-ctest --build-dir build_clean_cargo\n\
  cargo mako ctest --build-dir build_clean_cargo\n"
    );
}
