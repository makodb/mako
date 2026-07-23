#!/usr/bin/env python3
"""Inventory Raft declarations for the RustyCpp migration plan.

The scanner is intentionally conservative and textual. It is a map for
planning, not a replacement for C++ parsing. Re-run it after adding or
removing declarations and review bucket changes before acting on them.
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RAFT_DIR = ROOT / "src" / "deptran" / "raft"
DEFAULT_MD = ROOT / "docs" / "migration" / "rustycpp" / "raft-inventory.md"
DEFAULT_CSV = ROOT / "docs" / "migration" / "rustycpp" / "raft-inventory.csv"
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".cc", ".cpp", ".cxx"}

DECL_RE = re.compile(
    r"^(?P<prefix>pub\s+)?(?P<kind>class|struct|enum(?:\s+class)?|union|using|typedef)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
)
FN_RE = re.compile(r"^pub\s+fn\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)", re.MULTILINE)
RUST_DECL_RE = re.compile(
    r"\b(?:pub\s+)?(?:struct|enum|trait)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
)


@dataclass
class Declaration:
    file: str
    line: int
    kind: str
    name: str
    bucket: str
    status: str
    blockers: str
    rust_blocks: int


def rust_regions(text: str) -> list[str]:
    regions: list[str] = []
    start = 0
    while True:
        begin = text.find("#if RUSTYCPP_RUST", start)
        if begin < 0:
            return regions
        end = text.find("#endif", begin)
        if end < 0:
            regions.append(text[begin:])
            return regions
        regions.append(text[begin:end])
        start = end + len("#endif")


def without_generated_regions(text: str) -> str:
    """Blank GEN regions while preserving line numbers for source scanning."""
    lines = text.splitlines(keepends=True)
    masked: list[str] = []
    in_gen = False
    for line in lines:
        if line.startswith("/*RUSTYCPP:GEN-BEGIN"):
            in_gen = True
        if in_gen:
            masked.append("".join("\n" if char == "\n" else " " for char in line))
        else:
            masked.append(line)
        if line.startswith("/*RUSTYCPP:GEN-END"):
            in_gen = False
    return "".join(masked)


def blocker_names(text: str) -> list[str]:
    blockers: list[str] = []
    if re.search(r"\bvoid\s*\*", text):
        blockers.append("void*")
    if re.search(r"\bva_list\b", text):
        blockers.append("va_list")
    if re.search(r"\b(?:std::)?(?:array|CArray)\s*<", text):
        blockers.append("C-array")
    if re.search(r"\btemplate\s*<[^>]*>\s*(?:class|struct|[A-Za-z_].*\([^;{}]*\))", text, re.S):
        blockers.append("template")
    if re.search(r"(?:operator\s*<<|operator\s*>>|operator\s*\[\]|operator\s*\()", text):
        blockers.append("operator-overload")
    if re.search(r"\b[A-Za-z_][A-Za-z0-9_:<>]*\s*\*\s*[A-Za-z_]", text):
        blockers.append("raw-pointer")
    if re.search(r"\b(?:std::)?(?:mutex|recursive_mutex|condition_variable|thread)\b", text):
        blockers.append("threading")
    if re.search(r"\b(?:open|read|write|close|fstat|memcpy|rocksdb_)\b", text):
        blockers.append("I/O-or-FFI")
    return blockers


def classify(kind: str, body: str, rust_covered: bool, blockers: list[str]) -> tuple[str, str]:
    if rust_covered:
        return "already-DSL", "migrated"
    if any(item in blockers for item in ("void*", "va_list", "C-array", "template", "operator-overload")):
        return "needs-transpiler", "unmigrated"
    if kind in {"enum", "enum class", "using", "typedef"}:
        return "trivial", "unmigrated"
    if kind in {"class", "struct", "union"}:
        if re.search(r"\bvirtual\b|\s:\s*(?:public|protected|private)?\s*[A-Za-z_]", body):
            return "refactor-then-DSL", "unmigrated"
        if blockers:
            return "refactor-then-DSL", "unmigrated"
        return "trivial", "unmigrated"
    return "boundary", "unmigrated"


def scan() -> list[Declaration]:
    rows: list[Declaration] = []
    for path in sorted(RAFT_DIR.iterdir()):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        regions = rust_regions(text)
        rust_names = {match.group("name") for region in regions for match in RUST_DECL_RE.finditer(region)}
        rust_names.update(match.group("name") for region in regions for match in FN_RE.finditer(region))
        lines = without_generated_regions(text).splitlines()
        seen: set[tuple[str, str]] = set()
        for number, line in enumerate(lines, 1):
            match = DECL_RE.match(line.strip())
            if not match:
                fn_match = FN_RE.match(line.strip())
                if not fn_match:
                    continue
                kind, name = "fn", fn_match.group("name")
            else:
                kind, name = match.group("kind"), match.group("name")
            key = (kind, name)
            if key in seen:
                continue
            # Forward declarations do not represent migration units. Keep
            # aliases, whose complete declaration is one line, in the map.
            if kind not in {"using", "typedef", "fn"} and line.strip().endswith(";"):
                continue
            seen.add(key)
            start = max(0, number - 1)
            end = min(len(lines), start + 180)
            body = "\n".join(lines[start:end])
            blockers = blocker_names(body)
            bucket, status = classify(kind, body, name in rust_names, blockers)
            rows.append(Declaration(
                file=str(path.relative_to(ROOT)),
                line=number,
                kind=kind,
                name=name,
                bucket=bucket,
                status=status,
                blockers=", ".join(blockers) or "none",
                rust_blocks=len(regions),
            ))
    return rows


def write_csv(rows: list[Declaration], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["file", "line", "kind", "name", "bucket", "status", "blockers", "rust_blocks"])
        for row in rows:
            writer.writerow([row.file, row.line, row.kind, row.name, row.bucket, row.status, row.blockers, row.rust_blocks])


def write_markdown(rows: list[Declaration], path: Path, csv_path: Path) -> None:
    buckets = Counter(row.bucket for row in rows)
    statuses = Counter(row.status for row in rows)
    blockers = Counter(blocker for row in rows for blocker in row.blockers.split(", ") if blocker != "none")
    files = sorted({row.file for row in rows})
    lines = [
        "# Raft RustyCpp Inventory",
        "",
        "This report is generated by `scripts/raft_rustcpp_inventory.py`. The scanner is a conservative textual triage tool; review its bucket assignment before using it as a migration decision.",
        "",
        "## Regeneration",
        "",
        "```bash",
        "python3 scripts/raft_rustcpp_inventory.py",
        "```",
        "",
        f"The per-declaration CSV is regenerated at `{csv_path.relative_to(ROOT)}` and is ignored by the repository's global `*.csv` rule.",
        "",
        "## Scope",
        "",
        f"- Files scanned: {len(files)}",
        f"- Declarations scanned: {len(rows)}",
        f"- DSL-covered declarations/functions: {statuses.get('migrated', 0)}",
        f"- Unmigrated declarations/functions: {statuses.get('unmigrated', 0)}",
        "",
        "## Declaration Buckets",
        "",
        "| Bucket | Count | Meaning |",
        "| --- | ---: | --- |",
        f"| `already-DSL` | {buckets.get('already-DSL', 0)} | Rust DSL source exists and has a generated region |",
        f"| `trivial` | {buckets.get('trivial', 0)} | POD, enum, alias, or simple value candidate |",
        f"| `refactor-then-DSL` | {buckets.get('refactor-then-DSL', 0)} | Needs ownership, inheritance, threading, or API reshaping first |",
        f"| `needs-transpiler` | {buckets.get('needs-transpiler', 0)} | Contains a guide-listed syntax blocker |",
        f"| `boundary` | {buckets.get('boundary', 0)} | No safe automatic bucket assignment; inspect manually |",
        "",
        "## Blocker Histogram",
        "",
        "| Blocker | Count |",
        "| --- | ---: |",
    ]
    if blockers:
        lines.extend(f"| `{name}` | {count} |" for name, count in sorted(blockers.items()))
    else:
        lines.append("| none detected | 0 |")
    lines += ["", "## Per-Declaration Status", "", "| File | Line | Kind | Declaration | Bucket | Status | Blockers |", "| --- | ---: | --- | --- | --- | --- | --- |"]
    lines.extend(
        f"| `{row.file}` | {row.line} | `{row.kind}` | `{row.name}` | `{row.bucket}` | `{row.status}` | {row.blockers} |"
        for row in rows
    )
    lines += [
        "",
        "## Interpretation",
        "",
        "`already-DSL` means only that a matching Rust source declaration was found. It does not mean the surrounding class is fully migrated; inspect the C++ class and its runtime boundaries separately.",
        "",
        "Raw pointers, threading, I/O, RocksDB, RPC, callbacks, and filesystem work are recorded as blockers or boundaries and should normally remain in C++ bridges unless a focused transpiler probe proves a safe shape.",
        "",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--markdown", type=Path, default=DEFAULT_MD)
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    args = parser.parse_args()
    rows = scan()
    write_csv(rows, args.csv)
    write_markdown(rows, args.markdown, args.csv)
    print(f"wrote {args.markdown.relative_to(ROOT)}")
    print(f"wrote {args.csv.relative_to(ROOT)}")
    print(f"scanned {len(rows)} declarations")


if __name__ == "__main__":
    main()
