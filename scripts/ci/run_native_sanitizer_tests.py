#!/usr/bin/env python3
"""Run every selected native Rust test in its own instrumented process.

Only exact, inventoried quarantine cases disable leak reporting. They still
execute with compiler instrumentation and all other sanitizer checks enabled.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--package", action="append", required=True)
    parser.add_argument("--features")
    parser.add_argument("--all-features", action="store_true")
    parser.add_argument("--quarantine", action="append", type=Path, default=[])
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    address = "-Zsanitizer=address" in os.environ.get("RUSTFLAGS", "")
    quarantine = set()
    for path in args.quarantine:
        for line in path.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            package, suite, case = line.split("|")
            if package in args.package:
                identity = (package, suite, case)
                if identity in quarantine:
                    raise RuntimeError(f"repeated quarantine identity: {identity}")
                quarantine.add(identity)
    if address and "detect_leaks=1" not in os.environ.get("ASAN_OPTIONS", "").split(":"):
        raise RuntimeError("ASan requires detect_leaks=1 by default")
    command = [args.cargo, "test", "--locked", "--manifest-path", args.manifest,
               "--target", args.target, "--no-run", "--message-format=json"]
    for package in args.package:
        command += ["-p", package]
    if args.features:
        command += ["--features", args.features]
    if args.all_features:
        command.append("--all-features")
    built = subprocess.run(command, stdout=subprocess.PIPE, text=True, check=False)
    executables = {}
    for line in built.stdout.splitlines():
        event = json.loads(line)
        if event.get("reason") == "compiler-message":
            print(event["message"].get("rendered", ""), end="", file=sys.stderr)
        if event.get("reason") == "compiler-artifact" and event.get("profile", {}).get("test") and event.get("executable"):
            package = Path(event["manifest_path"]).parent.name
            library = any(kind in {"lib", "rlib", "staticlib", "cdylib", "dylib"}
                          for kind in event["target"]["kind"])
            suite = "lib" if library else event["target"]["name"]
            executables[event["executable"]] = (package, suite, str(Path(event["manifest_path"]).parent))
    if built.returncode:
        return built.returncode
    inventory = {}
    for executable, (package, suite, directory) in sorted(executables.items()):
        listed = subprocess.check_output([executable, "--list", "--format", "terse"], cwd=directory, text=True, timeout=180)
        for line in listed.splitlines():
            if line.endswith(": test"):
                identity = (package, suite, line.removesuffix(": test"))
                if identity in inventory:
                    raise RuntimeError(f"duplicate test identity: {identity}")
                inventory[identity] = (executable, directory)
    if not inventory or quarantine - inventory.keys():
        raise RuntimeError(f"missing tests or quarantine identities: {quarantine - inventory.keys()}")
    report = {"passed": False, "tests": [], "quarantine": [list(i) for i in sorted(quarantine)]}
    args.report.parent.mkdir(exist_ok=True, parents=True)
    try:
        for index, (identity, (executable, directory)) in enumerate(sorted(inventory.items())):
            environment = os.environ.copy()
            qualified = address and identity in quarantine
            if qualified:
                environment["ASAN_OPTIONS"] = environment["ASAN_OPTIONS"].replace("detect_leaks=1", "detect_leaks=0")
            result = subprocess.run([executable, identity[2], "--exact", "--include-ignored", "--test-threads=1"],
                                    cwd=directory, env=environment, text=True, capture_output=True, timeout=600)
            output = result.stdout + result.stderr
            log = args.report.with_name(f"{args.report.stem}-{index}.log")
            log.write_text(output)
            passed = result.returncode == 0 and "1 passed; 0 failed; 0 ignored" in result.stdout
            if re.search(r"ERROR: (AddressSanitizer|LeakSanitizer)|WARNING: ThreadSanitizer|runtime error:", output):
                passed = False
            report["tests"].append(dict(identity=identity, passed=passed, leak_qualified=qualified,
                                        exit_code=result.returncode, executable=executable, log=str(log)))
            if not passed:
                print(output, end="", file=sys.stderr)
                raise RuntimeError(f"sanitizer test failed: {identity}, see {log}")
            print(f"PASS {'|'.join(identity)}" + (" (intentional quarantine, leak reporting disabled)" if qualified else ""), flush=True)
        report["passed"] = True
    finally:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Native sanitizer tests: {len(inventory)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
