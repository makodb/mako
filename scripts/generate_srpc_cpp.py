#!/usr/bin/env python3
"""Generate Mako-facing C++ modules from the srpc Rust crate.

The checked-in C++ is a build artifact: C++-only builders compile it without
installing rustc.  Regeneration is deliberately stricter.  It requires the
manifest-pinned rusty-cpp checkout and exact emitter binary, validates the
real Rust crate, rejects incomplete/TODO lowering, and compares complete bytes
in ``--check`` mode.  ``--check-stamps`` performs an offline integrity check.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib


SCHEMA_VERSION = 2
GENERATOR_VERSION = 2
CANONICAL_TRANSPILER_SHA256 = (
    "65f10285f4954c422c7a0968197c118025a0d8676d20a32f24edc4b2f68d1782"
)
DEFAULT_MANIFEST = Path("crates/srpc/cpp/mako-consumer.toml")
DEFAULT_TRANSPILER = Path(
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
HAND_SLOT_PREFIXES = (
    "// TODO transpiler",
    "/* TODO transpiler",
    "// TODO(",
    "// TODO:",
    "// TODO ",
)


class ProfileError(RuntimeError):
    """A deterministic consumer-profile invariant was violated."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ProfileError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def run_checked(argv: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        argv,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "no diagnostics"
        raise ProfileError(
            f"command failed ({proc.returncode}): {' '.join(argv)}\n{detail}"
        )
    return proc


def load_manifest(path: Path, root: Path) -> tuple[dict, bytes]:
    path = path.resolve()
    if not within(path, root):
        raise ProfileError("profile manifest must remain inside the repository")
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ProfileError(f"cannot read manifest {path}: {exc}") from exc
    try:
        data = tomllib.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise ProfileError(f"invalid manifest {path}: {exc}") from exc

    allowed_top = {
        "schema_version",
        "profile",
        "crate_root",
        "output_root",
        "cxx_namespace",
        "transpiler_git",
        "transpiler_sha256",
        "module",
    }
    unknown = sorted(set(data) - allowed_top)
    if unknown:
        raise ProfileError(f"unknown manifest keys: {', '.join(unknown)}")
    if data.get("schema_version") != SCHEMA_VERSION:
        raise ProfileError(
            f"schema_version must be {SCHEMA_VERSION}, got {data.get('schema_version')!r}"
        )
    for key in ("profile", "crate_root", "output_root", "cxx_namespace"):
        if not isinstance(data.get(key), str) or not data[key]:
            raise ProfileError(f"manifest key {key!r} must be a non-empty string")
        if "\n" in data[key] or "\r" in data[key]:
            raise ProfileError(f"manifest key {key!r} must fit on one line")
    pin = data.get("transpiler_git")
    if not isinstance(pin, str) or not re.fullmatch(r"[0-9a-f]{40}", pin):
        raise ProfileError("transpiler_git must be a full 40-character lowercase SHA")
    binary_pin = data.get("transpiler_sha256")
    if binary_pin != CANONICAL_TRANSPILER_SHA256:
        raise ProfileError(
            "transpiler_sha256 must be the canonical Mako emitter SHA256 "
            f"{CANONICAL_TRANSPILER_SHA256}"
        )
    modules = data.get("module")
    if not isinstance(modules, list) or not modules:
        raise ProfileError("manifest must contain at least one [[module]] entry")

    crate_dir = (root / data["crate_root"]).resolve()
    output_dir = (root / data["output_root"]).resolve()
    if not within(crate_dir, root) or not within(output_dir, root):
        raise ProfileError("crate_root and output_root must remain inside the repository")
    data["_crate_dir"] = crate_dir
    data["_output_dir"] = output_dir

    allowed_module = {"source", "module_name", "output", "kind", "gmf_headers"}
    seen_sources: set[str] = set()
    seen_modules: set[str] = set()
    seen_outputs: set[str] = set()
    for index, entry in enumerate(modules, 1):
        if not isinstance(entry, dict):
            raise ProfileError(f"module entry {index} must be a table")
        unknown = sorted(set(entry) - allowed_module)
        if unknown:
            raise ProfileError(
                f"module entry {index} has unknown keys: {', '.join(unknown)}"
            )
        for key in ("source", "module_name", "output", "kind"):
            if not isinstance(entry.get(key), str) or not entry[key]:
                raise ProfileError(
                    f"module entry {index} key {key!r} must be a non-empty string"
                )
            if "\n" in entry[key] or "\r" in entry[key]:
                raise ProfileError(
                    f"module entry {index} key {key!r} must fit on one line"
                )
        if entry["kind"] not in {"interface", "implementation"}:
            raise ProfileError(
                f"module entry {index} kind must be interface or implementation"
            )
        headers = entry.get("gmf_headers", [])
        if not isinstance(headers, list) or not all(isinstance(h, str) for h in headers):
            raise ProfileError(f"module entry {index} gmf_headers must be a string array")
        for header in headers:
            if not re.fullmatch(r'<[^<>\n]+>|"[^"\n]+"', header):
                raise ProfileError(
                    f"module entry {index} has invalid GMF header {header!r}"
                )
        entry["gmf_headers"] = headers

        source = (crate_dir / entry["source"]).resolve()
        output = (output_dir / entry["output"]).resolve()
        if not within(source, crate_dir):
            raise ProfileError(f"module entry {index} source escapes crate_root")
        if not within(output, output_dir):
            raise ProfileError(f"module entry {index} output escapes output_root")
        if Path(entry["source"]).suffix != ".rs":
            raise ProfileError(f"module entry {index} source must end in .rs")
        expected_suffix = ".cppm" if entry["kind"] == "interface" else ".cpp"
        if Path(entry["output"]).suffix != expected_suffix:
            raise ProfileError(
                f"module entry {index} {entry['kind']} output must end in {expected_suffix}"
            )
        if not re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*", entry["module_name"]):
            raise ProfileError(
                f"module entry {index} has invalid C++ module name {entry['module_name']!r}"
            )
        for label, value, seen in (
            ("source", entry["source"], seen_sources),
            ("module_name", entry["module_name"], seen_modules),
            ("output", entry["output"], seen_outputs),
        ):
            if value in seen:
                raise ProfileError(f"duplicate module {label}: {value}")
            seen.add(value)
        entry["_source"] = source
        entry["_output"] = output
    return data, raw


