#!/usr/bin/env python3
"""Check rusty-cpp crate output against the production rrr module ABI."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import extract_rrr_rust as extraction


DEFAULT_TRANSPILER = (
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
RUSTY_CPP_SUBMODULE = "third-party/rusty-cpp"
REQUIRED_RUSTY_CPP_COMMIT = "ba70b6ab6d8b38bfc5107ce963c6766d460b0e42"
EXTRACTION_DRIVER = "scripts/extract_rrr_rust.py"
EXTRACTION_MANIFEST = "src/rrr/rust-extraction.toml"
NM_LINE = re.compile(r"^[0-9A-Fa-f]+\s+([A-Za-z])\s+(.+)$")
PLACEHOLDER = re.compile(r"\b(?:TODO|UNSUPPORTED|skipped)\b", re.IGNORECASE)


@dataclass(frozen=True)
class AbiSpec:
    """Checked C++ surface and exact symbols for one extracted module."""

    surface: frozenset[str]
    symbols: frozenset[tuple[str, str]]


ABI_SPECS = {
    "rrr.internal_protocol": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.internal_protocol;",
                "namespace rrr {",
                "export constexpr int32_t kInternalHeartbeatRpcId",
                "export constexpr uint32_t kResponseHeaderExtFlag",
                "export constexpr uint32_t kResponseSizeMask",
                "export bool response_has_extended_header(int32_t encoded_size);",
                "export int32_t response_payload_size(int32_t encoded_size);",
                "export int32_t encode_response_size(int32_t payload_size, bool extended_header);",
            }
        ),
        symbols=frozenset(
            {
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
        ),
    ),
    "rrr.stat": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.stat;",
                "namespace rrr {",
                "export struct AvgStat",
                "int64_t n_stat_;",
                "int64_t sum_;",
                "int64_t avg_;",
                "int64_t max_;",
                "int64_t min_;",
                "static AvgStat new_();",
                "void sample(int64_t s);",
                "void clear();",
                "AvgStat reset();",
                "AvgStat peek() const;",
                "int64_t avg() const;",
            }
        ),
        symbols=frozenset(
            {
                ("T", "rrr::AvgStat@rrr.stat::new_()"),
                ("T", "rrr::AvgStat@rrr.stat::sample(long)"),
                ("T", "rrr::AvgStat@rrr.stat::clear()"),
                ("T", "rrr::AvgStat@rrr.stat::reset()"),
                ("T", "rrr::AvgStat@rrr.stat::peek() const"),
                ("T", "rrr::AvgStat@rrr.stat::avg() const"),
            }
        ),
    ),
}


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
    if build_info["git_hash"] != REQUIRED_RUSTY_CPP_COMMIT:
        raise GateError(
            "rusty-cpp transpiler build commit mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, "
            f"got {build_info['git_hash']!r}"
        )
    if build_info["git_dirty"] is not False:
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


def load_owned_modules(root: Path) -> list[extraction.ModuleEntry]:
    try:
        modules = extraction.load_manifest(root, root / EXTRACTION_MANIFEST)
    except extraction.ExtractionError as exc:
        raise GateError(f"cannot load extraction ownership: {exc}") from exc
    actual = {module.cpp_module for module in modules}
    expected = set(ABI_SPECS)
    if actual != expected:
        details = ["crate-mode ABI ratchet does not match extraction manifest"]
        if expected - actual:
            details.append(
                "missing manifest module(s): " + ", ".join(sorted(expected - actual))
            )
        if actual - expected:
            details.append(
                "missing ABI specification(s): " + ", ".join(sorted(actual - expected))
            )
        raise GateError("\n".join(details))
    return modules


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


def require_cpp_surfaces(
    output: Path, modules: list[extraction.ModuleEntry]
) -> None:
    expected_files = {f"{module.cpp_module}.cppm" for module in modules}
    expected_files.add("rrr.cppm")
    actual_files = {path.name for path in output.glob("*.cppm") if path.is_file()}
    if actual_files != expected_files:
        raise GateError(
            "generated C++ module census mismatch: expected "
            f"{sorted(expected_files)!r}, got {sorted(actual_files)!r}"
        )

    for module in modules:
        path = output / f"{module.cpp_module}.cppm"
        text = read_generated(path, f"child module {module.cpp_module}")
        missing = sorted(
            fragment
            for fragment in ABI_SPECS[module.cpp_module].surface
            if fragment not in text
        )
        if missing:
            raise GateError(
                f"generated module {module.cpp_module} is missing required surface:\n  "
                + "\n  ".join(missing)
            )
        if "namespace rrr::" in text:
            raise GateError(
                f"generated module {module.cpp_module} drifted to a nested namespace"
            )

    root_text = read_generated(output / "rrr.cppm", "root module")
    root_required = {
        "export module rrr;",
        "namespace rrr {",
        *(f"export import {module.cpp_module};" for module in modules),
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


def module_symbols(
    nm: Path,
    root: Path,
    binary: Path,
    module_name: str,
) -> set[tuple[str, str]]:
    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    symbols: set[tuple[str, str]] = set()
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is not None and f"@{module_name}" in match.group(2):
            symbols.add((match.group(1), match.group(2)))
    return symbols


def format_symbols(symbols: set[tuple[str, str]]) -> str:
    return "\n".join(f"  {kind} {name}" for kind, name in sorted(symbols))


def require_expected_symbols(
    module_name: str,
    label: str,
    symbols: set[tuple[str, str]],
) -> None:
    expected = set(ABI_SPECS[module_name].symbols)
    if symbols == expected:
        return
    missing = expected - symbols
    unexpected = symbols - expected
    details = [
        f"{label} does not define the exact {len(expected)}-symbol "
        f"{module_name} ABI"
    ]
    if missing:
        details.append("missing:\n" + format_symbols(missing))
    if unexpected:
        details.append("unexpected:\n" + format_symbols(unexpected))
    raise GateError("\n".join(details))


def importer_source() -> str:
    return """\
