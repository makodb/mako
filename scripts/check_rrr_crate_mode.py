#!/usr/bin/env python3
"""Check rusty-cpp crate output against the production rrr module ABI."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


DEFAULT_TRANSPILER = (
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
RUSTY_CPP_SUBMODULE = "third-party/rusty-cpp"
REQUIRED_RUSTY_CPP_COMMIT = "ba70b6ab6d8b38bfc5107ce963c6766d460b0e42"
EXTRACTION_DRIVER = "scripts/extract_rrr_rust.py"
MODULE_NAME = "rrr.internal_protocol"
EXPECTED_SYMBOLS = {
    ("R", "rrr::kInternalHeartbeatRpcId@rrr.internal_protocol"),
    ("R", "rrr::kResponseHeaderExtFlag@rrr.internal_protocol"),
    ("R", "rrr::kResponseSizeMask@rrr.internal_protocol"),
    (
        "T",
        "rrr::encode_response_size@rrr.internal_protocol(int, bool)",
    ),
    (
        "T",
        "rrr::response_has_extended_header@rrr.internal_protocol(int)",
    ),
    (
        "T",
        "rrr::response_payload_size@rrr.internal_protocol(int)",
    ),
}
NM_LINE = re.compile(r"^[0-9A-Fa-f]+\s+([A-Za-z])\s+(.+)$")
PLACEHOLDER = re.compile(r"\b(?:TODO|UNSUPPORTED|skipped)\b", re.IGNORECASE)


class GateError(RuntimeError):
    """A crate generation, compilation, import, or ABI-parity failure."""


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def executable(root: Path, value: str, description: str) -> Path:
    candidate = Path(value)
    if candidate.is_absolute() or "/" in value:
        resolved = candidate if candidate.is_absolute() else root / candidate
    else:
        found = shutil.which(value)
        if found is None:
            raise GateError(f"{description} is unavailable: {value}")
        resolved = Path(found)
    resolved = resolved.resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise GateError(f"{description} is unavailable: {resolved}")
    return resolved


def run(command: list[str], cwd: Path) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        rendered = " ".join(command)
        raise GateError(
            f"command failed with exit {completed.returncode}: {rendered}\n{diagnostic}"
        )
    return completed.stdout


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
        raise GateError(f"cannot inspect {description}: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        raise GateError(f"cannot inspect {description}: {diagnostic}")
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
        raise GateError(
            f"cannot read rusty-cpp transpiler build info: {exc}"
        ) from exc
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        raise GateError(
            "rusty-cpp transpiler --build-info failed with exit "
            f"{completed.returncode}: {diagnostic}"
        )
    lines = completed.stdout.splitlines()
    if len(lines) != 1:
        raise GateError(
            "rusty-cpp transpiler --build-info must emit exactly one JSON line"
        )
    try:
        build_info = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise GateError(
            f"rusty-cpp transpiler --build-info emitted invalid JSON: {exc}"
        ) from exc
    if not isinstance(build_info, dict) or set(build_info) != {
        "git_hash",
        "git_dirty",
    }:
        raise GateError(
            "rusty-cpp transpiler --build-info JSON keys must be exactly "
            "git_hash and git_dirty"
        )
    git_hash = build_info["git_hash"]
    git_dirty = build_info["git_dirty"]
    if git_hash != REQUIRED_RUSTY_CPP_COMMIT:
        raise GateError(
            "rusty-cpp transpiler build commit mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {git_hash!r}"
        )
    if git_dirty is not False:
        raise GateError("rusty-cpp transpiler build must report git_dirty=false")


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
        raise GateError(
            "rusty-cpp gitlink pin mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {actual}"
        )
    submodule = root / RUSTY_CPP_SUBMODULE
    head = git_output(submodule, ["rev-parse", "HEAD"], "rusty-cpp HEAD")
    if head != REQUIRED_RUSTY_CPP_COMMIT:
        raise GateError(
            "rusty-cpp submodule HEAD mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {head}"
        )
    dirty = git_output(
        submodule,
        ["status", "--porcelain", "--untracked-files=no"],
        "rusty-cpp worktree",
    )
    if dirty:
        raise GateError("rusty-cpp submodule has tracked local changes")
    verify_transpiler_build_info(root, transpiler)


def require_extraction_check(root: Path, transpiler: Path) -> None:
    run(
        [
            sys.executable,
            EXTRACTION_DRIVER,
            "--check",
            "--transpiler",
            str(transpiler),
        ],
        root,
    )


def read_generated(path: Path, description: str) -> str:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"missing generated {description} {path}: {exc}") from exc
    placeholder = PLACEHOLDER.search(text)
    if placeholder is not None:
        raise GateError(
            f"generated {description} contains placeholder marker "
            f"{placeholder.group(0)!r}: {path}"
        )
    return text


def require_cpp_surface(child_module: Path, root_module: Path) -> None:
    text = read_generated(child_module, "child module")
    required = {
        "export module rrr.internal_protocol;",
        "namespace rrr {",
        "export constexpr int32_t kInternalHeartbeatRpcId",
        "export constexpr uint32_t kResponseHeaderExtFlag",
        "export constexpr uint32_t kResponseSizeMask",
        "export bool response_has_extended_header(int32_t encoded_size);",
        "export int32_t response_payload_size(int32_t encoded_size);",
        "export int32_t encode_response_size(int32_t payload_size, bool extended_header);",
    }
    missing = sorted(fragment for fragment in required if fragment not in text)
    if missing:
        raise GateError("generated module is missing required surface:\n  " + "\n  ".join(missing))
    if "namespace rrr::internal_protocol" in text:
        raise GateError(
            "generated module drifted to nested namespace rrr::internal_protocol"
        )

    root_text = read_generated(root_module, "root module")
    root_required = {
        "export module rrr;",
        "export import rrr.internal_protocol;",
        "namespace rrr {",
    }
    root_missing = sorted(
        fragment for fragment in root_required if fragment not in root_text
    )
    if root_missing:
        raise GateError(
            "generated root module is missing required surface:\n  "
            + "\n  ".join(root_missing)
        )


def require_zero_hand_slots(path: Path) -> None:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"missing generated hand-slot manifest {path}: {exc}") from exc
    if not re.search(
        r"^0 slot\(s\) requiring hand-attention across 0 file\(s\)\.$",
        text,
        re.MULTILINE,
    ):
        raise GateError(f"generated crate does not report zero hand slots: {path}")


def module_symbols(nm: Path, root: Path, object_file: Path) -> set[tuple[str, str]]:
    output = run(
        [str(nm), "--defined-only", "--demangle", str(object_file)],
        root,
    )
    symbols: set[tuple[str, str]] = set()
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is not None and f"@{MODULE_NAME}" in match.group(2):
            symbols.add((match.group(1), match.group(2)))
    return symbols


def format_symbols(symbols: set[tuple[str, str]]) -> str:
    return "\n".join(f"  {kind} {name}" for kind, name in sorted(symbols))


def require_expected_symbols(label: str, symbols: set[tuple[str, str]]) -> None:
    if symbols == EXPECTED_SYMBOLS:
        return
    missing = EXPECTED_SYMBOLS - symbols
    unexpected = symbols - EXPECTED_SYMBOLS
    details = [f"{label} does not define the exact six-symbol rrr ABI"]
    if missing:
        details.append("missing:\n" + format_symbols(missing))
    if unexpected:
        details.append("unexpected:\n" + format_symbols(unexpected))
    raise GateError("\n".join(details))


def importer_source() -> str:
    return """\
