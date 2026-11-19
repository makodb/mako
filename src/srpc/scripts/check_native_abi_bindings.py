#!/usr/bin/env python3
"""Pin canonical Rust declarations of native C types and their C++ names."""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tomllib

import rust_source_audit

ROOT = Path(__file__).resolve().parent.parent


def audit(repo: Path, binary: Path) -> list[str]:
    inventory = json.loads((repo / "scripts/native-abi-bindings.json").read_text())
    if set(inventory) != {"schema_version", "bindings"} or inventory["schema_version"] != 1:
        raise ValueError("invalid native ABI binding inventory")
    expected = inventory["bindings"]
    type_map = tomllib.loads((repo / "rust-type-map.toml").read_text())
    preambles = tomllib.loads((repo / "module-preambles.toml").read_text())
    headers = {row["name"]: {entry["path"] for entry in row.get("includes", [])}
               for row in preambles["module"]}
    modules = tomllib.loads((repo / "rust-modules.toml").read_text())["module"]
    findings: list[str] = []
    seen: set[str] = set()
    for module in modules:
        parsed = rust_source_audit.parse(binary, "canonical", repo / module["source"], module["cpp_module"])
        for record in parsed["declarations"]:
            if not record["native_binding"]:
                continue
            key = module["source"] + ":" + rust_source_audit.declaration_key(record)
            if key in seen:
                findings.append(f"duplicate native ABI binding: {key}")
            seen.add(key)
            entry = expected.get(key)
            if not isinstance(entry, dict) or set(entry) != {"sha256", "c_type", "header"}:
                findings.append(f"unreviewed native ABI binding: {key}")
                continue
            if rust_source_audit.digest(record) != entry["sha256"]:
                findings.append(f"changed native ABI declaration: {key}")
            if type_map.get(record["name"]) != entry["c_type"]:
                findings.append(f"changed native C type map: {key}")
            if entry["header"] not in headers.get(module["cpp_module"], set()):
                findings.append(f"missing native owning header: {key}")
    for key in sorted(expected.keys() - seen):
        findings.append(f"stale native ABI binding: {key}")
    return findings


def main() -> int:
    repo = Path(os.environ.get("SRPC_REPO", ROOT)).resolve()
    try:
        findings = audit(repo, rust_source_audit.scanner_binary())
    except (OSError, ValueError, KeyError) as error:
        findings = [str(error)]
    if findings:
        print("native ABI binding audit FAILED:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("native ABI binding audit passed: reviewed Rust C layouts, type maps, and owning headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