def verify_transpiler(
    root: Path, executable: Path, expected_git: str, expected_sha256: str
) -> tuple[str, str]:
    executable = executable.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ProfileError(f"transpiler is not executable: {executable}")
    actual_sha256 = sha256_file(executable)
    if actual_sha256 != expected_sha256:
        raise ProfileError(
            "transpiler binary pin mismatch: "
            f"manifest={expected_sha256}, executable={actual_sha256}"
        )
    checkout = (root / "third-party/rusty-cpp").resolve()
    actual = run_checked(["git", "rev-parse", "HEAD"], cwd=checkout).stdout.strip()
    if actual != expected_git:
        raise ProfileError(
            f"rusty-cpp pin mismatch: manifest={expected_git}, checkout={actual}"
        )
    tracked = run_checked(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=checkout
    ).stdout.strip()
    if tracked:
        raise ProfileError("rusty-cpp has tracked local changes; refusing an unpinned emit")
    return actual, actual_sha256


def stage_transpiler(executable: Path, destination: Path, expected_sha256: str) -> Path:
    """Copy the verified emitter so the executed bytes cannot change mid-run."""
    staged = destination / "rusty-cpp-transpiler"
    try:
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(executable.resolve(), staged)
        staged.chmod(0o500)
    except OSError as exc:
        raise ProfileError(f"cannot stage transpiler {executable}: {exc}") from exc
    actual = sha256_file(staged)
    if actual != expected_sha256:
        raise ProfileError(
            f"staged transpiler pin mismatch: expected={expected_sha256}, actual={actual}"
        )
    return staged


