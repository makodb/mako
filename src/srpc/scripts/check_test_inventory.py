#!/usr/bin/env python3
"""Check C++ source dispositions, Rust coverage references, and CMake registration."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def check(repo: Path, inventory: dict, configured: str | None = None,
          testing: bool = True) -> None:
    if inventory.get("version") != 1:
        raise ValueError("unsupported test inventory version")
    entries: dict[str, tuple[str, str]] = {}
    for kind in ("runtime", "docs", "support", "benchmark"):
        for source, target in inventory[kind].items():
            if source in entries:
                raise ValueError(f"duplicate source: {source}")
            if not re.fullmatch(r"[A-Za-z0-9_]+", target):
                raise ValueError(f"invalid target: {target}")
            entries[source] = (kind, target)
    for group in inventory["excluded"]:
        if not group["reason"].strip():
            raise ValueError("exclusion needs a reason")
        if group["disposition"] not in ("rust", "upstream", "historical-benchmark", "retired-api"):
            raise ValueError("unknown exclusion disposition")
        if group["disposition"] == "rust" and not group["rust_cases"]:
            raise ValueError("Rust replacement needs named tests")
        for case in group["rust_cases"]:
            path, name = case.split("::", 1)
            if not re.fullmatch(r"tests/[A-Za-z0-9_]+_rust\.rs", path):
                raise ValueError(f"not a Cargo integration test: {path}")
            text = (repo / path).read_text()
            pattern = rf"#\[test\](?:\s*#\[[^\n]*\])*\s*fn\s+{re.escape(name)}\s*\("
            if not re.search(pattern, text):
                raise ValueError(f"missing Rust test: {case}")
        for target in group.get("cpp_replacements", []):
            if target not in inventory["runtime"].values():
                raise ValueError(f"missing C++ replacement: {target}")
        for source in group["sources"]:
            if source in entries:
                raise ValueError(f"duplicate source: {source}")
            entries[source] = ("excluded", "")
    actual = {str(p.relative_to(repo)) for p in (repo / "tests").rglob("*.cc")}
    if actual != entries.keys():
        raise ValueError(f"C++ test inventory mismatch: unaccounted={sorted(actual - entries.keys())}; "
                         f"stale={sorted(entries.keys() - actual)}")
    if configured is None:
        return
    found: dict[str, set[tuple[str, bool, bool]]] = {}
    for line in configured.splitlines():
        source, target, registered, default_build = line.split("|")
        found.setdefault(source, set()).add((target, registered == "yes", default_build == "yes"))
    for source, (kind, target) in entries.items():
        if kind == "excluded":
            if source in found:
                raise ValueError(f"excluded source unexpectedly built: {source}")
            continue
        if not testing and source not in found and (
            kind in ("runtime", "docs") or target in inventory["runtime"].values()
        ):
            continue
        expected_test = testing and kind in ("runtime", "docs")
        # Support TUs may belong to a registered executable, e.g. fd-reuse tests.
        if kind == "support":
            if not any(t == target for t, _, _ in found.get(source, set())):
                raise ValueError(f"missing configured source: {source} -> {target}")
        elif not any(t == target and registered == expected_test and
                     (default_build or kind == "benchmark")
                     for t, registered, default_build in found.get(source, set())):
            raise ValueError(f"missing target or CTest registration: {source} -> {target}")
    if found.keys() - entries.keys():
        raise ValueError("configured C++ test source missing from inventory")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--configured", type=Path)
    parser.add_argument("--no-testing", action="store_true")
    args = parser.parse_args()
    try:
        inventory = json.loads((args.repo / "tests/test-inventory.json").read_text())
        check(args.repo, inventory,
              args.configured.read_text() if args.configured else None,
              not args.no_testing)
    except (ValueError, KeyError, OSError) as error:
        print(f"test inventory: {error}")
        return 1
    built = sum(len(inventory[k]) for k in ("runtime", "docs", "support", "benchmark"))
    excluded = sum(len(g["sources"]) for g in inventory["excluded"])
    print(f"test inventory: {built} C++ sources assigned to targets; {excluded} explicit exclusions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
