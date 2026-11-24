#!/usr/bin/env python3
"""Run native storage tests with fresh process-wide runtime state per case.

Several existing tests intentionally poison native storage or negotiate
incompatible runtime limits. The old CMake runner already ran them in separate
processes. Discover Cargo's actual test executables once and retain that contract
without invoking Cargo again for every case.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--package", action="append", required=True)
    parser.add_argument("--features")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    command = [args.cargo, "test", "--locked", "--manifest-path", args.manifest,
               "--no-run", "--message-format=json"]
    for package in args.package:
        command.extend(["-p", package])
    if args.features:
        command.extend(["--features", args.features])
    built = subprocess.run(command, stdout=subprocess.PIPE, text=True, check=False)
    executables = set()
    for line in built.stdout.splitlines():
        event = json.loads(line)
        if event.get("reason") == "compiler-message":
            print(event["message"].get("rendered", ""), end="", file=sys.stderr)
        if event.get("reason") == "compiler-artifact" and event.get("profile", {}).get("test"):
            executable = event.get("executable")
            if executable:
                executables.add(executable)
    if built.returncode:
        return built.returncode
    count = 0
    for executable in sorted(executables):
        listing = subprocess.check_output([executable, "--list", "--format", "terse"],
                                          text=True, timeout=args.timeout)
        cases = [line.removesuffix(": test") for line in listing.splitlines()
                 if line.endswith(": test")]
        for case in cases:
            result = subprocess.run([executable, case, "--exact", "--include-ignored",
                                     "--test-threads=1"], capture_output=True,
                                    text=True, timeout=args.timeout)
            if result.returncode:
                print(result.stdout, end="")
                print(result.stderr, end="", file=sys.stderr)
                return result.returncode
            # Exact test names come from this same binary; insist that a real
            # test ran so an argument mismatch cannot produce a false pass.
            if "1 passed; 0 failed; 0 ignored" not in result.stdout:
                raise RuntimeError(f"test did not run: {executable} {case}\n{result.stdout}")
            print(f"PASS {Path(executable).name}::{case}", flush=True)
            count += 1
    if not count:
        raise RuntimeError("Cargo produced no runnable native tests")
    print(f"native Rust tests: {count} passed in fresh processes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