def hand_slots(cpp: str) -> list[tuple[int, str]]:
    slots: list[tuple[int, str]] = []
    for line_no, line in enumerate(cpp.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith(HAND_SLOT_PREFIXES):
            slots.append((line_no, stripped))
        elif "/* TODO transpiler" in line:
            slots.append((line_no, stripped))
        elif (
            stripped.startswith("// Rust-only")
            or stripped.startswith("// #[cfg(test)]")
        ) and ("skipped" in stripped or "omitted" in stripped):
            slots.append((line_no, stripped))
    return slots


def inject_gmf_headers(cpp: str, headers: list[str]) -> str:
    if not headers:
        return cpp
    needle = "module;\n\n"
    if cpp.count(needle) != 1:
        raise ProfileError("emitter output has no unique global-module-fragment opener")
    includes = "".join(f"#include {header}\n" for header in headers)
    return cpp.replace(needle, f"module;\n\n{includes}\n", 1)


def apply_unit_kind(cpp: str, module_name: str, kind: str) -> str:
    interface = f"export module {module_name};"
    if cpp.count(interface) != 1:
        raise ProfileError(
            f"emitter output does not contain exactly one {interface!r} declaration"
        )
    if kind == "interface":
        return cpp
    cpp = cpp.replace(interface, f"module {module_name};", 1)
    return re.sub(r"(?m)^export (?!(?:module)\b)", "", cpp)


def stamp_fields(
    *,
    manifest_path: Path,
    manifest_sha: str,
    profile: str,
    source_label: str,
    source_sha: str,
    module_name: str,
    kind: str,
    transpiler_git: str,
    transpiler_sha256: str,
    root: Path,
) -> list[tuple[str, str]]:
    manifest_label = manifest_path.resolve().relative_to(root).as_posix()
    return [
        ("generator-version", str(GENERATOR_VERSION)),
        ("profile", profile),
        ("profile-manifest", manifest_label),
        ("profile-sha256", manifest_sha),
        ("source", source_label),
        ("source-sha256", source_sha),
        ("transpiler-git", transpiler_git),
        ("transpiler-sha256", transpiler_sha256),
        ("module", module_name),
        ("unit-kind", kind),
    ]


def render_stamp(fields: list[tuple[str, str]]) -> bytes:
    return "".join(f"// srpc-cpp-{key}: {value}\n" for key, value in fields).encode()


def stamp_output(cpp: str, fields: list[tuple[str, str]]) -> bytes:
    """Stamp output with a checksum over every byte except its own hash line."""
    body = cpp.lstrip("\n").encode("utf-8")
    unhashed = render_stamp(fields) + body
    integrity = sha256_bytes(unhashed)
    return render_stamp(fields + [("output-sha256", integrity)]) + body


def generate_one(
    entry: dict,
    manifest: dict,
    manifest_path: Path,
    manifest_sha: str,
    transpiler: Path,
    transpiler_git: str,
    transpiler_sha256: str,
    root: Path,
    scratch: Path,
    source_bytes: bytes,
    staged_source: Path,
) -> bytes:
    source: Path = entry["_source"]
    raw_output = scratch / entry["output"]
    raw_output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(transpiler),
        str(staged_source),
        "-o",
        str(raw_output),
        "-m",
        entry["module_name"],
        "--cxx-namespace",
        manifest["cxx_namespace"],
    ]
    run_checked(command, cwd=root)
    try:
        cpp = raw_output.read_text(encoding="utf-8")
    except OSError as exc:
        raise ProfileError(f"transpiler did not produce {raw_output}: {exc}") from exc

    slots = hand_slots(cpp)
    if slots:
        rendered = "\n".join(f"  {line}: {text}" for line, text in slots)
        raise ProfileError(
            f"hand-override slot(s) in {entry['module_name']}:\n{rendered}"
        )
    cpp = inject_gmf_headers(cpp, entry["gmf_headers"])
    cpp = apply_unit_kind(cpp, entry["module_name"], entry["kind"])
    if entry["kind"] == "interface":
        exported = [
            line
            for line in cpp.splitlines()
            if line.startswith("export ") and not line.startswith("export module ")
        ]
        if not exported:
            raise ProfileError(
                f"{entry['module_name']} exports no Rust items; mark its public API `pub`"
            )
    source_label = source.relative_to(root).as_posix()
    fields = stamp_fields(
        manifest_path=manifest_path,
        manifest_sha=manifest_sha,
        profile=manifest["profile"],
        source_label=source_label,
        source_sha=sha256_bytes(source_bytes),
        module_name=entry["module_name"],
        kind=entry["kind"],
        transpiler_git=transpiler_git,
        transpiler_sha256=transpiler_sha256,
        root=root,
    )
    if not cpp.endswith("\n"):
        cpp += "\n"
    return stamp_output(cpp, fields)