#include <cstddef>
#include <cstdint>
#include <type_traits>

import rrr.internal_protocol;
import rrr.stat;

static_assert(std::is_standard_layout_v<rrr::AvgStat>);
static_assert(std::is_trivially_copyable_v<rrr::AvgStat>);
static_assert(sizeof(rrr::AvgStat) == 5 * sizeof(std::int64_t));
static_assert(alignof(rrr::AvgStat) == alignof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, n_stat_) == 0 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, sum_) == 1 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, avg_) == 2 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, max_) == 3 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, min_) == 4 * sizeof(std::int64_t));

static bool stat_is(
    const rrr::AvgStat& stat,
    std::int64_t count,
    std::int64_t sum,
    std::int64_t average,
    std::int64_t maximum,
    std::int64_t minimum) {
    return stat.n_stat_ == count && stat.sum_ == sum &&
           stat.avg_ == average && stat.max_ == maximum &&
           stat.min_ == minimum;
}

int main() {
    constexpr int kMin = (-2147483647 - 1);
    if (rrr::kInternalHeartbeatRpcId != kMin) {
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

    auto stat = rrr::AvgStat::new_();
    if (!stat_is(stat, 0, 0, 0, 0, 0) || stat.avg() != 0) {
        return 10;
    }
    stat.sample(3);
    stat.sample(-5);
    stat.sample(8);
    if (!stat_is(stat, 3, 6, 2, 8, -5) || stat.avg() != 2) {
        return 11;
    }
    const auto peeked = stat.peek();
    if (!stat_is(peeked, 3, 6, 2, 8, -5) ||
        !stat_is(stat, 3, 6, 2, 8, -5)) {
        return 12;
    }
    const auto reset = stat.reset();
    if (!stat_is(reset, 3, 6, 2, 8, -5) ||
        !stat_is(stat, 0, 0, 0, 0, 0)) {
        return 13;
    }
    stat.sample(-7);
    stat.sample(-2);
    if (!stat_is(stat, 2, -9, -4, 0, -7) || stat.avg() != -4) {
        return 14;
    }
    stat.clear();
    if (!stat_is(stat, 0, 0, 0, 0, 0)) {
        return 15;
    }
    return 0;
}
"""


def compile_module(
    clang: Path,
    root: Path,
    include: Path,
    output: Path,
    module_name: str,
) -> Path:
    source = output / f"{module_name}.cppm"
    pcm = output / f"{module_name}.pcm"
    object_file = output / f"{module_name}.o"
    run(
        [
            str(clang),
            "-std=c++23",
            "-Wno-deprecated-declarations",
            "-I",
            str(include),
            f"-fprebuilt-module-path={output}",
            "--precompile",
            str(source),
            "-o",
            str(pcm),
        ],
        root,
    )
    run(
        [
            str(clang),
            "-std=c++23",
            f"-fprebuilt-module-path={output}",
            "-c",
            str(pcm),
            "-o",
            str(object_file),
        ],
        root,
    )
    return object_file


def check(args: argparse.Namespace) -> None:
    root = repository_root()
    transpiler = executable(root, args.transpiler, "rusty-cpp transpiler")
    verify_pinned_toolchain(root, transpiler)
    require_extraction_check(root, transpiler)
    modules = load_owned_modules(root)
    clang = executable(root, args.clang, "Clang C++ compiler")
    nm = executable(root, args.nm, "nm")
    reference = Path(args.reference_library)
    if not reference.is_absolute():
        reference = root / reference
    reference = reference.resolve()
    if not reference.is_file():
        raise GateError(f"production reference library is unavailable: {reference}")

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

        require_cpp_surfaces(output, modules)
        require_zero_hand_slots(output / "rusty_hand_slots.md")
        include = root / "third-party/rusty-cpp/include"
        generated_objects = [
            compile_module(
                clang,
                root,
                include,
                output,
                module.cpp_module,
            )
            for module in modules
        ]
        compile_module(clang, root, include, output, "rrr")

        importer = output / "importer.cpp"
        importer_object = output / "importer.o"
        generated_importer = output / "importer-generated"
        production_importer = output / "importer-production"
        importer.write_text(importer_source(), encoding="utf-8")
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
                *(str(path) for path in generated_objects),
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

        for module, generated_object in zip(
            modules, generated_objects, strict=True
        ):
            reference_symbols = module_symbols(
                nm, root, reference, module.cpp_module
            )
            generated_symbols = module_symbols(
                nm, root, generated_object, module.cpp_module
            )
            require_expected_symbols(
                module.cpp_module,
                "production reference library",
                reference_symbols,
            )
            require_expected_symbols(
                module.cpp_module,
                "crate-generated object",
                generated_symbols,
            )
            if generated_symbols != reference_symbols:
                raise GateError(
                    f"crate-generated {module.cpp_module} ABI differs from "
                    "production reference ABI"
                )

    symbol_count = sum(len(spec.symbols) for spec in ABI_SPECS.values())
    print(
        f"checked whole rrr crate ({len(modules) + 1} modules, 0 hand slots), "
        "combined importer against generated objects and production library, "
        f"AvgStat layout/runtime, and {symbol_count} exact ABI symbols"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--reference-library",
        "--reference-object",
        dest="reference_library",
        required=True,
        help="production librrr archive (legacy --reference-object is accepted)",
    )
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
