#!/usr/bin/env python3
"""Create and verify Mako's source-derived native transaction fingerprint.

The manifest beside ``libmako.a`` is deliberately not trusted by Cargo.  Both
the CMake build and Cargo run this program against the current source tree,
generated configuration, effective compile commands, compiler, target CPU,
and C++ runtime.  Rust also references a digest-named C symbol, so a manifest
from one build cannot bless an archive from another build.

Only the Python standard library is used; this script is part of the native
build recipe and its own contents are fingerprinted.
"""

from __future__ import annotations

import argparse
from collections import Counter
import concurrent.futures
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable, Sequence


SCHEMA = 1
ALGORITHM = "sha256"
DOMAIN = b"mako-local-build-fingerprint\0v1\0"
FINGERPRINT_SIZE = 32
DEFAULT_MANIFEST = "generated/mako_local_build_manifest.json"
NATIVE_LINK_ARCHIVE_MANIFEST = "crates/mako-local/native-link-archives.txt"

# ``libmako.a`` also physically bundles every object from this object target,
# so it is part of the same closure even though Cargo does not name it as a
# separate archive. CMake's standard-library module is a build dependency, not
# an object member of any archive in the current build graph; the compiler,
# effective commands, scanned libc++ headers, and libc++/libc++abi binaries are
# fingerprinted separately below.
MAKO_BUNDLED_OBJECT_TARGETS = ("txlog_core_obj",)
GENERATED_IDENTITY = "generated/mako_local_build_identity.cc"
ABI_IMPLEMENTATION = "src/mako/storage/mako_local_abi.cc"

RECIPE_FILES = (
    "CMakeLists.txt",
    "src/rrr/CMakeLists.txt",
    "src/masstree/CMakeLists.txt",
    "src/masstree/ConfigureMasstree.cmake",
    "src/masstree/config-cmake.h.in",
    "third-party/rusty-cpp/CMakeLists.txt",
    "crates/mako-local/build.rs",
    NATIVE_LINK_ARCHIVE_MANIFEST,
    "scripts/mako_local_fingerprint.py",
)

# Effective commands are authoritative.  These cache values are retained as a
# readable diagnostic record and cover configuration whose effect might be
# delayed until generated input is rebuilt.
CACHE_KEYS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_FLAGS",
    "CMAKE_CXX_FLAGS_DEBUG",
    "CMAKE_CXX_FLAGS_RELEASE",
    "CMAKE_CXX_FLAGS_RELWITHDEBINFO",
    "CMAKE_CXX_FLAGS_MINSIZEREL",
    "CHECK_INVARIANTS",
    "DEBUG",
    "DISABLE_MULTI_VERSION",
    "FAIL_NEW_VERSION",
    "HASHTABLE",
    "MAKO_LOCAL_TEST_HOOKS",
    "MASSTREE_ENABLE_SUPERPAGE",
    "MASSTREE_MAX_KEY_LEN",
    "MASSTREE_ROW_TYPE",
    "MODE",
    "OPACITY",
    "STO_RMW",
    "USE_MALLOC_MODE",
)


class FingerprintError(RuntimeError):
    """A fingerprint cannot be computed or does not match."""