def snapshot_sources(manifest: dict) -> dict[Path, bytes]:
    snapshots: dict[Path, bytes] = {}
    for entry in manifest["module"]:
        source: Path = entry["_source"]
        if not source.is_file():
            raise ProfileError(f"Rust source does not exist: {source}")
        try:
            snapshots[source] = source.read_bytes()
        except OSError as exc:
            raise ProfileError(f"cannot snapshot Rust source {source}: {exc}") from exc
    return snapshots


def verify_source_snapshots(snapshots: dict[Path, bytes]) -> None:
    for source, expected in snapshots.items():
        try:
            actual = source.read_bytes()
        except OSError as exc:
            raise ProfileError(f"Rust source changed during generation: {source}: {exc}") from exc
        if actual != expected:
            raise ProfileError(
                f"Rust source changed during generation: {source}; retry from a stable tree"
            )


def stage_sources(
    manifest: dict, snapshots: dict[Path, bytes], destination: Path
) -> dict[Path, Path]:
    staged: dict[Path, Path] = {}
    for entry in manifest["module"]:
        source: Path = entry["_source"]
        target = destination / entry["source"]
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            target.write_bytes(snapshots[source])
            target.chmod(0o400)
        except OSError as exc:
            raise ProfileError(f"cannot stage Rust source {source}: {exc}") from exc
        staged[source] = target
    return staged


def validate_rust_crate(root: Path) -> None:
    run_checked(["cargo", "check", "-p", "srpc"], cwd=root)


def verify_stamped_output(path: Path, data: bytes, fields: list[tuple[str, str]]) -> None:
    """Verify exact ownership metadata and the self-excluding output checksum."""
    lines = data.splitlines(keepends=True)
    expected_lines = render_stamp(fields).splitlines(keepends=True)
    if len(lines) <= len(expected_lines):
        raise ProfileError(f"generated output has a truncated stamp: {path}")
    for index, (key_and_value, expected) in enumerate(zip(fields, expected_lines)):
        if lines[index] != expected:
            key, value = key_and_value
            actual = lines[index].decode("utf-8", errors="replace").rstrip("\r\n")
            raise ProfileError(
                f"{path} has stale {key} stamp: expected {value!r}, got {actual!r}"
            )

    checksum_index = len(expected_lines)
    match = re.fullmatch(
        rb"// srpc-cpp-output-sha256: ([0-9a-f]{64})\n", lines[checksum_index]
    )
    if match is None:
        raise ProfileError(f"generated output has no valid integrity stamp: {path}")
    recorded = match.group(1).decode("ascii")
    unhashed = b"".join(lines[:checksum_index] + lines[checksum_index + 1 :])
    actual = sha256_bytes(unhashed)
    if actual != recorded:
        raise ProfileError(
            f"generated output integrity drift: {path}: recorded={recorded}, actual={actual}"
        )

    body = b"".join(lines[checksum_index + 1 :])
    try:
        body_text = body.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProfileError(f"generated output is not UTF-8: {path}: {exc}") from exc
    slots = hand_slots(body_text)
    if slots:
        raise ProfileError(f"generated output contains a hand-override slot: {path}")


def check_stamps(
    manifest: dict,
    manifest_path: Path,
    manifest_raw: bytes,
    root: Path,
) -> None:
    """Offline drift check: never invokes the transpiler, rustc, Cargo, or Git."""
    manifest_sha = sha256_bytes(manifest_raw)
    for entry in manifest["module"]:
        source: Path = entry["_source"]
        output: Path = entry["_output"]
        try:
            source_bytes = source.read_bytes()
        except OSError as exc:
            raise ProfileError(f"cannot read manifest-owned source {source}: {exc}") from exc
        try:
            output_bytes = output.read_bytes()
        except OSError as exc:
            raise ProfileError(f"cannot read manifest-owned output {output}: {exc}") from exc
        fields = stamp_fields(
            manifest_path=manifest_path,
            manifest_sha=manifest_sha,
            profile=manifest["profile"],
            source_label=source.relative_to(root).as_posix(),
            source_sha=sha256_bytes(source_bytes),
            module_name=entry["module_name"],
            kind=entry["kind"],
            transpiler_git=manifest["transpiler_git"],
            transpiler_sha256=manifest["transpiler_sha256"],
            root=root,
        )
        verify_stamped_output(output, output_bytes, fields)
        print(f"ok {output.relative_to(root)}")


