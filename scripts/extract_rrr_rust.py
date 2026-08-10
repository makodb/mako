#!/usr/bin/env python3
"""Deterministically materialize the rustc view of production rrr DSL modules.

The actual Rust extraction is owned exclusively by rusty-cpp's pinned
``inline-rust --emit-rust`` command. This driver owns the repository manifest,
module/source validation, provenance envelopes, generated ``lib.rs``, source
census, check behavior, and per-file atomic writes. It has no extraction
fallback.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import tomllib


DEFAULT_MANIFEST = "src/rrr/rust-extraction.toml"
DEFAULT_TRANSPILER = (
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
PACKAGE_NAME = "rrr"
GENERATED_ROOT = PurePosixPath("src/rrr/src")
GENERATED_LIB = GENERATED_ROOT / "lib.rs"
RUSTY_CPP_SUBMODULE = "third-party/rusty-cpp"
REQUIRED_RUSTY_CPP_COMMIT = "ba70b6ab6d8b38bfc5107ce963c6766d460b0e42"
APPROVED_PRODUCTION_ROOTS = (
    PurePosixPath("src/rrr/base"),
    PurePosixPath("src/rrr/misc"),
    PurePosixPath("src/rrr/rpc"),
    PurePosixPath("src/rrr/reactor"),
)
BLOCK_ID_RE = re.compile(r"[A-Za-z0-9_.-]+\Z")
CPP_MODULE_RE = re.compile(r"rrr\.([A-Za-z_][A-Za-z0-9_]*)\Z")
NAMED_MODULE_DECL_RE = re.compile(
    r"(?:(export)\s+)?module\s+([A-Za-z_][A-Za-z0-9_.]*)\s*;\Z"
)
RUST_KEYWORDS = {
    "as",
    "async",
    "await",
    "break",
    "const",
    "continue",
    "crate",
    "dyn",
    "else",
    "enum",
    "extern",
    "false",
    "fn",
    "for",
    "if",
    "impl",
    "in",
    "let",
    "loop",
    "match",
    "mod",
    "move",
    "mut",
    "pub",
    "ref",
    "return",
    "self",
    "Self",
    "static",
    "struct",
    "super",
    "trait",
    "true",
    "type",
    "union",
    "unsafe",
    "use",
    "where",
    "while",
}


class ExtractionError(RuntimeError):
    """A manifest, emitter, provenance, census, or drift failure."""


@dataclass(frozen=True)
class SourceGroup:
    source_label: str
    source: Path
    block_ids: tuple[str, ...]


@dataclass(frozen=True)
class ModuleEntry:
    cpp_module: str
    rust_module: str
    output_label: str
    output: Path
    inputs: tuple[SourceGroup, ...]


@dataclass(frozen=True)
class GeneratedFile:
    output_label: str
    output: Path
    content: bytes


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def normalized_repo_path(
    root: Path, label: object, description: str
) -> tuple[str, Path]:
    if not isinstance(label, str) or not label:
        raise ExtractionError(f"{description} must be a non-empty string")
    if "\\" in label or "\n" in label or "\r" in label:
        raise ExtractionError(
            f"{description} must be a normalized POSIX path: {label!r}"
        )
    relative = PurePosixPath(label)
    if relative.is_absolute() or any(
        part in {"", ".", ".."} for part in relative.parts
    ):
        raise ExtractionError(
            f"{description} must stay inside the repository: {label!r}"
        )
    normalized = relative.as_posix()
    if normalized != label:
        raise ExtractionError(f"{description} is not normalized: {label!r}")
    resolved_root = root.resolve()
    return normalized, resolved_root.joinpath(*relative.parts)


def reject_symlink_components(
    root: Path, path: Path, description: str
) -> None:
    """Reject every symlink at or below root on the lexical path to path."""

    resolved_root = root.resolve()
    try:
        relative = path.relative_to(resolved_root)
    except ValueError as exc:
        raise ExtractionError(
            f"{description} escapes the repository: {path}"
        ) from exc
    current = resolved_root
    for part in relative.parts:
        current /= part
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise ExtractionError(
                f"cannot inspect {description} path component {current}: {exc}"
            ) from exc
        if stat.S_ISLNK(metadata.st_mode):
            raise ExtractionError(
                f"{description} must not use symlink components: {current}"
            )


def validate_production_source_path(
    root: Path,
    source_label: str,
    source: Path,
    description: str,
) -> None:
    relative = PurePosixPath(source_label)
    approved = next(
        (
            candidate
            for candidate in APPROVED_PRODUCTION_ROOTS
            if relative != candidate
            and relative.parts[: len(candidate.parts)] == candidate.parts
        ),
        None,
    )
    if approved is None:
        roots = ", ".join(path.as_posix() for path in APPROVED_PRODUCTION_ROOTS)
        raise ExtractionError(
            f"{description} must be lexically inside an approved production "
            f"root ({roots}): {source_label}"
        )
    reject_symlink_components(root, source, description)
    if source.suffix not in {".cpp", ".cc", ".cxx"} or not source.is_file():
        raise ExtractionError(
            f"{description} is not an existing C++ file: {source_label}"
        )
    try:
        physical_source = source.resolve(strict=True)
        physical_root = (root.resolve() / approved.as_posix()).resolve(strict=True)
        physical_source.relative_to(physical_root)
    except (OSError, ValueError) as exc:
        raise ExtractionError(
            f"{description} is not physically inside approved production root "
            f"{approved.as_posix()}: {source_label}"
        ) from exc


def validate_generated_path(root: Path, path: Path, description: str) -> None:
    resolved_root = root.resolve()
    generated_root = resolved_root / GENERATED_ROOT.as_posix()
    try:
        relative = path.relative_to(generated_root)
    except ValueError as exc:
        raise ExtractionError(
            f"{description} is outside {GENERATED_ROOT.as_posix()}: {path}"
        ) from exc
    if any(part == ".." for part in relative.parts):
        raise ExtractionError(
            f"{description} is outside {GENERATED_ROOT.as_posix()}: {path}"
        )
    reject_symlink_components(root, path, description)


def named_module_declarations(source: Path, source_label: str) -> list[tuple[bool, str]]:
    try:
        text = source.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise ExtractionError(f"cannot read module source {source_label}: {exc}") from exc
    declarations: list[tuple[bool, str]] = []
    for line in text.splitlines():
        match = NAMED_MODULE_DECL_RE.fullmatch(line.strip())
        if match is not None:
            declarations.append((match.group(1) is not None, match.group(2)))
    return declarations


def validate_source_module(
    source: Path,
    source_label: str,
    cpp_module: str,
    interface: bool,
) -> None:
    actual = named_module_declarations(source, source_label)
    expected = [(interface, cpp_module)]
    if actual != expected:
        declaration = (
            f"export module {cpp_module};" if interface else f"module {cpp_module};"
        )
        role = "interface" if interface else "implementation"
        raise ExtractionError(
            f"{role} source {source_label} must contain exactly {declaration!r}; "
            f"found {actual!r}"
        )


def parse_block_ids(
    raw: object,
    module_index: int,
    input_index: int,
    block_owners: dict[str, tuple[int, int]],
) -> tuple[str, ...]:
    if not isinstance(raw, list) or not raw:
        raise ExtractionError(
            f"module {module_index} input {input_index} block_ids must be a "
            "non-empty array"
        )
    block_ids: list[str] = []
    local_ids: set[str] = set()
    for position, block_id in enumerate(raw):
        if not isinstance(block_id, str) or not BLOCK_ID_RE.fullmatch(block_id):
            raise ExtractionError(
                f"module {module_index} input {input_index} "
                f"block_ids[{position}] is invalid: {block_id!r}"
            )
        if block_id in local_ids:
            raise ExtractionError(
                f"module {module_index} input {input_index} block_ids contains "
                f"duplicate {block_id!r}"
            )
        owner = block_owners.get(block_id)
        if owner is not None:
            raise ExtractionError(
                f"block ID {block_id!r} is already owned by module {owner[0]} "
                f"input {owner[1]}"
            )
        local_ids.add(block_id)
        block_ids.append(block_id)
    return tuple(block_ids)


def load_manifest(root: Path, manifest_path: Path) -> list[ModuleEntry]:
    try:
        raw = manifest_path.read_bytes()
        data = tomllib.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise ExtractionError(
            f"cannot read extraction manifest {manifest_path}: {exc}"
        ) from exc

    if set(data) != {"schema_version", "module"}:
        raise ExtractionError("manifest keys must be exactly schema_version and module")
    if data["schema_version"] != 1:
        raise ExtractionError("manifest schema_version must be 1")
    modules = data["module"]
    if not isinstance(modules, list) or not modules:
        raise ExtractionError("manifest module must be a non-empty array of tables")

    validate_generated_path(
        root,
        root.resolve() / GENERATED_LIB.as_posix(),
        "generated Rust lib.rs",
    )

    seen_cpp_modules: set[str] = set()
    seen_sources: set[str] = set()
    seen_outputs: set[str] = set()
    block_owners: dict[str, tuple[int, int]] = {}
    loaded: list[ModuleEntry] = []
    for module_index, module in enumerate(modules):
        if not isinstance(module, dict) or set(module) != {
            "cpp_module",
            "output",
            "input",
        }:
            raise ExtractionError(
                f"module {module_index} keys must be exactly cpp_module, output, "
                "and input"
            )
        cpp_module = module["cpp_module"]
        if not isinstance(cpp_module, str):
            raise ExtractionError(
                f"module {module_index} cpp_module must be a string"
            )
        module_match = CPP_MODULE_RE.fullmatch(cpp_module)
        if module_match is None:
            raise ExtractionError(
                f"module {module_index} cpp_module must be a direct "
                f"{PACKAGE_NAME}.<rust_module> name: {cpp_module!r}"
            )
        rust_module = module_match.group(1)
        if rust_module in RUST_KEYWORDS:
            raise ExtractionError(
                f"module {module_index} maps to reserved Rust name {rust_module!r}"
            )
        if cpp_module in seen_cpp_modules:
            raise ExtractionError(f"duplicate cpp_module ownership: {cpp_module}")

        output_label, output = normalized_repo_path(
            root, module["output"], f"module {module_index} output"
        )
        expected_output = (GENERATED_ROOT / f"{rust_module}.rs").as_posix()
        if output_label != expected_output:
            raise ExtractionError(
                f"module {module_index} output does not match cpp_module "
                f"{cpp_module!r}: expected {expected_output}, got {output_label}"
            )
        if output_label in seen_outputs:
            raise ExtractionError(f"duplicate output ownership: {output_label}")
        validate_generated_path(
            root,
            output,
            f"module {module_index} output {output_label}",
        )

        raw_inputs = module["input"]
        if not isinstance(raw_inputs, list) or not raw_inputs:
            raise ExtractionError(
                f"module {module_index} input must be a non-empty ordered "
                "array of tables"
            )
        inputs: list[SourceGroup] = []
        for input_index, input_group in enumerate(raw_inputs):
            if not isinstance(input_group, dict) or set(input_group) != {
                "source",
                "block_ids",
            }:
                raise ExtractionError(
                    f"module {module_index} input {input_index} keys must be "
                    "exactly source and block_ids"
                )
            source_label, source = normalized_repo_path(
                root,
                input_group["source"],
                f"module {module_index} input {input_index} source",
            )
            validate_production_source_path(
                root,
                source_label,
                source,
                f"module {module_index} input {input_index} source",
            )
            if source_label in seen_sources:
                raise ExtractionError(
                    f"duplicate source ownership: {source_label}"
                )
            block_ids = parse_block_ids(
                input_group["block_ids"],
                module_index,
                input_index,
                block_owners,
            )
            validate_source_module(
                source,
                source_label,
                cpp_module,
                interface=input_index == 0,
            )
            inputs.append(SourceGroup(source_label, source, block_ids))
            seen_sources.add(source_label)
            for block_id in block_ids:
                block_owners[block_id] = (module_index, input_index)

        loaded.append(
            ModuleEntry(
                cpp_module=cpp_module,
                rust_module=rust_module,
                output_label=output_label,
                output=output,
                inputs=tuple(inputs),
            )
        )
        seen_cpp_modules.add(cpp_module)
        seen_outputs.add(output_label)
    return loaded


def resolve_transpiler(root: Path, value: str) -> Path:
    candidate = Path(value)
    if candidate.is_absolute() or "/" in value:
        executable = candidate if candidate.is_absolute() else root / candidate
    else:
        found = shutil.which(value)
        executable = Path(found) if found is not None else root / candidate
    executable = executable.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ExtractionError(
            "inline-Rust emitter is unavailable: "
            f"{executable}; build rusty-cpp at the required emitter pin "
            "or pass --transpiler"
        )
    return executable


def git_output(cwd: Path, arguments: list[str], description: str) -> str:
    try:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise ExtractionError(f"cannot inspect {description}: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        raise ExtractionError(f"cannot inspect {description}: {diagnostic}")
    return completed.stdout.strip()


def verify_transpiler_build_info(root: Path, transpiler: Path) -> None:
    try:
        completed = subprocess.run(
            [str(transpiler), "--build-info"],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise ExtractionError(
            f"cannot read rusty-cpp transpiler build info: {exc}"
        ) from exc
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        raise ExtractionError(
            "rusty-cpp transpiler --build-info failed with exit "
            f"{completed.returncode}: {diagnostic}"
        )
    lines = completed.stdout.splitlines()
    if len(lines) != 1:
        raise ExtractionError(
            "rusty-cpp transpiler --build-info must emit exactly one JSON line"
        )
    try:
        build_info = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise ExtractionError(
            f"rusty-cpp transpiler --build-info emitted invalid JSON: {exc}"
        ) from exc
    if not isinstance(build_info, dict) or set(build_info) != {
        "git_hash",
        "git_dirty",
    }:
        raise ExtractionError(
            "rusty-cpp transpiler --build-info JSON keys must be exactly "
            "git_hash and git_dirty"
        )
    git_hash = build_info["git_hash"]
    git_dirty = build_info["git_dirty"]
    if git_hash != REQUIRED_RUSTY_CPP_COMMIT:
        raise ExtractionError(
            "rusty-cpp transpiler build commit mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {git_hash!r}"
        )
    if git_dirty is not False:
        raise ExtractionError(
            "rusty-cpp transpiler build must report git_dirty=false"
        )


def verify_pinned_toolchain(root: Path, transpiler: Path) -> None:
    index_entry = git_output(
        root,
        ["ls-files", "--stage", "--", RUSTY_CPP_SUBMODULE],
        "rusty-cpp gitlink",
    ).split()
    if (
        len(index_entry) < 3
        or index_entry[0] != "160000"
        or index_entry[1] != REQUIRED_RUSTY_CPP_COMMIT
    ):
        actual = index_entry[1] if len(index_entry) >= 2 else "missing"
        raise ExtractionError(
            "rusty-cpp gitlink pin mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {actual}"
        )

    submodule = root / RUSTY_CPP_SUBMODULE
    head = git_output(submodule, ["rev-parse", "HEAD"], "rusty-cpp HEAD")
    if head != REQUIRED_RUSTY_CPP_COMMIT:
        raise ExtractionError(
            "rusty-cpp submodule HEAD mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {head}"
        )
    dirty = git_output(
        submodule,
        ["status", "--porcelain", "--untracked-files=no"],
        "rusty-cpp worktree",
    )
    if dirty:
        raise ExtractionError("rusty-cpp submodule has tracked local changes")

    verify_transpiler_build_info(root, transpiler)


def normalize_payload(raw: bytes, output_label: str) -> bytes:
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ExtractionError(
            f"emitter output for {output_label} is not UTF-8"
        ) from exc
    if "\x00" in text:
        raise ExtractionError(f"emitter output for {output_label} contains NUL")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = text.strip("\n") + "\n"
    if not text.strip():
        raise ExtractionError(f"emitter output for {output_label} is empty")
    return text.encode("utf-8")


def render_module(module: ModuleEntry, payloads: list[bytes]) -> bytes:
    if len(payloads) != len(module.inputs):
        raise AssertionError("source-group payload count does not match manifest")
    payload = b"\n".join(payloads)
    header = [
        "// @generated by scripts/extract_rrr_rust.py; DO NOT EDIT.",
        f"// provenance-cpp-module: {module.cpp_module}",
    ]
    for index, (source_group, group_payload) in enumerate(
        zip(module.inputs, payloads, strict=True)
    ):
        header.extend(
            [
                f"// provenance-input[{index}]-source: {source_group.source_label}",
                f"// provenance-input[{index}]-block-ids: "
                f"{', '.join(source_group.block_ids)}",
                f"// provenance-input[{index}]-source-sha256: "
                f"{sha256(source_group.source.read_bytes())}",
                f"// provenance-input[{index}]-rust-sha256: "
                f"{sha256(group_payload)}",
            ]
        )
    header.extend(
        [
            f"// provenance-rust-sha256: {sha256(payload)}",
            "//",
        ]
    )
    return ("\n".join(header) + "\n").encode("utf-8") + payload


def render_lib(
    manifest_label: str,
    manifest_path: Path,
    modules: list[ModuleEntry],
) -> bytes:
    lines = [
        "// @generated by scripts/extract_rrr_rust.py; DO NOT EDIT.",
        f"// provenance-manifest: {manifest_label}",
        f"// provenance-manifest-sha256: {sha256(manifest_path.read_bytes())}",
        "//",
        "#![forbid(unsafe_code)]",
        "",
    ]
    for module in sorted(modules, key=lambda entry: entry.rust_module):
        lines.extend(
            [
                "#[allow(dead_code, non_upper_case_globals)]",
                f"pub mod {module.rust_module};",
            ]
        )
    return ("\n".join(lines) + "\n").encode("utf-8")


def generate_all(
    root: Path,
    modules: list[ModuleEntry],
    transpiler: Path,
    manifest_label: str,
    manifest_path: Path,
) -> list[GeneratedFile]:
    generated = [
        GeneratedFile(
            output_label=GENERATED_LIB.as_posix(),
            output=root / GENERATED_LIB.as_posix(),
            content=render_lib(manifest_label, manifest_path, modules),
        )
    ]
    with tempfile.TemporaryDirectory(prefix="rrr-inline-rust-") as temporary:
        scratch = Path(temporary)
        for module_index, module in enumerate(modules):
            payloads: list[bytes] = []
            for input_index, source_group in enumerate(module.inputs):
                validate_production_source_path(
                    root,
                    source_group.source_label,
                    source_group.source,
                    f"module {module_index} input {input_index} source",
                )
                raw_output = scratch / f"{module_index:04d}-{input_index:04d}.rs"
                command = [
                    str(transpiler),
                    "inline-rust",
                    "--emit-rust",
                    str(raw_output),
                ]
                for block_id in source_group.block_ids:
                    command.extend(["--block-id", block_id])
                command.extend(["--files", source_group.source_label])
                completed = subprocess.run(
                    command,
                    cwd=root,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                if completed.returncode != 0:
                    diagnostic = (completed.stdout + completed.stderr).strip()
                    raise ExtractionError(
                        f"inline-Rust emitter failed for {source_group.source_label} "
                        f"blocks {', '.join(source_group.block_ids)} "
                        f"(exit {completed.returncode}):\n{diagnostic}"
                    )
                if not raw_output.is_file():
                    raise ExtractionError(
                        f"inline-Rust emitter did not create {raw_output} for "
                        f"{module.output_label} input {input_index}"
                    )
                payloads.append(
                    normalize_payload(
                        raw_output.read_bytes(),
                        f"{module.output_label} input {input_index}",
                    )
                )
            for input_index, source_group in enumerate(module.inputs):
                validate_production_source_path(
                    root,
                    source_group.source_label,
                    source_group.source,
                    f"module {module_index} input {input_index} source",
                )
            generated.append(
                GeneratedFile(
                    output_label=module.output_label,
                    output=module.output,
                    content=render_module(module, payloads),
                )
            )
    return generated


def rust_source_census(root: Path) -> set[str]:
    source_root = root.resolve() / GENERATED_ROOT.as_posix()
    validate_generated_path(root, source_root, "generated Rust source root")
    if not source_root.is_dir():
        return set()
    sources: set[str] = set()
    for directory, child_directories, files in os.walk(
        source_root, followlinks=False
    ):
        current = Path(directory)
        for name in [*child_directories, *files]:
            child = current / name
            if child.is_symlink():
                raise ExtractionError(
                    "generated Rust source census rejects symlinks: "
                    f"{child.relative_to(root.resolve()).as_posix()}"
                )
        for name in files:
            path = current / name
            if path.suffix == ".rs" and path.is_file():
                sources.add(path.relative_to(root.resolve()).as_posix())
    return sources


def validate_census(
    root: Path,
    generated: list[GeneratedFile],
    allow_missing: bool,
) -> None:
    expected = {item.output_label for item in generated}
    if len(expected) != len(generated):
        raise AssertionError("generated output labels are not unique")
    actual = rust_source_census(root)
    orphans = actual - expected
    missing = expected - actual
    if not orphans and (allow_missing or not missing):
        return
    details = ["Rust source census does not match manifest ownership"]
    if orphans:
        details.append("orphan Rust source(s):\n" + "\n".join(
            f"  {path}" for path in sorted(orphans)
        ))
    if missing and not allow_missing:
        details.append("missing generated Rust source(s):\n" + "\n".join(
            f"  {path}" for path in sorted(missing)
        ))
    raise ExtractionError("\n".join(details))


def apply_mode(root: Path, generated: list[GeneratedFile], mode: str) -> None:
    for item in generated:
        validate_generated_path(
            root,
            item.output,
            f"generated output {item.output_label}",
        )
    validate_census(root, generated, allow_missing=mode == "write")
    drift: list[str] = []
    for item in generated:
        validate_generated_path(
            root,
            item.output,
            f"generated output {item.output_label}",
        )
        actual = item.output.read_bytes() if item.output.is_file() else None
        if mode == "check":
            if actual != item.content:
                drift.append(item.output_label)
            continue
        if actual == item.content:
            print(f"unchanged {item.output_label}")
            continue
        item.output.parent.mkdir(parents=True, exist_ok=True)
        validate_generated_path(
            root,
            item.output,
            f"generated output {item.output_label}",
        )
        temporary: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                dir=item.output.parent, delete=False
            ) as handle:
                temporary = Path(handle.name)
                handle.write(item.content)
            os.replace(temporary, item.output)
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
        print(f"wrote {item.output_label}")
    if drift:
        joined = "\n".join(f"  {path}" for path in drift)
        raise ExtractionError(f"generated Rust output is stale:\n{joined}")
    if mode == "check":
        print(f"checked {len(generated) - 1} extracted Rust module(s) and lib.rs")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="regenerate manifest outputs")
    mode.add_argument("--check", action="store_true", help="fail if outputs drift")
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--transpiler",
        default=os.environ.get("RUSTY_CPP_TRANSPILER", DEFAULT_TRANSPILER),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = repository_root()
    try:
        manifest_label, manifest = normalized_repo_path(root, args.manifest, "manifest")
        modules = load_manifest(root, manifest)
        transpiler = resolve_transpiler(root, args.transpiler)
        verify_pinned_toolchain(root, transpiler)
        generated = generate_all(
            root,
            modules,
            transpiler,
            manifest_label,
            manifest,
        )
        apply_mode(root, generated, "write" if args.write else "check")
    except ExtractionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
