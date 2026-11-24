#!/usr/bin/env python3
"""Reject non-storage C++ in the native Rust production build graph."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    allowed = {
        (root / line.strip()).resolve()
        for line in (root / "cmake/native-storage-sources.txt").read_text().splitlines()
        if line.strip() and not line.startswith("#")
    }
    entries = json.loads((args.build / "compile_commands.json").read_text())
    observed = set()
    unexpected = set()
    for entry in entries:
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        path = path.resolve()
        # Clang's standard-library module is a toolchain dependency, not a
        # provider generated from project Rust. Only those exact names qualify.
        if not path.is_relative_to(root) and path.name in {"std.cppm", "std.compat.cppm"}:
            continue
        if path not in allowed:
            unexpected.add(str(path))
        observed.add(path)
    if unexpected or allowed - observed:
        for path in sorted(unexpected):
            print(f"unexpected native production source: {path}", file=sys.stderr)
        for path in sorted(allowed - observed):
            print(f"missing storage source: {path}", file=sys.stderr)
        return 1
    if args.archive:
        symbols = subprocess.check_output(
            [args.nm, "--demangle", "--defined-only", str(args.archive)],
            text=True, stderr=subprocess.DEVNULL)
        forbidden = ("TThread::", "EpochRuntime::", "ShardClient::", "ShardReceiver::",
                     "srpc::", "janus::", "rust_sto_tpcc_wrapper::")
        for symbol in symbols.splitlines():
            if any(name in symbol for name in forbidden):
                print(f"application symbol in storage archive: {symbol}", file=sys.stderr)
                return 1
    print(f"native source audit: {len(observed)} storage sources; no application or generated C++")
    return 0


if __name__ == "__main__":
    sys.exit(main())