def atomic_write(path: Path, data: bytes) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        path.chmod(0o644)
        return False
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    tmp = Path(temporary)
    try:
        with os.fdopen(fd, "wb") as handle:
            os.fchmod(handle.fileno(), 0o644)
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)
    return True


def check_unexpected_outputs(manifest: dict) -> None:
    output_dir: Path = manifest["_output_dir"]
    expected = {entry["_output"] for entry in manifest["module"]}
    if not output_dir.exists():
        return
    actual = {
        path.resolve()
        for path in output_dir.rglob("*")
        if path.is_file() and path.suffix in {".cppm", ".cpp"}
    }
    unexpected = sorted(actual - expected)
    if unexpected:
        labels = "\n".join(f"  {path}" for path in unexpected)
        raise ProfileError(f"generated outputs not owned by the manifest:\n{labels}")


def display_diff(path: Path, expected: bytes) -> str:
    actual_text = ""
    if path.exists():
        actual_text = path.read_text(encoding="utf-8", errors="replace")
    expected_text = expected.decode("utf-8")
    return "".join(
        difflib.unified_diff(
            actual_text.splitlines(keepends=True),
            expected_text.splitlines(keepends=True),
            fromfile=str(path),
            tofile=f"{path} (regenerated)",
            n=3,
        )
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="atomically update outputs")
    mode.add_argument("--check", action="store_true", help="fail if outputs differ")
    mode.add_argument(
        "--check-stamps",
        action="store_true",
        help="offline source/profile/output integrity check (no Cargo or transpiler)",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help=f"profile manifest (default: {DEFAULT_MANIFEST})",
    )
    parser.add_argument(
        "--transpiler",
        type=Path,
        default=DEFAULT_TRANSPILER,
        help=f"rusty-cpp executable (default: {DEFAULT_TRANSPILER})",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = repo_root()
    manifest_path = args.manifest
    if not manifest_path.is_absolute():
        manifest_path = root / manifest_path
    transpiler = args.transpiler
    if not transpiler.is_absolute():
        transpiler = root / transpiler
    try:
        manifest, manifest_raw = load_manifest(manifest_path.resolve(), root)
        check_unexpected_outputs(manifest)
        if args.check_stamps:
            check_stamps(manifest, manifest_path, manifest_raw, root)
            return 0

        transpiler_git, transpiler_sha256 = verify_transpiler(
            root,
            transpiler,
            manifest["transpiler_git"],
            manifest["transpiler_sha256"],
        )
        manifest_sha = sha256_bytes(manifest_raw)
        generated: list[tuple[Path, bytes]] = []
        snapshots = snapshot_sources(manifest)
        validate_rust_crate(root)
        verify_source_snapshots(snapshots)
        with tempfile.TemporaryDirectory(prefix="srpc-cpp-profile-") as temporary:
            scratch = Path(temporary)
            staged_transpiler = stage_transpiler(
                transpiler, scratch / "emitter", transpiler_sha256
            )
            staged_sources = stage_sources(
                manifest, snapshots, scratch / "sources"
            )
            for entry in manifest["module"]:
                source: Path = entry["_source"]
                data = generate_one(
                    entry,
                    manifest,
                    manifest_path,
                    manifest_sha,
                    staged_transpiler,
                    transpiler_git,
                    transpiler_sha256,
                    root,
                    scratch,
                    snapshots[source],
                    staged_sources[source],
                )
                generated.append((entry["_output"], data))
        verify_source_snapshots(snapshots)

        if args.write:
            for path, data in generated:
                changed = atomic_write(path, data)
                action = "wrote" if changed else "unchanged"
                print(f"{action} {path.relative_to(root)}")
            return 0

        stale = False
        for path, data in generated:
            if not path.exists() or path.read_bytes() != data:
                stale = True
                print(display_diff(path, data), end="", file=sys.stderr)
        if stale:
            raise ProfileError(
                "checked-in srpc C++ outputs are stale; run "
                "scripts/generate_srpc_cpp.py --write"
            )
        for path, _ in generated:
            print(f"ok {path.relative_to(root)}")
        return 0
    except (OSError, ProfileError, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