def read_native_link_archives(path: Path) -> tuple[tuple[str, str], ...]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise FingerprintError(
            f"cannot read native link archive manifest {path}: {error}"
        ) from error
    archives: list[tuple[str, str]] = []
    names: set[str] = set()
    paths: set[str] = set()
    for line_number, raw_line in enumerate(lines, 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 2:
            raise FingerprintError(
                f"{path}:{line_number} must contain exactly a library name and path"
            )
        target, relative = fields
        relative_path = Path(relative)
        if not re.fullmatch(r"[A-Za-z0-9_]+", target):
            raise FingerprintError(
                f"{path}:{line_number} has invalid library name {target!r}"
            )
        if (
            relative_path.is_absolute()
            or not relative_path.parts
            or any(part in (".", "..") for part in relative_path.parts)
        ):
            raise FingerprintError(
                f"{path}:{line_number} path must stay relative to the build directory: "
                f"{relative!r}"
            )
        if target in names:
            raise FingerprintError(
                f"{path}:{line_number} repeats library name {target!r}"
            )
        if relative in paths:
            raise FingerprintError(
                f"{path}:{line_number} repeats archive path {relative!r}"
            )
        names.add(target)
        paths.add(relative)
        archives.append((target, relative))
    if not archives:
        raise FingerprintError(f"native link archive manifest {path} is empty")
    if archives[0] != ("mako", "libmako.a"):
        raise FingerprintError(
            f"native link archive manifest {path} must start with mako libmako.a"
        )
    return tuple(archives)


# Cargo recreates this consumer-before-provider slice of CMake's native link
# closure. Verification fails closed on archive membership and object bytes.
RUST_LINKED_ARCHIVES = read_native_link_archives(
    Path(__file__).resolve().parents[1] / NATIVE_LINK_ARCHIVE_MANIFEST
)
RUST_LINKED_ARCHIVE_TARGETS = tuple(
    target for target, _relative in RUST_LINKED_ARCHIVES
)

# Selecting every matching output from compile_commands.json covers out-of-line
# implementations reached indirectly by the facade (for example shardClient
# and message), not merely the STO translation units that include its header.
NATIVE_CLOSURE_TARGETS = (
    "mako",
    *MAKO_BUNDLED_OBJECT_TARGETS,
    *(target for target in RUST_LINKED_ARCHIVE_TARGETS if target != "mako"),
)

# Archive paths are relative to the CMake build directory. The boolean records
# the one generated identity member deliberately excluded from the source hash
# to avoid making its fingerprint depend on itself.
ARCHIVE_COMPOSITIONS = (
    ("libmako.a", ("mako", *MAKO_BUNDLED_OBJECT_TARGETS), True),
    *(
        (relative, (target,), False)
        for target, relative in RUST_LINKED_ARCHIVES
        if target != "mako"
    ),
)


@dataclasses.dataclass(frozen=True, order=True)
class Record:
    kind: str
    name: str
    size: int
    sha256: str
    text: str | None = dataclasses.field(default=None, compare=False)

    @classmethod
    def from_bytes(
        cls, kind: str, name: str, contents: bytes, *, show_text: bool = False
    ) -> "Record":
        _validate_record_name(kind, "record kind")
        _validate_record_name(name, "record name")
        text = None
        if show_text:
            text = contents.decode("utf-8")
        return cls(kind, name, len(contents), hashlib.sha256(contents).hexdigest(), text)

    def manifest_row(self) -> dict[str, object]:
        row: dict[str, object] = {
            "kind": self.kind,
            "name": self.name,
            "size": self.size,
            "sha256": self.sha256,
        }
        if self.text is not None:
            row["text"] = self.text
        return row


@dataclasses.dataclass(frozen=True, order=True)
class ObjectContent:
    basename: str
    size: int
    sha256: str

    @classmethod
    def from_bytes(cls, basename: str, contents: bytes) -> "ObjectContent":
        return cls(basename, len(contents), hashlib.sha256(contents).hexdigest())


@dataclasses.dataclass
class Snapshot:
    source_root: Path
    build_dir: Path
    engine_id: str
    records: list[Record]
    dependencies: set[Path]

    @property
    def fingerprint(self) -> str:
        return fingerprint_records(self.records)

    def manifest(self) -> dict[str, object]:
        return {
            "schema": SCHEMA,
            "algorithm": ALGORITHM,
            "engine_id": self.engine_id,
            "fingerprint": self.fingerprint,
            "records": [record.manifest_row() for record in sorted(self.records)],
        }


class PathNormalizer:
    def __init__(
        self,
        source_root: Path,
        build_dir: Path,
        compiler: Path,
        compiler_spelling: Path | None = None,
    ):
        self.source_root = source_root.resolve()
        self.build_dir = build_dir.resolve()
        compiler = compiler.resolve()
        # clang commonly lives in <toolchain>/bin.  Normalizing that prefix
        # keeps equivalent installations independent of their checkout/user.
        self.toolchain_root = compiler.parent.parent
        replacements = [
            (str(self.build_dir), "$BUILD"),
            (str(self.source_root), "$SRC"),
            (str(self.toolchain_root), "$TOOLCHAIN"),
        ]
        if compiler_spelling is not None:
            lexical_toolchain = compiler_spelling.absolute().parent.parent
            replacements.append((str(lexical_toolchain), "$TOOLCHAIN"))
        self._replacements = sorted(
            replacements,
            key=lambda item: len(item[0]),
            reverse=True,
        )

    def text(self, value: str) -> str:
        normalized = value
        for prefix, replacement in self._replacements:
            normalized = normalized.replace(prefix, replacement)
        return normalized.replace(os.sep, "/") if os.sep != "/" else normalized

    def path(self, value: Path) -> str:
        return self.text(str(value.resolve()))


def _validate_record_name(value: str, description: str) -> None:
    if not value or "\0" in value or "\n" in value or "\r" in value:
        raise FingerprintError(f"invalid {description}: {value!r}")


def fingerprint_records(records: Iterable[Record]) -> str:
    ordered = sorted(records)
    seen: set[tuple[str, str]] = set()
    digest = hashlib.sha256()
    digest.update(DOMAIN)
    for record in ordered:
        key = (record.kind, record.name)
        if key in seen:
            raise FingerprintError(f"duplicate fingerprint record {key!r}")
        seen.add(key)
        kind = record.kind.encode("utf-8")
        name = record.name.encode("utf-8")
        content_digest = bytes.fromhex(record.sha256)
        if len(content_digest) != FINGERPRINT_SIZE:
            raise FingerprintError(f"invalid SHA-256 for {record.kind}:{record.name}")
        digest.update(len(kind).to_bytes(4, "big"))
        digest.update(kind)
        digest.update(len(name).to_bytes(4, "big"))
        digest.update(name)
        digest.update(record.size.to_bytes(8, "big"))
        digest.update(content_digest)
    return digest.hexdigest()


def parse_cmake_cache(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise FingerprintError(f"cannot read CMake cache {path}: {error}") from error
    values: dict[str, str] = {}
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        entry, value = line.split("=", 1)
        if ":" not in entry:
            continue
        name, _kind = entry.split(":", 1)
        values[name] = value
    return values


def read_compile_commands(path: Path) -> list[dict[str, object]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FingerprintError(f"cannot read compile database {path}: {error}") from error
    if not isinstance(document, list):
        raise FingerprintError(f"compile database {path} is not a JSON array")
    return document


def entry_arguments(entry: dict[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        try:
            return shlex.split(command)
        except ValueError as error:
            raise FingerprintError(f"cannot parse compile command: {error}") from error
    raise FingerprintError("compile database entry has neither string command nor arguments")


def resolve_executable(value: str, directory: Path) -> Path:
    candidate = Path(value)
    if candidate.is_absolute():
        resolved = candidate
    elif candidate.parent != Path("."):
        resolved = directory / candidate
    else:
        found = shutil.which(value)
        if found is None:
            raise FingerprintError(f"cannot locate compiler executable {value!r}")
        resolved = Path(found)
    try:
        return resolved.resolve(strict=True)
    except OSError as error:
        raise FingerprintError(f"cannot resolve compiler executable {resolved}: {error}") from error


_SEPARATE_OUTPUT_OPTIONS = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
_STANDALONE_DEP_OPTIONS = {"-c", "-MD", "-MMD", "-MP", "-MG"}
_PREFIXED_OUTPUT_OPTIONS = ("-o", "-MF", "-MT", "-MQ", "-MJ")


def strip_compile_outputs(arguments: Sequence[str], *, strip_modmap: bool) -> list[str]:
    if not arguments:
        raise FingerprintError("empty compile command")
    result = [arguments[0]]
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument in _SEPARATE_OUTPUT_OPTIONS:
            index += 2
            continue
        if argument in _STANDALONE_DEP_OPTIONS:
            index += 1
            continue
        if any(
            argument.startswith(prefix) and argument != prefix
            for prefix in _PREFIXED_OUTPUT_OPTIONS
        ):
            index += 1
            continue
        if strip_modmap and argument.startswith("@") and argument.endswith(".modmap"):
            index += 1
            continue
        result.append(argument)
        index += 1
    return result


def parse_make_dependencies(output: str, directory: Path) -> list[Path]:
    logical = output.replace("\\\r\n", " ").replace("\\\n", " ")
    if ":" not in logical:
        raise FingerprintError("compiler dependency output has no target separator")
    _target, body = logical.split(":", 1)
    try:
        fields = shlex.split(body, posix=True)
    except ValueError as error:
        raise FingerprintError(f"cannot parse compiler dependency output: {error}") from error
    dependencies: list[Path] = []
    for field in fields:
        path = Path(field)
        if not path.is_absolute():
            path = directory / path
        try:
            dependencies.append(path.resolve(strict=True))
        except OSError as error:
            raise FingerprintError(
                f"compiler reported missing fingerprint input {path}: {error}"
            ) from error
    return dependencies


@dataclasses.dataclass(frozen=True)
class CompileRoot:
    target: str
    output: str
    source: Path
    directory: Path
    arguments: tuple[str, ...]
    compiler: Path
    compiler_spelling: Path

    @property
    def inventory_name(self) -> str:
        return f"{self.target}:{self.output}"


def _canonical_target_output(output: str, build_dir: Path) -> str:
    path = Path(output)
    if path.is_absolute():
        try:
            path = path.resolve().relative_to(build_dir.resolve())
        except ValueError as error:
            raise FingerprintError(
                f"target output {output!r} is outside CMake build directory {build_dir}"
            ) from error
    value = path.as_posix()
    if not value or value.startswith("../"):
        raise FingerprintError(f"invalid target output path {output!r}")
    return value


def _entry_target(output: str) -> str | None:
    normalized = output.replace("\\", "/")
    matches = [
        target
        for target in NATIVE_CLOSURE_TARGETS
        if re.search(rf"(?:^|/)CMakeFiles/{re.escape(target)}\.dir/", normalized)
    ]
    if len(matches) > 1:
        raise FingerprintError(f"compile output ambiguously belongs to targets {matches}: {output}")
    return matches[0] if matches else None


def select_compile_roots(
    database: Sequence[dict[str, object]], source_root: Path, build_dir: Path
) -> list[CompileRoot]:
    roots: list[CompileRoot] = []
    seen_outputs: set[tuple[str, str]] = set()
    target_counts = {target: 0 for target in NATIVE_CLOSURE_TARGETS}
    identity_entries = 0
    abi_entries = 0
    identity_source = (build_dir / GENERATED_IDENTITY).resolve()
    abi_source = (source_root / ABI_IMPLEMENTATION).resolve()

    for entry in database:
        output_value = entry.get("output")
        if not isinstance(output_value, str):
            continue
        target = _entry_target(output_value)
        if target is None:
            continue
        file_value = entry.get("file")
        directory_value = entry.get("directory")
        if not isinstance(file_value, str) or not isinstance(directory_value, str):
            raise FingerprintError(
                f"compile entry for {target} output {output_value!r} lacks file/directory"
            )
        directory = Path(directory_value).resolve()
        source = Path(file_value)
        if not source.is_absolute():
            source = directory / source
        source = source.resolve()

        if source == identity_source:
            if target != "mako":
                raise FingerprintError(
                    f"generated identity unexpectedly belongs to archive target {target}"
                )
            identity_entries += 1
            continue

        output = _canonical_target_output(output_value, build_dir)
        output_key = (target, output)
        if output_key in seen_outputs:
            raise FingerprintError(
                f"duplicate compile-database member for archive {target}: {output}"
            )
        seen_outputs.add(output_key)
        target_counts[target] += 1
        if source == abi_source:
            if target != "mako":
                raise FingerprintError(
                    f"ABI implementation unexpectedly belongs to archive target {target}"
                )
            abi_entries += 1

        arguments = entry_arguments(entry)
        compiler = resolve_executable(arguments[0], directory)
        compiler_spelling = Path(arguments[0])
        if not compiler_spelling.is_absolute():
            if compiler_spelling.parent == Path("."):
                located = shutil.which(arguments[0])
                if located is None:
                    raise FingerprintError(f"cannot locate compiler {arguments[0]!r}")
                compiler_spelling = Path(located)
            else:
                compiler_spelling = directory / compiler_spelling
        roots.append(
            CompileRoot(
                target,
                output,
                source,
                directory,
                tuple(arguments),
                compiler,
                compiler_spelling,
            )
        )

    missing = [target for target, count in target_counts.items() if count == 0]
    if missing:
        raise FingerprintError(
            "compile database has no members for required linked archive target(s): "
            + ", ".join(missing)
        )
    if identity_entries != 1:
        raise FingerprintError(
            f"expected exactly one excluded generated identity member in mako, found {identity_entries}"
        )
    if abi_entries != 1:
        raise FingerprintError(
            f"expected exactly one {ABI_IMPLEMENTATION} member in mako, found {abi_entries}"
        )
    roots.sort(key=lambda root: (NATIVE_CLOSURE_TARGETS.index(root.target), root.output))
    compilers = {root.compiler for root in roots}
    if len(compilers) != 1:
        raise FingerprintError(f"linked archive members use multiple compilers: {compilers}")
    return roots


def generated_identity_output(
    database: Sequence[dict[str, object]], build_dir: Path
) -> str:
    identity_source = (build_dir / GENERATED_IDENTITY).resolve()
    matches: list[str] = []
    for entry in database:
        output_value = entry.get("output")
        if not isinstance(output_value, str) or _entry_target(output_value) != "mako":
            continue
        file_value = entry.get("file")
        directory_value = entry.get("directory")
        if not isinstance(file_value, str) or not isinstance(directory_value, str):
            continue
        directory = Path(directory_value).resolve()
        source = Path(file_value)
        if not source.is_absolute():
            source = directory / source
        if source.resolve() == identity_source:
            output = _canonical_target_output(output_value, build_dir)
            matches.append(output)
    if len(matches) != 1:
        raise FingerprintError(
            f"expected exactly one generated identity archive member, found {len(matches)}"
        )
    return matches[0]


def generated_identity_member(
    database: Sequence[dict[str, object]], build_dir: Path
) -> str:
    return Path(generated_identity_output(database, build_dir)).name


def scan_one_root(root: CompileRoot) -> tuple[str, list[Path]]:
    arguments = strip_compile_outputs(root.arguments, strip_modmap=True)
    arguments.extend(("-M", "-MT", "mako_local_fingerprint"))
    try:
        result = subprocess.run(
            arguments,
            cwd=root.directory,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FingerprintError(
            f"dependency scan failed to start for {root.inventory_name}: {error}"
        ) from error
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        raise FingerprintError(
            f"dependency scan failed for {root.inventory_name} (exit {result.returncode}):\n"
            f"{diagnostic}"
        )
    return root.inventory_name, parse_make_dependencies(result.stdout, root.directory)


def dependency_scan_worker_count(task_count: int) -> int:
    if task_count <= 0:
        raise FingerprintError(
            f"dependency scan requires at least one compile root, got {task_count}"
        )

    requested = 8
    source = "safe default"
    for variable in (
        "MAKO_LOCAL_FINGERPRINT_JOBS",
        "CI_BUILD_JOBS",
        "CI_MAKE_JOBS",
    ):
        if variable not in os.environ:
            continue
        raw = os.environ[variable]
        if not re.fullmatch(r"[1-9][0-9]*", raw):
            raise FingerprintError(
                f"{variable} must be a positive integer, got {raw!r}"
            )
        try:
            requested = int(raw)
        except ValueError as error:
            raise FingerprintError(
                f"{variable} must be a positive integer, got {raw!r}"
            ) from error
        source = variable
        break

    cpu_count = os.cpu_count() or 1
    if cpu_count < 1:
        raise FingerprintError(f"invalid host CPU count {cpu_count} while applying {source}")
    return min(requested, cpu_count, task_count)


def scan_dependencies(roots: Sequence[CompileRoot]) -> dict[str, list[Path]]:
    workers = dependency_scan_worker_count(len(roots))
    results: dict[str, list[Path]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(scan_one_root, root): root for root in roots}
        for future in concurrent.futures.as_completed(futures):
            root = futures[future]
            try:
                relative, dependencies = future.result()
            except FingerprintError:
                for pending in futures:
                    pending.cancel()
                raise
            results[relative] = dependencies
    return results


def normalize_command(root: CompileRoot, normalizer: PathNormalizer) -> str:
    arguments = strip_compile_outputs(root.arguments, strip_modmap=False)
    normalized = ["$CXX"]
    normalized.extend(normalizer.text(argument) for argument in arguments[1:])
    return "\0".join(normalized)


def archive_inventory_records(
    roots: Sequence[CompileRoot], normalizer: PathNormalizer
) -> list[Record]:
    records = []
    for target in NATIVE_CLOSURE_TARGETS:
        inventory = "".join(
            f"{normalizer.text(root.output)}\n"
            for root in roots
            if root.target == target
        ).encode("utf-8")
        records.append(
            Record.from_bytes("archive-inventory", target, inventory, show_text=True)
        )
    return records


def expected_archive_members(
    roots: Sequence[CompileRoot], identity_member: str
) -> dict[str, Counter[str]]:
    by_target: dict[str, Counter[str]] = {
        target: Counter() for target in NATIVE_CLOSURE_TARGETS
    }
    for root in roots:
        if root.target not in by_target:
            raise FingerprintError(
                f"compile member belongs to unlisted native target {root.target!r}"
            )
        member = Path(root.output).name
        if not member:
            raise FingerprintError(f"compile output has no archive basename: {root.output}")
        by_target[root.target][member] += 1

    expected: dict[str, Counter[str]] = {}
    for archive, targets, includes_identity in ARCHIVE_COMPOSITIONS:
        members: Counter[str] = Counter()
        for target in targets:
            members.update(by_target[target])
        if includes_identity:
            members[identity_member] += 1
        expected[archive] = members
    return expected


def expected_archive_object_paths(
    roots: Sequence[CompileRoot], identity_output: str, build_dir: Path
) -> dict[str, list[tuple[str, Path]]]:
    by_target: dict[str, list[tuple[str, Path]]] = {
        target: [] for target in NATIVE_CLOSURE_TARGETS
    }
    for root in roots:
        if root.target not in by_target:
            raise FingerprintError(
                f"compile member belongs to unlisted native target {root.target!r}"
            )
        member = Path(root.output).name
        by_target[root.target].append((member, build_dir / root.output))

    expected: dict[str, list[tuple[str, Path]]] = {}
    for archive, targets, includes_identity in ARCHIVE_COMPOSITIONS:
        objects: list[tuple[str, Path]] = []
        for target in targets:
            objects.extend(by_target[target])
        if includes_identity:
            objects.append((Path(identity_output).name, build_dir / identity_output))
        expected[archive] = objects
    return expected


def compare_archive_members(
    archive: Path, expected: Counter[str], actual: Counter[str]
) -> None:
    missing = expected - actual
    unexpected = actual - expected
    if not missing and not unexpected:
        return

    def describe(label: str, members: Counter[str]) -> str:
        rendered = ", ".join(
            f"{name} (x{count})" if count != 1 else name
            for name, count in sorted(members.items())
        )
        return f"{label}: {rendered}"

    details = []
    if missing:
        details.append(describe("missing", missing))
    if unexpected:
        details.append(describe("unexpected", unexpected))
    raise FingerprintError(
        f"native archive composition does not match compile database for {archive}:\n  "
        + "\n  ".join(details)
        + "\nrebuild/reconfigure the native target and update the fingerprint closure "
        "if CMake intentionally changed archive membership"
    )


def object_content_from_path(
    basename: str, path: Path, description: str
) -> ObjectContent:
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                digest.update(chunk)
    except OSError as error:
        raise FingerprintError(f"cannot read {description} {path}: {error}") from error
    return ObjectContent(basename, size, digest.hexdigest())


def hash_expected_archive_objects(
    expected_paths: dict[str, list[tuple[str, Path]]]
) -> dict[str, Counter[ObjectContent]]:
    cache: dict[Path, ObjectContent] = {}
    result: dict[str, Counter[ObjectContent]] = {}
    for archive, objects in expected_paths.items():
        contents: Counter[ObjectContent] = Counter()
        for basename, path in objects:
            try:
                resolved = path.resolve(strict=True)
            except OSError as error:
                raise FingerprintError(
                    f"selected build object does not exist for {archive}: {path}: {error}"
                ) from error
            cached = cache.get(resolved)
            if cached is None:
                cached = object_content_from_path(basename, resolved, "selected build object")
                cache[resolved] = cached
            elif cached.basename != basename:
                raise FingerprintError(
                    f"selected build object has inconsistent archive basenames: "
                    f"{resolved} ({cached.basename!r} and {basename!r})"
                )
            contents[cached] += 1
        result[archive] = contents
    return result


def list_archive_members(archiver: Path, archive: Path) -> list[str]:
    if not archive.is_file():
        raise FingerprintError(f"required native archive does not exist: {archive}")
    try:
        result = subprocess.run(
            [str(archiver), "t", str(archive)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FingerprintError(f"cannot inspect native archive {archive}: {error}") from error
    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        raise FingerprintError(
            f"archiver failed to list {archive} (exit {result.returncode}): {diagnostic}"
        )
    members = [line for line in result.stdout.splitlines() if line]
    if not members:
        raise FingerprintError(f"native archive has no object members: {archive}")
    return members


def extract_archive_object_contents(
    archiver: Path, archive: Path, members: Sequence[str]
) -> Counter[ObjectContent]:
    contents: Counter[ObjectContent] = Counter()
    occurrences: Counter[str] = Counter()
    try:
        temporary = tempfile.TemporaryDirectory(prefix="mako-local-archive-")
    except OSError as error:
        raise FingerprintError(
            f"cannot create extraction directory for native archive {archive}: {error}"
        ) from error
    with temporary:
        temporary_root = Path(temporary.name)
        for index, member in enumerate(members):
            occurrences[member] += 1
            extraction_dir = temporary_root / f"{index:06d}"
            try:
                extraction_dir.mkdir()
                result = subprocess.run(
                    [
                        str(archiver),
                        "xN",
                        str(occurrences[member]),
                        "--output",
                        str(extraction_dir),
                        str(archive),
                        member,
                    ],
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=120,
                )
            except (OSError, subprocess.TimeoutExpired) as error:
                raise FingerprintError(
                    f"cannot extract member {member!r} occurrence "
                    f"{occurrences[member]} from {archive}: {error}"
                ) from error
            if result.returncode != 0:
                diagnostic = result.stderr.strip() or result.stdout.strip()
                raise FingerprintError(
                    f"archiver failed to extract member {member!r} occurrence "
                    f"{occurrences[member]} from {archive} (exit {result.returncode}): "
                    f"{diagnostic}"
                )
            extracted = [path for path in extraction_dir.rglob("*") if path.is_file()]
            if len(extracted) != 1:
                raise FingerprintError(
                    f"archiver extracted {len(extracted)} files for member {member!r} "
                    f"occurrence {occurrences[member]} from {archive}, expected one"
                )
            basename = Path(member).name
            if extracted[0].name != basename:
                raise FingerprintError(
                    f"archiver extracted unexpected member name {extracted[0].name!r}; "
                    f"expected {basename!r} from {archive}"
                )
            contents[
                object_content_from_path(
                    basename, extracted[0], "extracted native archive object"
                )
            ] += 1
    return contents


def compare_archive_object_contents(
    archive: Path,
    expected: Counter[ObjectContent],
    actual: Counter[ObjectContent],
) -> None:
    missing = expected - actual
    unexpected = actual - expected
    if not missing and not unexpected:
        return

    def describe(label: str, objects: Counter[ObjectContent]) -> str:
        rendered = ", ".join(
            f"{item.basename} size={item.size} sha256={item.sha256}"
            + (f" (x{count})" if count != 1 else "")
            for item, count in sorted(objects.items())
        )
        return f"{label}: {rendered}"

    details = []
    if missing:
        details.append(describe("selected build content missing from archive", missing))
    if unexpected:
        details.append(describe("different content present in archive", unexpected))
    raise FingerprintError(
        f"native archive object content does not match selected build objects for {archive}:\n  "
        + "\n  ".join(details)
        + "\nrebuild the native target; an archive from another or mixed build cannot "
        "be used with this manifest"
    )


def verify_archive_composition(source_root: Path, build_dir: Path) -> None:
    source_root = source_root.resolve()
    build_dir = build_dir.resolve()
    database = read_compile_commands(build_dir / "compile_commands.json")
    roots = select_compile_roots(database, source_root, build_dir)
    identity_output = generated_identity_output(database, build_dir)
    expected = expected_archive_members(roots, Path(identity_output).name)
    expected_paths = expected_archive_object_paths(roots, identity_output, build_dir)
    cache = parse_cmake_cache(build_dir / "CMakeCache.txt")
    archiver_value = cache.get("CMAKE_AR")
    if not archiver_value:
        raise FingerprintError("CMake cache does not define CMAKE_AR")
    archiver = resolve_executable(archiver_value, build_dir)

    # First report archive-composition drift using names and duplicate counts;
    # only after every inventory matches do we perform the costlier byte check.
    listed: dict[str, list[str]] = {}
    for relative, members in expected.items():
        archive = build_dir / relative
        listed[relative] = list_archive_members(archiver, archive)
        compare_archive_members(
            archive,
            members,
            Counter(Path(member).name for member in listed[relative]),
        )

    expected_contents = hash_expected_archive_objects(expected_paths)
    for relative, contents in expected_contents.items():
        archive = build_dir / relative
        actual_contents = extract_archive_object_contents(
            archiver, archive, listed[relative]
        )
        compare_archive_object_contents(archive, contents, actual_contents)


def compile_response_records(
    root: CompileRoot, normalizer: PathNormalizer
) -> tuple[list[Record], set[Path]]:
    records: list[Record] = []
    dependencies: set[Path] = set()
    for argument in root.arguments:
        if not argument.startswith("@"):
            continue
        # CMake creates module-map response files as build-order artifacts. They
        # may legitimately be absent while the identity TU is generated and
        # present later when Cargo verifies it. Their stable @ path remains in
        # the normalized effective command, while compiler-scanned inputs cover
        # the sources and headers that define semantics.
        if argument.endswith(".modmap"):
            continue
        response = Path(argument[1:])
        if not response.is_absolute():
            response = root.directory / response
        try:
            response = response.resolve(strict=True)
        except OSError as error:
            raise FingerprintError(
                f"compiler response file {response} does not exist: {error}"
            ) from error
        records.append(
            Record.from_bytes(
                "compile-response",
                f"{normalizer.text(root.inventory_name)}:{normalizer.path(response)}",
                normalizer.text(
                    _read_file(response, "compiler response file").decode("utf-8")
                ).encode("utf-8"),
                show_text=True,
            )
        )
        dependencies.add(response)
    return records, dependencies


def compiler_predefines(root: CompileRoot) -> bytes:
    arguments = strip_compile_outputs(root.arguments, strip_modmap=True)
    filtered = [arguments[0]]
    for argument in arguments[1:]:
        try:
            candidate = Path(argument)
            if not candidate.is_absolute():
                candidate = root.directory / candidate
            is_source = candidate.resolve() == root.source
        except (OSError, ValueError):
            is_source = False
        if not is_source:
            filtered.append(argument)
    filtered.extend(("-dM", "-E", "-x", "c++", "-"))
    try:
        result = subprocess.run(
            filtered,
            cwd=root.directory,
            input=b"",
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FingerprintError(f"compiler identity probe failed to start: {error}") from error
    if result.returncode != 0:
        raise FingerprintError(
            "compiler identity probe failed: "
            + result.stderr.decode("utf-8", errors="replace").strip()
        )
    return b"\n".join(sorted(result.stdout.splitlines())) + b"\n"


def compiler_text_probe(compiler: Path, argument: str) -> bytes:
    try:
        result = subprocess.run(
            (str(compiler), argument),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FingerprintError(f"compiler probe {argument} failed to start: {error}") from error
    if result.returncode != 0:
        raise FingerprintError(
            f"compiler probe {argument} failed: "
            + result.stdout.decode("utf-8", errors="replace").strip()
        )
    return result.stdout.replace(b"\r\n", b"\n")


def find_standard_libraries(cache: dict[str, str], compiler: Path) -> list[Path]:
    directories: list[Path] = []
    explicit = os.environ.get("LIBCXX_DIR")
    if explicit:
        directories.append(Path(explicit))
    flags = cache.get("CMAKE_EXE_LINKER_FLAGS", "")
    try:
        tokens = shlex.split(flags)
    except ValueError as error:
        raise FingerprintError(f"cannot parse CMAKE_EXE_LINKER_FLAGS: {error}") from error
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-L" and index + 1 < len(tokens):
            directories.append(Path(tokens[index + 1]))
            index += 2
            continue
        if token.startswith("-L") and len(token) > 2:
            directories.append(Path(token[2:]))
        index += 1
    directories.extend((compiler.parent.parent / "lib", compiler.parent / "../lib"))

    names_by_library = (
        ("libc++", ("libc++.so.1", "libc++.so", "libc++.dylib")),
        ("libc++abi", ("libc++abi.so.1", "libc++abi.so", "libc++abi.dylib")),
    )
    found: list[Path] = []
    for label, names in names_by_library:
        match = None
        for directory in directories:
            for name in names:
                candidate = directory / name
                if candidate.exists():
                    match = candidate.resolve()
                    break
            if match is not None:
                break
        if match is None:
            raise FingerprintError(
                f"cannot locate the configured {label} shared library; "
                "set LIBCXX_DIR to the exact directory used to link libmako.a"
            )
        found.append(match)
    return found


def read_engine_contract(header: Path) -> tuple[str, int]:
    try:
        source = header.read_text(encoding="utf-8")
    except OSError as error:
        raise FingerprintError(f"cannot read ABI header {header}: {error}") from error
    engine_matches = re.findall(r'^#define MAKO_LOCAL_ENGINE_ID "([^"\\]+)"$', source, re.M)
    size_matches = re.findall(
        r"^#define MAKO_LOCAL_BUILD_FINGERPRINT_SIZE ([0-9]+)u$", source, re.M
    )
    if len(engine_matches) != 1 or len(size_matches) != 1:
        raise FingerprintError(
            "ABI header must define exactly one simple MAKO_LOCAL_ENGINE_ID and "
            "MAKO_LOCAL_BUILD_FINGERPRINT_SIZE"
        )
    engine_id = engine_matches[0]
    if not re.fullmatch(r"[A-Za-z0-9._/-]+", engine_id):
        raise FingerprintError(f"engine ID is not a portable ASCII identifier: {engine_id!r}")
    size = int(size_matches[0])
    if size != FINGERPRINT_SIZE:
        raise FingerprintError(
            f"fingerprint header size is {size}, but schema {SCHEMA} requires {FINGERPRINT_SIZE}"
        )
    return engine_id, size


def _read_file(path: Path, description: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise FingerprintError(f"cannot read {description} {path}: {error}") from error


def compute_snapshot(source_root: Path, build_dir: Path) -> Snapshot:
    source_root = source_root.resolve()
    build_dir = build_dir.resolve()
    compile_db_path = build_dir / "compile_commands.json"
    cache_path = build_dir / "CMakeCache.txt"
    database = read_compile_commands(compile_db_path)
    cache = parse_cmake_cache(cache_path)
    roots = select_compile_roots(database, source_root, build_dir)
    compiler = roots[0].compiler
    normalizer = PathNormalizer(
        source_root, build_dir, compiler, roots[0].compiler_spelling
    )
    scanned = scan_dependencies(roots)

    header = source_root / "src/mako/storage/mako_local_abi.h"
    engine_id, _size = read_engine_contract(header)
    records: list[Record] = []
    dependencies: set[Path] = {compile_db_path.resolve(), cache_path.resolve()}

    # Hash every compiler-reported input once. This includes generated config,
    # compiler resource headers, libc++ headers, and system ABI headers.
    input_paths = {path for paths in scanned.values() for path in paths}
    for path in sorted(input_paths, key=lambda value: normalizer.path(value)):
        name = normalizer.path(path)
        records.append(Record.from_bytes("input", name, _read_file(path, "input")))
        dependencies.add(path)

    for relative in RECIPE_FILES:
        path = (source_root / relative).resolve()
        name = normalizer.path(path)
        key = ("input", name)
        if not any((record.kind, record.name) == key for record in records):
            records.append(Record.from_bytes("recipe", name, _read_file(path, "recipe")))
        dependencies.add(path)

    for root in roots:
        command = normalize_command(root, normalizer).encode("utf-8")
        records.append(
            Record.from_bytes(
                "compile-command",
                normalizer.text(root.inventory_name),
                command,
                show_text=True,
            )
        )
        response_records, response_dependencies = compile_response_records(
            root, normalizer
        )
        records.extend(response_records)
        dependencies.update(response_dependencies)

    records.extend(archive_inventory_records(roots, normalizer))

    cache_text = "".join(f"{key}={cache.get(key, '<unset>')}\n" for key in CACHE_KEYS)
    records.append(
        Record.from_bytes("configuration", "cmake-cache", cache_text.encode(), show_text=True)
    )

    records.extend(
        (
            Record.from_bytes("tool", "cxx-binary", _read_file(compiler, "compiler")),
            Record.from_bytes(
                "tool", "cxx-version", compiler_text_probe(compiler, "--version"), show_text=True
            ),
            Record.from_bytes(
                "tool", "cxx-target", compiler_text_probe(compiler, "-dumpmachine"), show_text=True
            ),
            Record.from_bytes(
                "tool", "cxx-predefined-macros", compiler_predefines(roots[0])
            ),
        )
    )
    dependencies.add(compiler)

    for label, library in zip(("libc++", "libc++abi"), find_standard_libraries(cache, compiler)):
        records.append(Record.from_bytes("tool", label, _read_file(library, label)))
        dependencies.add(library)

    return Snapshot(source_root, build_dir, engine_id, records, dependencies)


def render_cpp(snapshot: Snapshot) -> str:
    fingerprint = bytes.fromhex(snapshot.fingerprint)
    bytes_text = ", ".join(f"0x{value:02x}" for value in fingerprint)
    anchor = f"mako_local_build_anchor_{snapshot.fingerprint}"
    return f"""// @generated by scripts/mako_local_fingerprint.py; do not edit.
#include \"storage/mako_local_abi.h\"

namespace {{
constexpr uint8_t kMakoLocalBuildFingerprint[MAKO_LOCAL_BUILD_FINGERPRINT_SIZE] = {{
    {bytes_text}
}};
static_assert(sizeof(kMakoLocalBuildFingerprint) ==
              MAKO_LOCAL_BUILD_FINGERPRINT_SIZE);
}}  // namespace

extern \"C\" const char *mako_local_engine_id(void) noexcept {{
  return MAKO_LOCAL_ENGINE_ID;
}}

extern \"C\" const uint8_t *mako_local_build_fingerprint(void) noexcept {{
  return kMakoLocalBuildFingerprint;
}}

extern \"C\" size_t mako_local_build_fingerprint_size(void) noexcept {{
  return sizeof(kMakoLocalBuildFingerprint);
}}

extern \"C\" void {anchor}(void) noexcept {{}}
"""


def render_rust(snapshot: Snapshot) -> str:
    fingerprint = bytes.fromhex(snapshot.fingerprint)
    bytes_text = ", ".join(f"0x{value:02x}" for value in fingerprint)
    anchor = f"mako_local_build_anchor_{snapshot.fingerprint}"
    return f"""// @generated by scripts/mako_local_fingerprint.py; do not edit.
pub(super) const EXPECTED_ENGINE_ID: &[u8] = b\"{snapshot.engine_id}\";
pub(super) const EXPECTED_BUILD_FINGERPRINT: [u8; 32] = [{bytes_text}];

extern \"C\" {{
    #[link_name = \"{anchor}\"]
    fn build_anchor();
}}

pub(super) unsafe fn require_build_anchor() {{
    unsafe {{ build_anchor() }}
}}
"""


def _atomic_write_if_changed(path: Path, contents: bytes) -> None:
    try:
        if path.read_bytes() == contents:
            return
    except FileNotFoundError:
        pass
    except OSError as error:
        raise FingerprintError(f"cannot inspect output {path}: {error}") from error
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
            temporary.write(contents)
            temporary_path = Path(temporary.name)
        os.replace(temporary_path, path)
    except OSError as error:
        raise FingerprintError(f"cannot write output {path}: {error}") from error


def _dep_escape(path: Path) -> str:
    value = str(path)
    if "\n" in value or "\r" in value:
        raise FingerprintError(f"dependency path contains a newline: {path}")
    return (
        value.replace("\\", "\\\\")
        .replace(" ", "\\ ")
        .replace("#", "\\#")
        .replace(":", "\\:")
        .replace("$", "$$")
    )


def write_depfile(path: Path, target: Path, dependencies: Iterable[Path]) -> None:
    ordered = sorted({dependency.resolve() for dependency in dependencies}, key=str)
    line = _dep_escape(target.resolve()) + ":"
    chunks = [line]
    for dependency in ordered:
        chunks.append(" \\\n  " + _dep_escape(dependency))
    chunks.append("\n")
    _atomic_write_if_changed(path, "".join(chunks).encode("utf-8"))


def write_dependency_list(path: Path, dependencies: Iterable[Path]) -> None:
    ordered = sorted({dependency.resolve() for dependency in dependencies}, key=str)
    for dependency in ordered:
        if "\n" in str(dependency) or "\r" in str(dependency):
            raise FingerprintError(f"dependency path contains a newline: {dependency}")
    contents = "".join(f"{dependency}\n" for dependency in ordered)
    _atomic_write_if_changed(path, contents.encode("utf-8"))


def load_manifest(path: Path) -> dict[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FingerprintError(f"cannot read native build manifest {path}: {error}") from error
    if not isinstance(document, dict):
        raise FingerprintError(f"native build manifest {path} is not a JSON object")
    return document


def compare_manifest(expected: dict[str, object], current: Snapshot) -> None:
    if expected.get("schema") != SCHEMA or expected.get("algorithm") != ALGORITHM:
        raise FingerprintError(
            f"unsupported native build manifest schema/algorithm: "
            f"{expected.get('schema')!r}/{expected.get('algorithm')!r}"
        )
    if expected.get("engine_id") != current.engine_id:
        raise FingerprintError(
            f"native engine ID mismatch: manifest has {expected.get('engine_id')!r}, "
            f"current header requires {current.engine_id!r}"
        )
    expected_fingerprint = expected.get("fingerprint")
    if expected_fingerprint == current.fingerprint:
        # Still validate the record inventory; this turns manual manifest
        # corruption into a direct error rather than relying on hash luck.
        expected_rows = expected.get("records")
        current_rows = [record.manifest_row() for record in sorted(current.records)]
        if expected_rows == current_rows:
            return

    expected_records: dict[tuple[object, object], tuple[object, object]] = {}
    rows = expected.get("records")
    if isinstance(rows, list):
        for row in rows:
            if isinstance(row, dict):
                expected_records[(row.get("kind"), row.get("name"))] = (
                    row.get("size"),
                    row.get("sha256"),
                )
    current_records = {
        (record.kind, record.name): (record.size, record.sha256) for record in current.records
    }
    changes: list[str] = []
    for key in sorted(expected_records.keys() | current_records.keys(), key=str):
        if key not in expected_records:
            changes.append(f"added {key[0]}:{key[1]}")
        elif key not in current_records:
            changes.append(f"removed {key[0]}:{key[1]}")
        elif expected_records[key] != current_records[key]:
            changes.append(f"changed {key[0]}:{key[1]}")
    detail = "\n  ".join(changes[:12]) or "manifest fingerprint/record encoding changed"
    if len(changes) > 12:
        detail += f"\n  ... and {len(changes) - 12} more"
    raise FingerprintError(
        "native build fingerprint is stale or belongs to another build:\n"
        f"  manifest: {expected_fingerprint!r}\n"
        f"  current:  {current.fingerprint}\n"
        f"  {detail}\n"
        f"rebuild the native target in {current.build_dir} before linking Rust"
    )


def generate(args: argparse.Namespace) -> None:
    snapshot = compute_snapshot(args.source_root, args.build_dir)
    manifest = json.dumps(snapshot.manifest(), indent=2, sort_keys=True) + "\n"
    _atomic_write_if_changed(args.manifest, manifest.encode("utf-8"))
    _atomic_write_if_changed(args.cpp_out, render_cpp(snapshot).encode("utf-8"))
    if args.depfile is not None:
        write_depfile(args.depfile, args.cpp_out, snapshot.dependencies)
    print(snapshot.fingerprint)


def verify(args: argparse.Namespace) -> None:
    expected = load_manifest(args.manifest)
    snapshot = compute_snapshot(args.source_root, args.build_dir)
    compare_manifest(expected, snapshot)
    verify_archive_composition(args.source_root, args.build_dir)
    if args.rust_out is not None:
        _atomic_write_if_changed(args.rust_out, render_rust(snapshot).encode("utf-8"))
    if args.dependency_list is not None:
        dependencies = set(snapshot.dependencies)
        dependencies.add(args.manifest.resolve())
        write_dependency_list(args.dependency_list, dependencies)
    print(snapshot.fingerprint)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--source-root", type=Path, required=True)
    common.add_argument("--build-dir", type=Path, required=True)
    common.add_argument("--manifest", type=Path, required=True)

    generate_parser = subparsers.add_parser("generate", parents=[common])
    generate_parser.add_argument("--cpp-out", type=Path, required=True)
    generate_parser.add_argument("--depfile", type=Path)
    generate_parser.set_defaults(handler=generate)

    verify_parser = subparsers.add_parser("verify", parents=[common])
    verify_parser.add_argument("--rust-out", type=Path)
    verify_parser.add_argument("--dependency-list", type=Path)
    verify_parser.set_defaults(handler=verify)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.handler(args)
    except FingerprintError as error:
        parser.exit(2, f"mako-local fingerprint error: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
