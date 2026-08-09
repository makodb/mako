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
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib


SCHEMA_VERSION = 5
GENERATOR_VERSION = 5
CANONICAL_TRANSPILER_SHA256 = (
    "f3e5a069c420ff13d826260e704b12a7dc8b6fc694a3aa1417e7ccf5f2672213"
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
RUST_TYPE_PATH_PATTERN = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*"
)
CPP_TYPE_NAME_PATTERN = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*"
)
CPP_MODULE_NAME_PATTERN = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*"
)
CPP_NAMESPACE_PATTERN = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*"
)
CPP_BINDING_PATH_PATTERN = CPP_NAMESPACE_PATTERN
CPP_SYMBOL_NAME_PATTERN = CPP_NAMESPACE_PATTERN
CPP_SYMBOL_KIND_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_-]*")
CPP_INDEX_PATH_PATTERN = re.compile(r"[A-Za-z0-9_./-]+\.(?:json|toml)")
IMPLICIT_CPP_MODULE_IMPORTS = frozenset({"std", "rusty"})


class ProfileError(RuntimeError):
    """A deterministic consumer-profile invariant was violated."""


def validate_profile_path(label: str, value: str) -> None:
    """Accept only repository-relative paths safe to quote in generated CMake."""
    path = Path(value)
    if (
        path.is_absolute()
        or path.as_posix() != value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise ProfileError(f"{label} must be a normalized repository-relative path")
    if any(character in value for character in ('"', "'", ";", "$", "\\", "\n", "\r")):
        raise ProfileError(f"{label} contains a character unsafe for generated CMake")


def validate_type_mappings(index: int, value: object) -> dict[str, str]:
    """Validate a module-local map of nominal Rust paths to C++ type names.

    The deliberately narrow target grammar excludes templates, references,
    arbitrary cv-qualifiers, and arbitrary C++ tokens.  The sole pointer form
    is the reviewed `const char*` compatibility carrier used by historical
    constexpr string APIs. Broader mappings need an explicit schema review
    rather than becoming a code-injection surface.
    """
    if not isinstance(value, dict):
        raise ProfileError(
            f"module entry {index} type_mappings must be a string-to-string table"
        )
    normalized: dict[str, str] = {}
    for rust_type, cpp_type in value.items():
        if not isinstance(rust_type, str) or not RUST_TYPE_PATH_PATTERN.fullmatch(
            rust_type
        ):
            raise ProfileError(
                f"module entry {index} has invalid Rust type mapping key "
                f"{rust_type!r}"
            )
        if not isinstance(cpp_type, str) or (
            not CPP_TYPE_NAME_PATTERN.fullmatch(cpp_type)
            and cpp_type != "const char*"
        ):
            raise ProfileError(
                f"module entry {index} has invalid C++ type mapping value "
                f"{cpp_type!r} for {rust_type!r}"
            )
        normalized[rust_type] = cpp_type
    return dict(sorted(normalized.items()))


def render_type_map(type_mappings: dict[str, str]) -> bytes:
    """Render the exact deterministic TOML bytes consumed by rusty-cpp."""
    return "".join(
        f"{json.dumps(rust_type)} = {json.dumps(cpp_type)}\n"
        for rust_type, cpp_type in sorted(type_mappings.items())
    ).encode("utf-8")


def render_legacy_dependencies(dependencies: list[str]) -> bytes:
    """Render a dependency set deterministically for ownership stamps."""
    return (json.dumps(sorted(dependencies), separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


def _reject_duplicate_json_pairs(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise ProfileError(f"duplicate JSON key {key!r} in C++ module index")
        result[key] = value
    return result


def _parse_cpp_module_index(path: Path, raw: bytes) -> object:
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProfileError(f"C++ module index is not UTF-8: {path}: {exc}") from exc
    try:
        if path.suffix == ".json":
            return json.loads(text, object_pairs_hook=_reject_duplicate_json_pairs)
        return tomllib.loads(text)
    except (json.JSONDecodeError, tomllib.TOMLDecodeError) as exc:
        raise ProfileError(f"invalid C++ module index {path}: {exc}") from exc


def validate_cpp_module_index(
    index: int,
    path: Path,
    raw: bytes,
    legacy_dependencies: list[str],
) -> tuple[dict, bytes]:
    """Validate and canonically render one module-local foreign symbol index.

    The index key is the Rust binding path below ``cpp::``.  ``cpp_module`` is
    the independently named C++ module import, and ``namespace`` is the C++
    qualification root.  Keeping all three explicit avoids assuming that
    ``rrr::serializable``, ``rrr.serializable``, and ``rrr`` are interchangeable.
    """
    parsed = _parse_cpp_module_index(path, raw)
    if not isinstance(parsed, dict):
        raise ProfileError(f"module entry {index} C++ module index must be a table")
    unknown = sorted(set(parsed) - {"version", "modules"})
    if unknown:
        raise ProfileError(
            f"module entry {index} C++ module index has unknown keys: "
            f"{', '.join(unknown)}"
        )
    version = parsed.get("version")
    if type(version) is not int or version != 1:
        raise ProfileError(
            f"module entry {index} C++ module index version must be integer 1"
        )
    modules = parsed.get("modules")
    if not isinstance(modules, dict) or not modules:
        raise ProfileError(
            f"module entry {index} C++ module index must contain modules"
        )

    normalized_modules: dict[str, dict] = {}
    seen_cpp_modules: set[str] = set()
    declared_legacy = set(legacy_dependencies)
    for binding_path, module in modules.items():
        if not isinstance(binding_path, str) or not CPP_BINDING_PATH_PATTERN.fullmatch(
            binding_path
        ):
            raise ProfileError(
                f"module entry {index} C++ module index has invalid binding path "
                f"{binding_path!r}"
            )
        if not isinstance(module, dict):
            raise ProfileError(
                f"module entry {index} C++ module index entry {binding_path!r} "
                "must be a table"
            )
        module_unknown = sorted(
            set(module) - {"cpp_module", "namespace", "symbols"}
        )
        if module_unknown:
            raise ProfileError(
                f"module entry {index} C++ module index entry {binding_path!r} "
                f"has unknown keys: {', '.join(module_unknown)}"
            )
        cpp_module = module.get("cpp_module")
        namespace = module.get("namespace")
        symbols = module.get("symbols")
        if not isinstance(cpp_module, str) or not CPP_MODULE_NAME_PATTERN.fullmatch(
            cpp_module
        ):
            raise ProfileError(
                f"module entry {index} C++ module index entry {binding_path!r} "
                "has invalid cpp_module"
            )
        if not isinstance(namespace, str) or not CPP_NAMESPACE_PATTERN.fullmatch(
            namespace
        ):
            raise ProfileError(
                f"module entry {index} C++ module index entry {binding_path!r} "
                "has invalid namespace"
            )
        if (
            cpp_module not in declared_legacy
            and cpp_module not in IMPLICIT_CPP_MODULE_IMPORTS
        ):
            raise ProfileError(
                f"module entry {index} C++ module index maps {binding_path!r} to "
                f"undeclared legacy dependency {cpp_module!r}"
            )
        if cpp_module in seen_cpp_modules:
            raise ProfileError(
                f"module entry {index} C++ module index repeats cpp_module "
                f"{cpp_module!r}"
            )
        seen_cpp_modules.add(cpp_module)
        if not isinstance(symbols, dict) or not symbols:
            raise ProfileError(
                f"module entry {index} C++ module index entry {binding_path!r} "
                "must contain symbols"
            )

        normalized_symbols: dict[str, dict] = {}
        for symbol_name, symbol in symbols.items():
            if not isinstance(
                symbol_name, str
            ) or not CPP_SYMBOL_NAME_PATTERN.fullmatch(symbol_name):
                raise ProfileError(
                    f"module entry {index} C++ module index has invalid symbol "
                    f"{symbol_name!r}"
                )
            if not isinstance(symbol, dict):
                raise ProfileError(
                    f"module entry {index} C++ module index symbol "
                    f"{symbol_name!r} must be a table"
                )
            symbol_unknown = sorted(set(symbol) - {"kind", "callable_signatures"})
            if symbol_unknown:
                raise ProfileError(
                    f"module entry {index} C++ module index symbol "
                    f"{symbol_name!r} has unknown keys: {', '.join(symbol_unknown)}"
                )
            kind = symbol.get("kind")
            signatures = symbol.get("callable_signatures", [])
            if not isinstance(kind, str) or not CPP_SYMBOL_KIND_PATTERN.fullmatch(kind):
                raise ProfileError(
                    f"module entry {index} C++ module index symbol "
                    f"{symbol_name!r} has invalid kind"
                )
            if not isinstance(signatures, list) or not all(
                isinstance(signature, str)
                and signature
                and len(signature) <= 1024
                and signature.isascii()
                and all(character.isprintable() for character in signature)
                and not any(character in signature for character in "{};#$`\"'\\")
                for signature in signatures
            ):
                raise ProfileError(
                    f"module entry {index} C++ module index symbol "
                    f"{symbol_name!r} has invalid callable_signatures"
                )
            if len(set(signatures)) != len(signatures):
                raise ProfileError(
                    f"module entry {index} C++ module index symbol "
                    f"{symbol_name!r} repeats a callable signature"
                )
            normalized_symbols[symbol_name] = {
                "kind": kind,
                "callable_signatures": signatures,
            }
        normalized_modules[binding_path] = {
            "cpp_module": cpp_module,
            "namespace": namespace,
            "symbols": dict(sorted(normalized_symbols.items())),
        }

    normalized = {
        "version": 1,
        "modules": dict(sorted(normalized_modules.items())),
    }
    rendered = (
        json.dumps(normalized, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    return normalized, rendered


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
    validate_profile_path("crate_root", data["crate_root"])
    validate_profile_path("output_root", data["output_root"])
    if not CPP_NAMESPACE_PATTERN.fullmatch(data["cxx_namespace"]):
        raise ProfileError(
            "cxx_namespace must be an identifier or ::-qualified identifiers"
        )
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

    allowed_module = {
        "source",
        "rust_module",
        "legacy_source",
        "module_name",
        "output",
        "kind",
        "dependencies",
        "legacy_dependencies",
        "gmf_headers",
        "type_mappings",
        "cpp_module_index",
    }
    seen_sources: set[str] = set()
    seen_rust_modules: set[str] = set()
    seen_legacy_sources: set[str] = set()
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
        for key in ("source", "rust_module", "module_name", "output", "kind"):
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
        if not re.fullmatch(
            r"crate(?:::[A-Za-z_]\w*)+", entry["rust_module"]
        ):
            raise ProfileError(
                f"module entry {index} has invalid Rust module path "
                f"{entry['rust_module']!r}"
            )
        dependencies = entry.get("dependencies", [])
        if not isinstance(dependencies, list) or not all(
            isinstance(dependency, str) for dependency in dependencies
        ):
            raise ProfileError(
                f"module entry {index} dependencies must be a string array"
            )
        if len(set(dependencies)) != len(dependencies):
            raise ProfileError(f"module entry {index} has duplicate dependencies")
        if entry["rust_module"] in dependencies:
            raise ProfileError(f"module entry {index} depends on itself")
        entry["dependencies"] = dependencies
        legacy_dependencies = entry.get("legacy_dependencies", [])
        if not isinstance(legacy_dependencies, list) or not all(
            isinstance(dependency, str) for dependency in legacy_dependencies
        ):
            raise ProfileError(
                f"module entry {index} legacy_dependencies must be a string array"
            )
        if len(set(legacy_dependencies)) != len(legacy_dependencies):
            raise ProfileError(
                f"module entry {index} has duplicate legacy_dependencies"
            )
        for dependency in legacy_dependencies:
            if not CPP_MODULE_NAME_PATTERN.fullmatch(dependency):
                raise ProfileError(
                    f"module entry {index} has invalid legacy dependency "
                    f"{dependency!r}"
                )
            if dependency == entry["module_name"]:
                raise ProfileError(
                    f"module entry {index} has self legacy dependency {dependency!r}"
                )
        entry["legacy_dependencies"] = sorted(legacy_dependencies)
        validate_profile_path(f"module entry {index} source", entry["source"])
        validate_profile_path(f"module entry {index} output", entry["output"])
        headers = entry.get("gmf_headers", [])
        if not isinstance(headers, list) or not all(isinstance(h, str) for h in headers):
            raise ProfileError(f"module entry {index} gmf_headers must be a string array")
        for header in headers:
            if not re.fullmatch(r'<[^<>\n]+>|"[^"\n]+"', header):
                raise ProfileError(
                    f"module entry {index} has invalid GMF header {header!r}"
                )
        entry["gmf_headers"] = headers
        entry["type_mappings"] = validate_type_mappings(
            index, entry.get("type_mappings", {})
        )

        cpp_module_index_label = entry.get("cpp_module_index")
        if cpp_module_index_label is not None and (
            not isinstance(cpp_module_index_label, str) or not cpp_module_index_label
        ):
            raise ProfileError(
                f"module entry {index} cpp_module_index must be a non-empty string"
            )
        if cpp_module_index_label is not None:
            validate_profile_path(
                f"module entry {index} cpp_module_index", cpp_module_index_label
            )
            if not CPP_INDEX_PATH_PATTERN.fullmatch(cpp_module_index_label):
                raise ProfileError(
                    f"module entry {index} cpp_module_index must use a conservative "
                    "repository-relative .json or .toml path"
                )
            cpp_module_index_path = (root / cpp_module_index_label).resolve()
            if not within(cpp_module_index_path, root):
                raise ProfileError(
                    f"module entry {index} cpp_module_index escapes repository"
                )
            try:
                cpp_module_index_raw = cpp_module_index_path.read_bytes()
            except OSError as exc:
                raise ProfileError(
                    f"cannot read C++ module index {cpp_module_index_path}: {exc}"
                ) from exc
            cpp_module_index, cpp_module_index_bytes = validate_cpp_module_index(
                index,
                cpp_module_index_path,
                cpp_module_index_raw,
                entry["legacy_dependencies"],
            )
        else:
            cpp_module_index_path = None
            cpp_module_index_raw = b""
            cpp_module_index = None
            cpp_module_index_bytes = b""
        entry["_cpp_module_index_path"] = cpp_module_index_path
        entry["_cpp_module_index_raw"] = cpp_module_index_raw
        entry["_cpp_module_index"] = cpp_module_index
        entry["_cpp_module_index_bytes"] = cpp_module_index_bytes

        source = (crate_dir / entry["source"]).resolve()
        legacy_source_label = entry.get("legacy_source")
        if legacy_source_label is not None and (
            not isinstance(legacy_source_label, str) or not legacy_source_label
        ):
            raise ProfileError(
                f"module entry {index} legacy_source must be a non-empty string"
            )
        if legacy_source_label is not None and (
            "\n" in legacy_source_label or "\r" in legacy_source_label
        ):
            raise ProfileError(
                f"module entry {index} legacy_source must fit on one line"
            )
        if legacy_source_label is not None:
            validate_profile_path(
                f"module entry {index} legacy_source", legacy_source_label
            )
        legacy_source = (
            (root / legacy_source_label).resolve()
            if legacy_source_label is not None
            else None
        )
        output = (output_dir / entry["output"]).resolve()
        if not within(source, crate_dir):
            raise ProfileError(f"module entry {index} source escapes crate_root")
        if legacy_source is not None and not within(legacy_source, root):
            raise ProfileError(f"module entry {index} legacy_source escapes repository")
        if not within(output, output_dir):
            raise ProfileError(f"module entry {index} output escapes output_root")
        if Path(entry["source"]).suffix != ".rs":
            raise ProfileError(f"module entry {index} source must end in .rs")
        if legacy_source_label is not None and Path(legacy_source_label).suffix not in {
            ".cpp",
            ".cc",
            ".cxx",
        }:
            raise ProfileError(
                f"module entry {index} legacy_source must be a C++ source path"
            )
        expected_suffix = ".cppm" if entry["kind"] == "interface" else ".cpp"
        if Path(entry["output"]).suffix != expected_suffix:
            raise ProfileError(
                f"module entry {index} {entry['kind']} output must end in {expected_suffix}"
            )
        if not CPP_MODULE_NAME_PATTERN.fullmatch(entry["module_name"]):
            raise ProfileError(
                f"module entry {index} has invalid C++ module name {entry['module_name']!r}"
            )
        unique_values = [
            ("source", entry["source"], seen_sources),
            ("rust_module", entry["rust_module"], seen_rust_modules),
            ("module_name", entry["module_name"], seen_modules),
            ("output", entry["output"], seen_outputs),
        ]
        if legacy_source_label is not None:
            unique_values.append(
                ("legacy_source", legacy_source_label, seen_legacy_sources)
            )
        for label, value, seen in unique_values:
            if value in seen:
                raise ProfileError(f"duplicate module {label}: {value}")
            seen.add(value)
        entry["_source"] = source
        entry["_legacy_source"] = legacy_source
        entry["_output"] = output

    mapped_rust_modules = {
        entry["rust_module"]: index for index, entry in enumerate(modules)
    }
    for index, entry in enumerate(modules, 1):
        for dependency in entry["dependencies"]:
            if not re.fullmatch(r"crate(?:::[A-Za-z_]\w*)+", dependency):
                raise ProfileError(
                    f"module entry {index} has invalid dependency {dependency!r}"
                )
            dependency_index = mapped_rust_modules.get(dependency)
            if dependency_index is None:
                raise ProfileError(
                    f"module entry {index} has unmapped dependency {dependency!r}"
                )
            if dependency_index >= index - 1:
                raise ProfileError(
                    f"module entry {index} dependency {dependency!r} must precede "
                    "its consumer in topological manifest order"
                )
        # A manifest-owned C++ module is never a legacy edge, even when the
        # corresponding Rust dependency was accidentally omitted above.  The
        # emitted-import check will independently diagnose that missing Rust
        # graph edge during generation, but --emit-cmake and schema validation
        # must already reject the contradictory ownership declaration.
        overlap = sorted(seen_modules & set(entry["legacy_dependencies"]))
        if overlap:
            raise ProfileError(
                f"module entry {index} declares manifest-owned dependency as legacy: "
                f"{', '.join(overlap)}"
            )
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


def stage_type_map(entry: dict, destination: Path) -> Path | None:
    """Write one read-only type map for one module invocation, if configured."""
    data = render_type_map(entry["type_mappings"])
    if not data:
        return None
    path = destination / f"{entry['module_name']}.toml"
    try:
        destination.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o400)
    except OSError as exc:
        raise ProfileError(
            f"cannot stage type mappings for {entry['module_name']}: {exc}"
        ) from exc
    return path


def stage_cpp_module_index(entry: dict, destination: Path) -> Path | None:
    """Write one canonical, read-only foreign-symbol index for one invocation."""
    data: bytes = entry["_cpp_module_index_bytes"]
    if not data:
        return None
    path = destination / f"{entry['module_name']}.json"
    try:
        destination.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o400)
    except OSError as exc:
        raise ProfileError(
            f"cannot stage C++ module index for {entry['module_name']}: {exc}"
        ) from exc
    return path


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


def has_exported_items(cpp: str) -> bool:
    """Return whether an interface exports an item, including nested namespaces."""
    for line in cpp.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("export ") and not stripped.startswith(
            "export module "
        ):
            return True
    return False


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


def emitted_cpp_imports(cpp: str, module_name: str) -> set[str]:
    """Parse emitted named-module imports and reject unsupported spellings."""
    imports: set[str] = set()
    for line_no, line in enumerate(cpp.splitlines(), 1):
        stripped = line.strip()
        if not (
            stripped.startswith("import ")
            or stripped.startswith("export import ")
        ):
            continue
        match = re.fullmatch(
            r"(?:export\s+)?import\s+"
            r"([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*;",
            stripped,
        )
        if match is None:
            raise ProfileError(
                f"unsupported emitted import in {module_name} at line {line_no}: "
                f"{stripped!r}"
            )
        imports.add(match.group(1))
    return imports


def validate_dependency_imports(cpp: str, entry: dict, manifest: dict) -> None:
    """Require Rust-owned and declared-legacy imports to match exactly."""
    module_by_rust_path = {
        module["rust_module"]: module["module_name"]
        for module in manifest["module"]
    }
    known_cpp_modules = set(module_by_rust_path.values())
    expected_owned = {
        module_by_rust_path[dependency] for dependency in entry["dependencies"]
    }
    expected_legacy = set(entry.get("legacy_dependencies", []))
    expected = expected_owned | expected_legacy
    imports = emitted_cpp_imports(cpp, entry["module_name"])
    actual = imports - IMPLICIT_CPP_MODULE_IMPORTS
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        parts = []
        if missing:
            parts.append(f"missing emitted imports: {', '.join(missing)}")
        if extra:
            parts.append(f"undeclared emitted imports: {', '.join(extra)}")
        raise ProfileError(
            f"dependency drift in {entry['module_name']}: {'; '.join(parts)}"
        )

    legacy_owned_overlap = sorted(expected_legacy & known_cpp_modules)
    if legacy_owned_overlap:
        raise ProfileError(
            f"manifest-owned imports declared legacy in {entry['module_name']}: "
            f"{', '.join(legacy_owned_overlap)}"
        )


def write_consumer_module_map(manifest: dict, path: Path) -> None:
    """Write the emitter's complete consumer module map deterministically."""
    document = {
        "version": 1,
        "module": [
            {
                "rust_module": entry["rust_module"],
                "cpp_module": entry["module_name"],
                "cpp_namespace": manifest["cxx_namespace"],
            }
            for entry in manifest["module"]
        ],
    }
    path.write_text(
        json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )


def stamp_fields(
    *,
    manifest_path: Path,
    manifest_sha: str,
    profile: str,
    source_label: str,
    source_sha: str,
    module_name: str,
    kind: str,
    type_map_sha: str,
    legacy_dependencies_sha: str,
    cpp_module_index_source_sha: str,
    cpp_module_index_sha: str,
    transpiler_git: str,
    transpiler_sha256: str,
    root: Path,
) -> list[tuple[str, str]]:
    manifest_label = manifest_path.resolve().relative_to(root).as_posix()
    return [
        ("generator-version", str(GENERATOR_VERSION)),
        ("generator-sha256", sha256_file(Path(__file__).resolve())),
        ("profile", profile),
        ("profile-manifest", manifest_label),
        ("profile-sha256", manifest_sha),
        ("source", source_label),
        ("source-sha256", source_sha),
        ("transpiler-git", transpiler_git),
        ("transpiler-sha256", transpiler_sha256),
        ("module", module_name),
        ("unit-kind", kind),
        ("type-map-sha256", type_map_sha),
        ("legacy-dependencies-sha256", legacy_dependencies_sha),
        ("cpp-module-index-source-sha256", cpp_module_index_source_sha),
        ("cpp-module-index-sha256", cpp_module_index_sha),
    ]


def render_stamp(fields: list[tuple[str, str]]) -> bytes:
    return "".join(f"// srpc-cpp-{key}: {value}\n" for key, value in fields).encode()


def stamp_output(cpp: str, fields: list[tuple[str, str]]) -> bytes:
    """Stamp output with a checksum over every byte except its own hash line."""
    body = cpp.lstrip("\n").encode("utf-8")
    unhashed = render_stamp(fields) + body
    integrity = sha256_bytes(unhashed)
    return render_stamp(fields + [("output-sha256", integrity)]) + body


def build_transpiler_command(
    *,
    transpiler: Path,
    staged_source: Path,
    raw_output: Path,
    module_name: str,
    cxx_namespace: str,
    consumer_module_map: Path,
    type_map: Path | None,
    cpp_module_index: Path | None,
) -> list[str]:
    """Build shell-free argv with invocation-local sidecars only."""
    command = [
        str(transpiler),
        str(staged_source),
        "-o",
        str(raw_output),
        "-m",
        module_name,
        "--cxx-namespace",
        cxx_namespace,
        "--consumer-module-map",
        str(consumer_module_map),
    ]
    if type_map is not None:
        command.extend(["--type-map", str(type_map)])
    if cpp_module_index is not None:
        command.extend(["--cpp-module-index", str(cpp_module_index)])
    return command


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
    consumer_module_map: Path,
) -> bytes:
    source: Path = entry["_source"]
    raw_output = scratch / entry["output"]
    raw_output.parent.mkdir(parents=True, exist_ok=True)
    type_map = stage_type_map(entry, scratch / "type-maps")
    cpp_module_index = stage_cpp_module_index(
        entry, scratch / "cpp-module-indexes"
    )
    command = build_transpiler_command(
        transpiler=transpiler,
        staged_source=staged_source,
        raw_output=raw_output,
        module_name=entry["module_name"],
        cxx_namespace=manifest["cxx_namespace"],
        consumer_module_map=consumer_module_map,
        type_map=type_map,
        cpp_module_index=cpp_module_index,
    )
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
    validate_dependency_imports(cpp, entry, manifest)
    if entry["kind"] == "interface" and not has_exported_items(cpp):
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
        type_map_sha=sha256_bytes(render_type_map(entry["type_mappings"])),
        legacy_dependencies_sha=sha256_bytes(
            render_legacy_dependencies(entry["legacy_dependencies"])
        ),
        cpp_module_index_source_sha=sha256_bytes(
            entry["_cpp_module_index_raw"]
        ),
        cpp_module_index_sha=sha256_bytes(entry["_cpp_module_index_bytes"]),
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


def verify_cpp_module_index_snapshots(manifest: dict) -> None:
    """Reject a foreign-symbol sidecar changing after profile validation."""
    for entry in manifest["module"]:
        path: Path | None = entry["_cpp_module_index_path"]
        if path is None:
            continue
        try:
            actual = path.read_bytes()
        except OSError as exc:
            raise ProfileError(f"C++ module index changed during generation: {path}: {exc}") from exc
        if actual != entry["_cpp_module_index_raw"]:
            raise ProfileError(
                f"C++ module index changed during generation: {path}; "
                "retry from a stable tree"
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
            type_map_sha=sha256_bytes(render_type_map(entry["type_mappings"])),
            legacy_dependencies_sha=sha256_bytes(
                render_legacy_dependencies(entry["legacy_dependencies"])
            ),
            cpp_module_index_source_sha=sha256_bytes(
                entry["_cpp_module_index_raw"]
            ),
            cpp_module_index_sha=sha256_bytes(entry["_cpp_module_index_bytes"]),
            transpiler_git=manifest["transpiler_git"],
            transpiler_sha256=manifest["transpiler_sha256"],
            root=root,
        )
        verify_stamped_output(output, output_bytes, fields)
        try:
            output_text = output_bytes.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ProfileError(f"generated output is not UTF-8: {output}: {exc}") from exc
        validate_dependency_imports(output_text, entry, manifest)
        print(f"ok {output.relative_to(root)}")


def emit_cmake_manifest(manifest: dict, root: Path) -> None:
    """Print the generated/retired file sets for inclusion by CMake."""
    print("# Generated by scripts/generate_srpc_cpp.py --emit-cmake")
    print("set(SRPC_CPP_PROFILE_INPUT_FILES")
    # Configure-time stamp validation is the build's offline stale-output
    # gate.  Make every authored Rust owner, checked-in generated unit, and
    # optional foreign-symbol sidecar a configure dependency.  A plain
    # `cmake --build` must rerun the stamp check both when the source of truth
    # changes and when someone edits an output that would otherwise merely be
    # recompiled as an ordinary target source.
    profile_input_paths = {
        path
        for entry in manifest["module"]
        for path in (entry["_source"], entry["_output"])
    }
    profile_input_paths.update(
        entry["_cpp_module_index_path"]
        for entry in manifest["module"]
        if entry.get("_cpp_module_index_path") is not None
    )
    for path in sorted(profile_input_paths):
        relative = path.relative_to(root).as_posix()
        print(f'    "${{CMAKE_SOURCE_DIR}}/{relative}"')
    print(")")
    if profile_input_paths:
        print("set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS")
        print("    ${SRPC_CPP_PROFILE_INPUT_FILES}")
        print(")")
    print("set(SRPC_GENERATED_MODULE_FILES")
    for entry in manifest["module"]:
        relative = entry["_output"].relative_to(root).as_posix()
        print(f'    "${{CMAKE_SOURCE_DIR}}/{relative}"')
    print(")")
    print("set(SRPC_RETIRED_LEGACY_MODULE_FILES")
    for entry in manifest["module"]:
        legacy_source = entry["_legacy_source"]
        if legacy_source is not None:
            relative = legacy_source.relative_to(root).as_posix()
            print(f'    "${{CMAKE_SOURCE_DIR}}/{relative}"')
    print(")")


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
    mode.add_argument(
        "--emit-cmake",
        action="store_true",
        help="print manifest-owned generated and retired source sets for CMake",
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
        if args.emit_cmake:
            emit_cmake_manifest(manifest, root)
            return 0
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
        verify_cpp_module_index_snapshots(manifest)
        with tempfile.TemporaryDirectory(prefix="srpc-cpp-profile-") as temporary:
            scratch = Path(temporary)
            staged_transpiler = stage_transpiler(
                transpiler, scratch / "emitter", transpiler_sha256
            )
            staged_sources = stage_sources(
                manifest, snapshots, scratch / "sources"
            )
            consumer_module_map = scratch / "consumer-module-map.json"
            write_consumer_module_map(manifest, consumer_module_map)
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
                    consumer_module_map,
                )
                generated.append((entry["_output"], data))
        verify_source_snapshots(snapshots)
        verify_cpp_module_index_snapshots(manifest)

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
