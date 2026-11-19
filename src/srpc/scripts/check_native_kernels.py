#!/usr/bin/env python3
"""Pin reviewed native leaves and the common Rust/C++ compilation manifest.

C files own OS layouts, errno, clock/entropy access, and context-switch storage.
Rust owns SRPC policy. This gate intentionally does not try to classify arbitrary
C algorithms: every native body and header is reviewed and pinned, and any drift
fails closed. The repository-root import headers are reviewed too. Adding
handwritten C++ at the root or under a canonical source directory is always an
error. There is no inventory regeneration option.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
DIRECTORIES = ("base", "misc", "rpc", "reactor")
NATIVE_SUFFIXES = {".c", ".h", ".hpp", ".S", ".cc", ".cpp", ".cxx", ".cppm", ".ixx", ".hh", ".hxx", ".ipp", ".inc"}
FORBIDDEN_SUFFIXES = {".cc", ".cpp", ".cxx", ".cppm", ".ixx"}
ROLES = {
    "os-leaf": "Individual OS/libc operation, ABI layout, errno, or resource allocation.",
    "clock": "Native clock reads and local-calendar field extraction; canonical Rust formats time.",
    "entropy": "Native thread-local random seed and raw entropy draw; canonical Rust owns range policy.",
    "context-switch": "Machine registers, guarded stack allocation, TLS active context, and stack transfer.",
    "c-declarations": "Declarations and C layouts for reviewed native leaves.",
    "import-shim": "Legacy include compatibility that imports generated canonical modules.",
    "trait-dispatch": "C++ ADL or erased trait forwarding and native declarations; no serialization algorithm.",
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def native_paths(repo: Path) -> list[Path]:
    # Root headers are part of the public C++ include path. Scan all root
    # carriers so a new file cannot bypass the reviewed import-shim inventory.
    paths = list(repo.iterdir())
    for directory in DIRECTORIES:
        paths.extend((repo / directory).rglob("*"))
    return sorted(path for path in paths if path.suffix in NATIVE_SUFFIXES)


def consumer_hashes(repo: Path) -> dict[str, str]:
    cmake = (repo / "CMakeLists.txt").read_text()
    begin = "# SRPC_NATIVE_MANIFEST_BEGIN\n"
    end = "# SRPC_NATIVE_MANIFEST_END"
    if cmake.count(begin) != 1 or cmake.count(end) != 1:
        raise ValueError("missing/duplicate CMake native manifest block")
    block = cmake.split(begin, 1)[1].split(end, 1)[0]
    attachments = re.findall(r"target_sources\s*\(\s*srpc\s+PRIVATE\b[^)]*\)", cmake)
    if attachments != ["target_sources(srpc PRIVATE ${SRPC_NATIVE_SOURCES})"]:
        raise ValueError("CMake production native sources must come only from the common manifest")
    return {"build.rs": sha256((repo / "build.rs").read_bytes()),
            "CMakeLists.txt:native-manifest": sha256(block.encode())}


def audit(repo: Path) -> list[str]:
    inventory = json.loads((repo / "scripts" / "native-kernels.json").read_text())
    findings = []
    if set(inventory) != {"schema_version", "roles", "files", "sources", "consumers"}:
        findings.append("invalid native kernel inventory fields")
    if inventory.get("schema_version") != 1 or inventory.get("roles") != ROLES:
        findings.append("invalid native kernel inventory schema/roles")
    expected = inventory["files"]
    seen = set()
    for path in native_paths(repo):
        relative = path.relative_to(repo).as_posix()
        seen.add(relative)
        if path.is_symlink():
            findings.append(f"native source symlink is forbidden: {relative}")
        if path.suffix in FORBIDDEN_SUFFIXES:
            findings.append(f"handwritten C++ implementation carrier is forbidden: {relative}")
        if relative not in expected:
            findings.append(f"unreviewed native source/header: {relative}")
            continue
        entry = expected[relative]
        if set(entry) != {"role", "sha256"} or entry["role"] not in ROLES:
            findings.append(f"invalid native kernel inventory entry: {relative}")
        elif entry["sha256"] != sha256(path.read_bytes()):
            findings.append(f"changed native kernel/header: {relative}; review canonical Rust ownership")
    for missing in sorted(set(expected) - seen):
        findings.append(f"stale native kernel inventory entry: {missing}")

    sources = {}
    for line in (repo / "scripts" / "native-kernel-sources.txt").read_text().splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[0] not in {"all", "x86_64", "aarch64"}:
            raise ValueError(f"invalid native source manifest record: {line}")
        architecture, source = fields
        if source in sources:
            findings.append(f"duplicate native source manifest entry: {source}")
        sources[source] = architecture
        if source not in expected or Path(source).suffix not in {".c", ".S"}:
            findings.append(f"unreviewed compiled native source: {source}")
    if sources != inventory["sources"]:
        findings.append("native source manifest differs from reviewed kernel inventory")
    compiled = {path for path in seen if Path(path).suffix in {".c", ".S"}}
    if set(sources) != compiled:
        findings.append("native manifest must list every production C/assembly source exactly once")
    if consumer_hashes(repo) != inventory["consumers"]:
        findings.append("native manifest consumer changed; verify identical Rust/C++ kernel linkage")
    return findings


def main() -> int:
    repo = Path(os.environ.get("SRPC_REPO", ROOT)).resolve()
    try:
        findings = audit(repo)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"native kernel audit FAILED: {error}", file=sys.stderr)
        return 1
    if findings:
        print("native kernel audit FAILED:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("native kernel audit passed: reviewed leaves; Rust and C++ share one source manifest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