import rrr.internal_protocol;

int main() {
    constexpr int kMin = (-2147483647 - 1);
    if (rrr::kInternalHeartbeatRpcId != (-2147483647 - 1)) {
        return 1;
    }
    if (rrr::kResponseHeaderExtFlag != 0x80000000u ||
        rrr::kResponseSizeMask != 0x7fffffffu) {
        return 2;
    }
    struct Row {
        int input;
        bool has_extended;
        int payload;
        int plain;
        int extended;
    };
    constexpr Row rows[] = {
        {0, false, 0, 0, kMin},
        {1, false, 1, 1, kMin + 1},
        {2147483647, false, 2147483647, 2147483647, -1},
        {kMin, true, 0, 0, kMin},
        {kMin + 1, true, 1, 1, kMin + 1},
        {-1, true, 2147483647, 2147483647, -1},
    };
    for (const auto& row : rows) {
        if (rrr::response_has_extended_header(row.input) != row.has_extended) {
            return 3;
        }
        if (rrr::response_payload_size(row.input) != row.payload) {
            return 4;
        }
        if (rrr::encode_response_size(row.input, false) != row.plain) {
            return 5;
        }
        if (rrr::encode_response_size(row.input, true) != row.extended) {
            return 6;
        }
    }
    return 0;
}
"""


def check(args: argparse.Namespace) -> None:
    root = repository_root()
    transpiler = executable(root, args.transpiler, "rusty-cpp transpiler")
    verify_pinned_toolchain(root, transpiler)
    require_extraction_check(root, transpiler)
    clang = executable(root, args.clang, "Clang C++ compiler")
    nm = executable(root, args.nm, "nm")
    reference = Path(args.reference_object)
    if not reference.is_absolute():
        reference = root / reference
    reference = reference.resolve()
    if not reference.is_file():
        raise GateError(f"production reference object is unavailable: {reference}")

    with tempfile.TemporaryDirectory(prefix="rrr-crate-mode-") as temporary:
        output = Path(temporary)
        run(
            [
                str(transpiler),
                "--crate",
                "src/rrr/Cargo.toml",
                "--output-dir",
                str(output),
                "--cxx-namespace",
                "rrr",
            ],
            root,
        )

        child_module = output / "rrr.internal_protocol.cppm"
        root_module = output / "rrr.cppm"
        require_cpp_surface(child_module, root_module)
        require_zero_hand_slots(output / "rusty_hand_slots.md")
        child_pcm = output / "rrr.internal_protocol.pcm"
        generated_object = output / "rrr.internal_protocol.o"
        root_pcm = output / "rrr.pcm"
        root_object = output / "rrr.o"
        importer = output / "importer.cpp"
        importer_object = output / "importer.o"
        generated_importer = output / "importer-generated"
        production_importer = output / "importer-production"
        importer.write_text(importer_source(), encoding="utf-8")
        include = root / "third-party/rusty-cpp/include"

        run(
            [
                str(clang),
                "-std=c++23",
                "-Wno-deprecated-declarations",
                "-I",
                str(include),
                "--precompile",
                str(child_module),
                "-o",
                str(child_pcm),
            ],
            root,
        )
        run(
            [
                str(clang),
                "-std=c++23",
                "-c",
                str(child_pcm),
                "-o",
                str(generated_object),
            ],
            root,
        )
        run(
            [
                str(clang),
                "-std=c++23",
                "-Wno-deprecated-declarations",
                "-I",
                str(include),
                f"-fprebuilt-module-path={output}",
                "--precompile",
                str(root_module),
                "-o",
                str(root_pcm),
            ],
            root,
        )
        run(
            [
                str(clang),
                "-std=c++23",
                f"-fprebuilt-module-path={output}",
                "-c",
                str(root_pcm),
                "-o",
                str(root_object),
            ],
            root,
        )
        run(
            [
                str(clang),
                "-std=c++23",
                f"-fprebuilt-module-path={output}",
                "-c",
                str(importer),
                "-o",
                str(importer_object),
            ],
            root,
        )
        run(
            [
                str(clang),
                "-std=c++23",
                str(importer_object),
                str(generated_object),
                "-o",
                str(generated_importer),
            ],
            root,
        )
        run([str(generated_importer)], root)
        run(
            [
                str(clang),
                "-std=c++23",
                str(importer_object),
                str(reference),
                "-o",
                str(production_importer),
            ],
            root,
        )
        run([str(production_importer)], root)

        reference_symbols = module_symbols(nm, root, reference)
        generated_symbols = module_symbols(nm, root, generated_object)
        require_expected_symbols("production reference object", reference_symbols)
        require_expected_symbols("crate-generated object", generated_symbols)
        if generated_symbols != reference_symbols:
            raise GateError("crate-generated ABI differs from production reference ABI")

    print(
        "checked whole rrr crate (2 modules, 0 hand slots), importer against "
        "both objects, and six-symbol ABI"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-object", required=True)
    parser.add_argument(
        "--transpiler",
        default=os.environ.get("RUSTY_CPP_TRANSPILER", DEFAULT_TRANSPILER),
    )
    parser.add_argument("--clang", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--nm", default=os.environ.get("NM", "nm"))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        check(parse_args(argv))
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
