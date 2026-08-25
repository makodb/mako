#!/usr/bin/env python3
"""Validate the canonical Rust view of production srpc modules.

Schema 2 modules are canonical ``.rs`` sources compiled directly by rustc and
translated by rusty-cpp crate mode. The driver owns their manifest, path and
source census, generated ``lib.rs``, toolchain attestation, and check behavior.
Schema 1 remains supported for focused legacy-driver tests; the production
ownership manifest is schema 2 and has no extraction fallback.
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


DEFAULT_MANIFEST = "rust-modules.toml"
DEFAULT_TRANSPILER = (
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
PACKAGE_NAME = "srpc"
GENERATED_ROOT = PurePosixPath("src")
GENERATED_LIB = GENERATED_ROOT / "lib.rs"
RUSTY_CPP_SUBMODULE = "third-party/rusty-cpp"
REQUIRED_RUSTY_CPP_COMMIT = "fa7dd9d9612c0bcec695c3e391ace96b56498e74"
APPROVED_PRODUCTION_ROOTS = (
    PurePosixPath("base"),
    PurePosixPath("misc"),
    PurePosixPath("rpc"),
    PurePosixPath("reactor"),
)
BLOCK_ID_RE = re.compile(r"[A-Za-z0-9_.-]+\Z")
# Optional crate-level flat-import namespace: when present, crate mode runs
# with --flat-import-namespace <value> and every private
# `use crate::<child>::<Name leaves>;` in a canonical source carries an
# implicit cpp_import_namespace(<value>) contract (no per-item marker).
FLAT_IMPORT_NAMESPACE_KEY = "flat_import_namespace"
FLAT_IMPORT_NAMESPACE_RE = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*\Z"
)
CPP_MODULE_RE = re.compile(r"srpc\.([A-Za-z_][A-Za-z0-9_]*)\Z")
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
    canonical_source_label: str | None = None


@dataclass(frozen=True)
class GeneratedFile:
    output_label: str
    output: Path
    content: bytes
    writable: bool = True


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


# Suffix sets are per-call because the two kinds of manifest input are now
# genuinely different languages. A schema-2 `source` is CANONICAL RUST and ends
# in .rs; a schema-1 `input` is a hand-written C++ inline-DSL carrier and still
# ends in .cpp/.cc/.cxx. Defaulting to the C++ set keeps the schema-1 callers
# unchanged, so only the canonical-source caller opts into .rs.
CPP_CARRIER_SUFFIXES = frozenset({".cpp", ".cc", ".cxx"})
CANONICAL_RUST_SUFFIXES = frozenset({".rs"})


def validate_production_source_path(
    root: Path,
    source_label: str,
    source: Path,
    description: str,
    allowed_suffixes: frozenset = CPP_CARRIER_SUFFIXES,
    language: str = "C++",
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
    if source.suffix not in allowed_suffixes or not source.is_file():
        raise ExtractionError(
            f"{description} is not an existing {language} file: {source_label}"
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
    reject_symlink_components(root, manifest_path, "Rust ownership manifest")
    resolved_root = root.resolve()
    try:
        physical_manifest = manifest_path.resolve(strict=True)
        physical_manifest.relative_to(resolved_root)
    except (OSError, ValueError) as exc:
        raise ExtractionError(
            "Rust ownership manifest is not a physical repository file: "
            f"{manifest_path}"
        ) from exc
    if not manifest_path.is_file():
        raise ExtractionError(
            f"Rust ownership manifest is not a file: {manifest_path}"
        )
    try:
        raw = manifest_path.read_bytes()
        data = tomllib.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise ExtractionError(
            f"cannot read Rust ownership manifest {manifest_path}: {exc}"
        ) from exc

    required_keys = {"schema_version", "module"}
    if not required_keys <= set(data) or set(data) - required_keys - {
        FLAT_IMPORT_NAMESPACE_KEY
    }:
        raise ExtractionError(
            "manifest keys must be exactly schema_version and module, "
            f"plus optional {FLAT_IMPORT_NAMESPACE_KEY}"
        )
    flat_import_namespace = data.get(FLAT_IMPORT_NAMESPACE_KEY)
    if flat_import_namespace is not None and (
        not isinstance(flat_import_namespace, str)
        or FLAT_IMPORT_NAMESPACE_RE.fullmatch(flat_import_namespace) is None
    ):
        raise ExtractionError(
            f"manifest {FLAT_IMPORT_NAMESPACE_KEY} must be a C++ namespace path"
        )
    schema_version = data["schema_version"]
    if schema_version not in {1, 2}:
        raise ExtractionError("manifest schema_version must be 1 or 2")
    modules = data["module"]
    if not isinstance(modules, list) or not modules:
        raise ExtractionError("manifest module must be a non-empty array of tables")

    validate_generated_path(
        root,
        root.resolve() / GENERATED_LIB.as_posix(),
        "manifest-generated Rust lib.rs",
    )

    seen_cpp_modules: set[str] = set()
    seen_sources: set[str] = set()
    seen_outputs: set[str] = set()
    block_owners: dict[str, tuple[int, int]] = {}
    loaded: list[ModuleEntry] = []
    for module_index, module in enumerate(modules):
        expected_keys = (
            {"cpp_module", "output", "input"}
            if schema_version == 1
            else {"cpp_module", "source"}
        )
        if not isinstance(module, dict) or set(module) != expected_keys:
            raise ExtractionError(
                f"module {module_index} keys must be exactly "
                + ", ".join(sorted(expected_keys))
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

        source_key = "output" if schema_version == 1 else "source"
        output_label, output = normalized_repo_path(
            root, module[source_key], f"module {module_index} {source_key}"
        )
        expected_output = (GENERATED_ROOT / f"{rust_module}.rs").as_posix()
        if schema_version == 1 and output_label != expected_output:
            raise ExtractionError(
                f"module {module_index} output does not match cpp_module "
                f"{cpp_module!r}: expected {expected_output}, got {output_label}"
            )
        if output_label == GENERATED_LIB.as_posix():
            raise ExtractionError(
                f"module {module_index} cannot own {output_label}; "
                "lib.rs is reserved for the manifest-generated module index"
            )
        if output_label in seen_outputs:
            raise ExtractionError(f"duplicate output ownership: {output_label}")
        if schema_version == 2:
            validate_production_source_path(
                root,
                output_label,
                output,
                f"module {module_index} canonical source {output_label}",
                allowed_suffixes=CANONICAL_RUST_SUFFIXES,
                language="Rust",
            )
            if output.stem != rust_module:
                raise ExtractionError(
                    f"module {module_index} canonical source basename does not "
                    f"match {cpp_module!r}: {output_label}"
                )
            # No discovery shim to validate. The generated lib.rs names this
            # canonical file directly in a `#[path]` attribute, and --check
            # byte-compares that file, so the declaration is already pinned to
            # the manifest. `src/` holds nothing but lib.rs; the census below
            # rejects anything else that appears there, symlink included.
            loaded.append(
                ModuleEntry(
                    cpp_module=cpp_module,
                    rust_module=rust_module,
                    output_label=output_label,
                    output=output,
                    inputs=(),
                    canonical_source_label=output_label,
                )
            )
            seen_cpp_modules.add(cpp_module)
            seen_outputs.add(output_label)
            continue

        validate_generated_path(
            root,
            output,
            f"module {module_index} {source_key} {output_label}",
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


def load_flat_import_namespace(root: Path, manifest_path: Path) -> str | None:
    """The validated optional crate-level flat-import namespace, or None."""
    load_manifest(root, manifest_path)
    data = tomllib.loads(manifest_path.read_bytes().decode("utf-8"))
    value = data.get(FLAT_IMPORT_NAMESPACE_KEY)
    return value if isinstance(value, str) else None


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


def validate_canonical_source(raw: bytes, source_label: str) -> bytes:
    """Validate canonical Rust without ever normalizing or regenerating it."""

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ExtractionError(
            f"canonical Rust source {source_label} is not UTF-8"
        ) from exc
    if "\x00" in text:
        raise ExtractionError(f"canonical Rust source {source_label} contains NUL")
    if "\r" in text:
        raise ExtractionError(
            f"canonical Rust source {source_label} must use LF line endings"
        )
    if not text.strip():
        raise ExtractionError(f"canonical Rust source {source_label} is empty")
    if not text.endswith("\n"):
        raise ExtractionError(
            f"canonical Rust source {source_label} must end with a newline"
        )
    return raw


def render_module(module: ModuleEntry, payloads: list[bytes]) -> bytes:
    if len(payloads) != len(module.inputs):
        raise AssertionError("source-group payload count does not match manifest")
    payload = b"\n".join(payloads)
    header = [
        "// @generated by scripts/extract_srpc_rust.py; DO NOT EDIT.",
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


def module_path_attribute_value(output_label: str) -> str:
    """The `#[path]` value naming output_label from the generated lib.rs.

    rustc resolves a module `#[path]` against the directory holding the
    declaring file, so every value is relative to GENERATED_ROOT. Derive it
    from the manifest label rather than hardcoding a table: the manifest is
    the single owner of where a module's bytes live.
    """

    ascent = ("..",) * len(GENERATED_ROOT.parts)
    value = PurePosixPath(*ascent, output_label).as_posix()
    # The value is emitted verbatim into a Rust string literal. Reject any
    # character that would need escaping instead of escaping it, so a manifest
    # can never smuggle syntax into the generated crate index.
    if any(character in value for character in '"\\\n\r'):
        raise ExtractionError(
            f"canonical source path is not expressible as a Rust `#[path]` "
            f"string literal: {output_label!r}"
        )
    return value


def render_lib(
    manifest_label: str,
    manifest_path: Path,
    modules: list[ModuleEntry],
) -> bytes:
    lines = [
        "// @generated by scripts/extract_srpc_rust.py; DO NOT EDIT.",
        f"// provenance-manifest: {manifest_label}",
        f"// provenance-manifest-sha256: {sha256(manifest_path.read_bytes())}",
        "//",
        "#![deny(unsafe_code)]",
        "",
    ]
    for module in sorted(modules, key=lambda entry: entry.rust_module):
        style_allow = (
            "non_snake_case, " if module.rust_module == "epoll_wrapper" else ""
        )
        lines.append(
            f"#[allow(dead_code, {style_allow}non_upper_case_globals, "
            "clippy::new_without_default)]"
        )
        # A schema-2 module's canonical bytes live outside src/, so the
        # declaration has to name that file. A schema-1 output is generated
        # into src/ and stays conventional.
        if module.canonical_source_label is not None:
            value = module_path_attribute_value(module.canonical_source_label)
            lines.append(f'#[path = "{value}"]')
        lines.append(f"pub mod {module.rust_module};")
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
    with tempfile.TemporaryDirectory(prefix="srpc-inline-rust-") as temporary:
        scratch = Path(temporary)
        for module_index, module in enumerate(modules):
            if module.canonical_source_label is not None:
                generated.append(
                    GeneratedFile(
                        output_label=module.output_label,
                        output=module.output,
                        content=validate_canonical_source(
                            module.output.read_bytes(), module.output_label
                        ),
                        writable=False,
                    )
                )
                continue
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
        # Every entry, file or directory, must be a real thing in src/. A
        # module now reaches its canonical bytes through the `#[path]` in the
        # generated lib.rs, so a symlink here is never a legitimate discovery
        # shim -- it is a second, unowned route to a source. Skipping one
        # silently would let the whole retired shim layer grow back unnoticed.
        for name in (*child_directories, *files):
            child = current / name
            if child.is_symlink():
                raise ExtractionError(
                    "manifest-owned Rust source census rejects symlinks: "
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
    expected = {item.output_label for item in generated if item.writable}
    if len({item.output_label for item in generated}) != len(generated):
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
        details.append("missing manifest-owned Rust source(s):\n" + "\n".join(
            f"  {path}" for path in sorted(missing)
        ))
    raise ExtractionError("\n".join(details))


def apply_mode(root: Path, generated: list[GeneratedFile], mode: str) -> None:
    for item in generated:
        if item.writable:
            validate_generated_path(
                root,
                item.output,
                f"generated output {item.output_label}",
            )
    validate_census(root, generated, allow_missing=mode == "write")
    drift: list[str] = []
    for item in generated:
        if item.writable:
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
        if not item.writable:
            if actual != item.content:
                raise ExtractionError(
                    "canonical Rust source changed during ownership validation; "
                    f"refusing to overwrite it: {item.output_label}"
                )
            print(f"validated {item.output_label}")
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
        raise ExtractionError(f"manifest-owned Rust output is stale:\n{joined}")
    if mode == "check":
        print(
            f"checked {len(generated) - 1} manifest-owned Rust module(s) and lib.rs"
        )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--write",
        action="store_true",
        help="regenerate derived manifest outputs (never canonical module sources)",
    )
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
