#!/usr/bin/env python3
"""Reject missing or unreviewed constant behavior in canonical Rust sources."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib

SOURCE_ROOT = Path(__file__).resolve().parent.parent


def scanner_binary() -> Path:
    """Build the standalone parser with the repository's pinned lockfile."""
    target = SOURCE_ROOT / "target" / "rust-source-audit"
    scratch = target / "tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.update(CARGO_TARGET_DIR=str(target), TMPDIR=str(scratch))
    subprocess.run(
        ["cargo", "build", "--quiet", "--locked", "--offline", "--manifest-path",
         str(SOURCE_ROOT / "scripts" / "rust_source_audit" / "Cargo.toml")],
        env=environment, check=True,
    )
    return target / "debug" / "srpc-rust-source-audit"


def parse(binary: Path, mode: str, source: Path, namespace: str = "") -> dict:
    result = subprocess.run([str(binary), mode, str(source), namespace],
                            text=True, capture_output=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or f"AST parser failed for {source}")
    return json.loads(result.stdout)


def declaration_key(record: dict) -> str:
    key = f"{record['kind']}:{record['path']}"
    if record.get("presence"):
        key += " [" + record["presence"] + "]"
    return key


def digest(record: dict) -> str:
    return hashlib.sha256(record["tokens"].encode()).hexdigest()


def audit(repo: Path, binary: Path) -> list[str]:
    inventory = json.loads((repo / "scripts/canonical-constant-functions.json").read_text())
    if set(inventory) != {"schema_version", "functions"} or inventory["schema_version"] != 1:
        raise ValueError("invalid canonical constant-function inventory schema")
    expected = inventory["functions"]
    if not isinstance(expected, dict):
        raise ValueError("canonical constant-function inventory must be an object")
    manifest = tomllib.loads((repo / "rust-modules.toml").read_text())
    findings: list[str] = []
    seen: set[str] = set()
    for module in manifest["module"]:
        parsed = parse(binary, "canonical", repo / module["source"], module["cpp_module"])
        for record in parsed["declarations"]:
            for missing in record["missing_macros"]:
                findings.append(f"{module['source']}:{missing['line']}: missing canonical runtime behavior: {missing['name']}!")
            for function in record["constant_functions"]:
                key = f"{module['source']}:{record['path']}::{function['name']}"
                where = f"{module['source']}:{function['line']}"
                if key in seen:
                    findings.append(f"{where}: duplicate canonical constant function {key}")
                seen.add(key)
                entry = expected.get(key)
                if (not isinstance(entry, dict) or set(entry) != {"sha256", "reason"}
                        or not isinstance(entry["reason"], str) or not entry["reason"].strip()):
                    findings.append(f"{where}: unreviewed constant/empty canonical runtime function {key}")
                elif entry["sha256"] != digest(function):
                    findings.append(f"{where}: changed canonical constant function {key}; review actual behavior")
    for key in sorted(expected.keys() - seen):
        findings.append(f"stale canonical constant-function inventory entry {key}")
    return findings


def main() -> int:
    repo = Path(os.environ.get("SRPC_REPO", SOURCE_ROOT)).resolve()
    try:
        findings = audit(repo, scanner_binary())
    except (OSError, ValueError, KeyError) as error:
        findings = [str(error)]
    if findings:
        print("canonical Rust runtime audit FAILED:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("canonical Rust runtime audit passed: no missing behavior; constant functions reviewed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
