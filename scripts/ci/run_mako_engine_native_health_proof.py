#!/usr/bin/env python3
"""Run isolated native-poison diagnostics with a test-only boundary archive.

This is a functional state-transition proof, not native exception injection
or a sanitizer run. It never changes or rebuilds the production archives.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


TEST = "db::native_health_tests::native_poison_is_diagnostic_before_point_mapping"
HELPER = "mako_test_native_runtime_poison"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compile_command(entry, source, output):
    original = entry.get("arguments") or shlex.split(entry["command"])
    command = []
    removed = []
    index = 0
    while index < len(original):
        arg = original[index]
        if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
            if index + 1 == len(original):
                raise ValueError(f"missing argument after {arg}")
            removed.extend(original[index:index + 2])
            index += 2
            continue
        if arg in ("-MD", "-MMD", "-MP") or (
            arg.startswith("@") and arg.endswith(".modmap")
        ):
            removed.append(arg)
        elif arg.startswith("@"):
            raise ValueError(f"unreviewed response file: {arg}")
        else:
            command.append(arg)
        index += 1
    command += ["-DMAKO_NATIVE_RUNTIME_HEALTH_PROOF=1", "-c", str(source), "-o", str(output)]
    return original, removed, command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--libcxx-dir", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    repo = args.repo_root.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    libcxx = args.libcxx_dir.resolve(strict=True)
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=False, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="mako-native-runtime-health-"))
    source = repo / "crates/mako-engine-sto/tests/native_runtime_health_proof.cc"
    production = [build / "libmako.a", build / "src/masstree/libmasstree.a"]
    inputs = set(production + [source, Path(__file__).resolve(),
                              repo / "src/mako/storage/mtree_abi.cc",
                              build / "compile_commands.json", repo / "crates/Cargo.lock",
                              repo / "rust-toolchain.toml"])
    for crate in ("mako-engine-sto", "mako-engine-api", "mako-clock", "sto-core",
                  "sto-masstree", "masstree", "mtree-sys", "mako-differential", "mako-history"):
        inputs.update((repo / "crates" / crate).rglob("*.rs"))
        inputs.add(repo / "crates" / crate / "Cargo.toml")
    inputs.add(repo / "crates/Cargo.toml")
    before = {str(path): digest(path) for path in sorted(inputs)}
    report = {"scope": "test-only actual native Release poison transition; no native exception",
              "repo": str(repo), "output": str(output), "inputs_before": before,
              "commands": [], "passed": False}

    def run(label, command, *, cwd=repo, env=None, timeout=900):
        command = [str(arg) for arg in command]
        report["commands"].append({"label": label, "argv": command, "cwd": str(cwd)})
        with (output / f"{label}.log").open("w") as stream:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout, check=False)
        report["commands"][-1]["exit_code"] = result.returncode
        if result.returncode:
            raise RuntimeError(f"{label} failed ({result.returncode}); see {output / (label + '.log')}")
        return (output / f"{label}.log").read_text()

    try:
        entries = [entry for entry in json.loads((build / "compile_commands.json").read_text())
                   if entry.get("output") == "CMakeFiles/mako.dir/src/mako/storage/mtree_abi.cc.o"]
        if len(entries) != 1:
            raise ValueError("expected exactly one production mtree_abi.cc compile command")
        original, removed, command = compile_command(entries[0], source, output / "proof.o")
        report.update(original_native_command=original, removed_native_args=removed)
        compiler_bin = Path(command[0]).parent
        nm = compiler_bin / "llvm-nm"
        for index, archive in enumerate(production):
            symbols = run(f"production-symbols-{index}", [nm, "--defined-only", archive])
            if HELPER in symbols:
                raise RuntimeError("test helper unexpectedly present in production archive")
        run("native-compile", command, cwd=Path(entries[0]["directory"]))
        archive = output / "libmtree_runtime_health_proof.a"
        run("native-archive", [compiler_bin / "llvm-ar", "rcs", archive, output / "proof.o"])
        env = os.environ.copy()
        for name in list(env):
            if name.startswith(("CARGO_ENCODED_", "CARGO_BUILD_", "CARGO_TARGET_")) or name in (
                "RUSTC", "RUSTDOC", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "RUSTDOCFLAGS",
                "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS",
                "MAKO_NATIVE_RUNTIME_HEALTH_PROOF_ROLE",
            ):
                env.pop(name)
        configured = {
            "RUSTUP_TOOLCHAIN": "1.95.0", "CARGO_TARGET_DIR": str(output / "target"),
            "MAKO_BUILD_DIR": str(build), "MAKO_MTREE_NATIVE_INTEGRATION": "1",
            "MAKO_MTREE_NATIVE_LIB_DIRS": os.pathsep.join(map(str, [output, build, build / "src/masstree", libcxx])),
            "MAKO_MTREE_NATIVE_LIBS": "static=mtree_runtime_health_proof,static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread",
            "LD_LIBRARY_PATH": os.pathsep.join(map(str, [libcxx, build / "third-party/yaml-cpp"])),
            "RUSTFLAGS": "--cfg mako_native_runtime_health_proof -C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1",
        }
        env.update(configured)
        report["environment"] = configured
        report["rustc"] = run("rustc-version", ["rustc", "--version", "--verbose"], env=env)
        messages = run("rust-build", ["cargo", "test", "--manifest-path", repo / "crates/Cargo.toml",
                       "--locked", "-p", "mako-engine-sto", "--all-features", "--lib", "--no-run",
                       "--message-format=json"], env=env)
        executables = []
        for line in messages.splitlines():
            if line.startswith("{"):
                message = json.loads(line)
                if message.get("reason") == "compiler-artifact" and message.get("executable"):
                    executables.append(Path(message["executable"]))
        if len(executables) != 1:
            raise RuntimeError(f"expected one engine library test executable: {executables}")
        executable = executables[0]
        symbols = run("proof-symbols", [nm, "--defined-only", executable])
        for symbol in (HELPER, "mt_runtime_health"):
            if sum(line.split()[-1:] == [symbol] for line in symbols.splitlines()) != 1:
                raise RuntimeError(f"proof must define exactly one {symbol}")
        report["executable"] = {"path": str(executable), "sha256": digest(executable)}
        report["test_archive"] = {"path": str(archive), "sha256": digest(archive)}
        log = run("proof", [executable, TEST, "--exact", "--nocapture", "--test-threads=1"],
                  env=env, timeout=90)
        if "1 passed; 0 failed" not in log:
            raise RuntimeError("missing exact parent-test success")
        for scenario in ("abort", "drop"):
            for marker in ("DIAGNOSTIC_WITH_HEALTHY_CORES", "KNOWN_CORE_CLEANUP", "POINT_MAPPING_REJECTED"):
                if f"NATIVE_POISON_{marker} {scenario}" not in log:
                    raise RuntimeError(f"missing child proof marker {marker} {scenario}")
        report["passed"] = True
    finally:
        after = {str(path): digest(path) for path in sorted(inputs)}
        report["inputs_after"] = after
        report["inputs_unchanged"] = before == after
        report["passed"] = report["passed"] and report["inputs_unchanged"]
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(output / "report.json", flush=True)
    if not report["passed"]:
        raise RuntimeError("proof inputs changed during execution")


if __name__ == "__main__":
    main()
