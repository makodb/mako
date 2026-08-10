#!/usr/bin/env python3
"""Check rusty-cpp crate output against independent and production rrr ABIs."""

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
REQUIRED_RUSTY_CPP_COMMIT = "707650e4021b163ea37783c14c7a182eef8a9a63"
EXTRACTION_DRIVER = "scripts/extract_rrr_rust.py"
EXTRACTION_MANIFEST = "src/rrr/rust-extraction.toml"
MODULE_PREAMBLE = "src/rrr/module-preambles.toml"
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
    "rrr.errors": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.errors;",
                "namespace rrr {",
                "export enum class RpcErrorCategory",
                "export enum class RpcError",
                "export std::string_view rpc_error_category_to_string(RpcErrorCategory cat);",
                "export std::string_view rpc_error_to_string(RpcError err);",
                "export RpcErrorCategory get_error_category(RpcError err);",
                "export bool is_connection_error(RpcError err);",
                "export bool is_timeout_error(RpcError err);",
                "export bool is_retryable_error(RpcError err);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "rrr::get_error_category@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::is_connection_error@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::is_retryable_error@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::is_timeout_error@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::rpc_error_category_to_string@rrr.errors(rrr::RpcErrorCategory@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::rpc_error_to_string@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
            }
        ),
    ),
    "rrr.connection_metrics": AbiSpec(
        surface=frozenset(
            {
                "#include <rusty/sync/atomic.hpp>",
                "export module rrr.connection_metrics;",
                "namespace rrr {",
                "export struct ConnectionMetrics",
                "using rusty::sync::atomic::AtomicU64;",
                "using rusty::sync::atomic::Ordering;",
                "rusty::sync::atomic::AtomicU64 requests_sent_field;",
                "rusty::sync::atomic::AtomicU64 requests_completed_field;",
                "rusty::sync::atomic::AtomicU64 requests_failed_field;",
                "rusty::sync::atomic::AtomicU64 requests_timed_out_field;",
                "rusty::sync::atomic::AtomicU64 in_flight_requests_field;",
                "rusty::sync::atomic::AtomicU64 bytes_sent_field;",
                "rusty::sync::atomic::AtomicU64 bytes_received_field;",
                "rusty::sync::atomic::AtomicU64 reconnect_count_field;",
                "rusty::sync::atomic::AtomicU64 retry_attempts_field;",
                "rusty::sync::atomic::AtomicU64 queue_dropped_requests_field;",
                "rusty::sync::atomic::AtomicU64 circuit_open_rejections_field;",
                "rusty::sync::atomic::AtomicU64 circuit_open_transitions_field;",
                "rusty::sync::atomic::AtomicU64 circuit_half_open_transitions_field;",
                "rusty::sync::atomic::AtomicU64 circuit_closed_transitions_field;",
                "rusty::sync::atomic::AtomicU64 connect_time_ms_field;",
                "rusty::sync::atomic::AtomicU64 total_latency_us_field;",
                "rusty::sync::atomic::AtomicU64 min_latency_us_field;",
                "rusty::sync::atomic::AtomicU64 max_latency_us_field;",
                "static ConnectionMetrics new_();",
                "uint64_t requests_sent() const;",
                "uint64_t requests_completed() const;",
                "uint64_t requests_failed() const;",
                "uint64_t requests_timed_out() const;",
                "uint64_t in_flight_requests() const;",
                "uint64_t bytes_sent() const;",
                "uint64_t bytes_received() const;",
                "uint64_t reconnect_count() const;",
                "uint64_t retry_attempts() const;",
                "uint64_t queue_dropped_requests() const;",
                "uint64_t circuit_open_rejections() const;",
                "uint64_t circuit_open_transitions() const;",
                "uint64_t circuit_half_open_transitions() const;",
                "uint64_t circuit_closed_transitions() const;",
                "uint64_t connect_time_ms() const;",
                "uint64_t min_latency_us() const;",
                "uint64_t max_latency_us() const;",
                "uint64_t success_rate_percent() const;",
                "uint64_t avg_latency_us() const;",
                "uint64_t uptime_ms(uint64_t current_time_ms) const;",
                "void record_request_sent() const;",
                "void record_request_completed_with_latency(uint64_t latency_us) const;",
                "void record_request_completed() const;",
                "void record_request_failed() const;",
                "void record_request_timeout() const;",
                "void record_request_dropped() const;",
                "void record_bytes_sent(uint64_t bytes) const;",
                "void record_bytes_received(uint64_t bytes) const;",
                "void record_reconnect() const;",
                "void record_retry_attempt() const;",
                "void record_queue_drop() const;",
                "void record_circuit_open_rejection() const;",
                "void record_circuit_open_transition() const;",
                "void record_circuit_half_open_transition() const;",
                "void record_circuit_closed_transition() const;",
                "void record_connect(uint64_t current_time_ms) const;",
                "void reset() const;",
                "void decrement_in_flight() const;",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "rrr::ConnectionMetrics@rrr.connection_metrics::new_()",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_sent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_completed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_failed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_timed_out() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::in_flight_requests() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::bytes_sent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::bytes_received() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::reconnect_count() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::retry_attempts() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::queue_dropped_requests() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_open_rejections() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_open_transitions() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_half_open_transitions() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_closed_transitions() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::connect_time_ms() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::min_latency_us() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::max_latency_us() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::success_rate_percent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::avg_latency_us() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::uptime_ms(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_sent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_completed_with_latency(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_completed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_failed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_timeout() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_dropped() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_bytes_sent(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_bytes_received(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_reconnect() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_retry_attempt() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_queue_drop() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_open_rejection() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_open_transition() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_half_open_transition() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_closed_transition() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_connect(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::reset() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::decrement_in_flight() const",
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
    # Preserve the invoked basename. Clang selects C++ driver behavior from
    # argv[0], and resolving a `clang++ -> clang-N` symlink silently drops the
    # implicit C++ standard-library link in the direct gate commands.
    resolved = Path(os.path.abspath(resolved))
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


def inline_generated_block(source: str, block_id: str) -> str:
    begin = f"/*RUSTYCPP:GEN-BEGIN id={block_id} "
    end = f"/*RUSTYCPP:GEN-END id={block_id}*/"
    try:
        payload_start = source.index("\n", source.index(begin)) + 1
        payload_end = source.index(end, payload_start)
    except ValueError as exc:
        raise GateError(
            f"cannot locate inline generated block {block_id}"
        ) from exc
    return source[payload_start:payload_end].strip()


def require_connection_metrics_text_parity(root: Path, generated: str) -> None:
    source_path = root / "src/rrr/rpc/connection_metrics.cpp"
    try:
        inline = source_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(
            f"cannot read inline connection-metrics oracle {source_path}: {exc}"
        ) from exc

    inline_usings = inline_generated_block(
        inline, "connection_metrics.usings"
    )
    using_start = generated.find(
        "using rusty::sync::atomic::AtomicU64;"
    )
    definition_start = generated.find("export struct ConnectionMetrics {")
    if using_start < 0 or definition_start < 0:
        raise GateError(
            "cannot locate generated connection-metrics using/declaration surface"
        )
    generated_usings = generated[using_start:definition_start].strip()
    if generated_usings != inline_usings:
        raise GateError(
            "crate-generated connection-metrics using declarations differ "
            "from the inline provider"
        )

    inline_payload = inline_generated_block(inline, "connection_metrics.1")
    body_token = "ConnectionMetrics ConnectionMetrics::new_()"
    try:
        inline_body_start = inline_payload.index(body_token)
        generated_body_start = generated.index(body_token, definition_start)
        generated_body_end = generated.rindex("\n} // namespace rrr")
    except ValueError as exc:
        raise GateError(
            "cannot locate the full connection-metrics declaration/body surface"
        ) from exc

    inline_declarations = inline_payload[:inline_body_start].strip()
    inline_bodies = inline_payload[inline_body_start:].strip()
    generated_forward = "struct ConnectionMetrics;"
    generated_definition = generated[
        definition_start:generated_body_start
    ].strip().replace(
        "export struct ConnectionMetrics {",
        "struct ConnectionMetrics {",
        1,
    )
    generated_declarations = (
        generated_forward + "\n\n" + generated_definition
    )
    generated_bodies = generated[
        generated_body_start:generated_body_end
    ].strip()
    if generated_declarations != inline_declarations:
        raise GateError(
            "crate-generated connection-metrics declarations differ from "
            "the inline provider"
        )
    if generated_bodies != inline_bodies:
        raise GateError(
            "crate-generated connection-metrics method bodies differ from "
            "the inline provider"
        )


def require_cpp_surfaces(
    root: Path, output: Path, modules: list[extraction.ModuleEntry]
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
        atomic_preamble = "#include <rusty/sync/atomic.hpp>"
        if module.cpp_module == "rrr.connection_metrics":
            if text.count(atomic_preamble) != 1:
                raise GateError(
                    "generated rrr.connection_metrics must contain exactly one "
                    "structured atomic preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(atomic_preamble),
                text.find("#include <cstdint>"),
                text.find("export module rrr.connection_metrics;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated rrr.connection_metrics atomic preamble is not "
                    "between the global module fragment and standard includes"
                )
            require_connection_metrics_text_parity(root, text)
        elif atomic_preamble in text:
            raise GateError(
                f"connection-metrics preamble leaked into {module.cpp_module}"
            )

    root_text = read_generated(output / "rrr.cppm", "root module")
    if "#include <rusty/sync/atomic.hpp>" in root_text:
        raise GateError("connection-metrics preamble leaked into the crate root")
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
        if match is None:
            continue
        kind, symbol = match.groups()
        # Template instantiations and lambda helpers are optimization-sensitive
        # weak implementation details, not the strong module ABI ratcheted here.
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == module_name:
            symbols.add((kind, symbol))
    return symbols


def function_parameter_open(symbol: str) -> int:
    """Return the outer function-parameter `(`, or the end for a data symbol."""

    close = symbol.rfind(")")
    if close == -1:
        return len(symbol)
    depth = 0
    for index in range(close, -1, -1):
        character = symbol[index]
        if character == ")":
            depth += 1
        elif character == "(":
            depth -= 1
            if depth == 0:
                return index
    return len(symbol)


def is_operator_angle(text: str, index: int) -> bool:
    """Return whether `<` at index spells a C++ operator rather than a template."""

    operator = text.rfind("operator", 0, index + 1)
    if operator == -1:
        return False
    candidate = "".join(text[operator : index + 1].split())
    return any(
        spelling.startswith(candidate)
        for spelling in (
            "operator<",
            "operator<=",
            "operator<=>",
            "operator<<",
            "operator<<=",
        )
    )


def actual_entity_declarator(symbol: str) -> str:
    """Remove parameter and optional return-type text from a demangled symbol."""

    prefix = symbol[: function_parameter_open(symbol)].rstrip()
    angle_depth = 0
    last_separator = -1
    for index, character in enumerate(prefix):
        if character == "<" and not is_operator_angle(prefix, index):
            angle_depth += 1
        elif character == ">" and angle_depth > 0:
            angle_depth -= 1
        elif character.isspace() and angle_depth == 0:
            last_separator = index
    return prefix[last_separator + 1 :]


def top_level_module_attachment(declarator: str) -> str | None:
    """Return the last module attachment outside template arguments."""

    angle_depth = 0
    owner: str | None = None
    for index, character in enumerate(declarator):
        if character == "<" and not is_operator_angle(declarator, index):
            angle_depth += 1
        elif character == ">" and angle_depth > 0:
            angle_depth -= 1
        elif character == "@" and angle_depth == 0:
            end = index + 1
            while end < len(declarator) and (
                declarator[end].isalnum() or declarator[end] in "._"
            ):
                end += 1
            module = declarator[index + 1 : end]
            if module and (
                end == len(declarator) or declarator[end] in "<:"
            ):
                owner = module
    return owner


def symbol_owner_module(symbol: str) -> str | None:
    """Return the module attached to the symbol's actual declared entity."""

    prefix = symbol[: function_parameter_open(symbol)].rstrip()
    qualified_operator = prefix.rfind("::operator")
    if qualified_operator != -1:
        # Conversion-operator target types may carry their own attachments.
        # A module attached to the qualified class owns the member operator;
        # namespace-qualified free operators instead fall through to the
        # attachment on the operator name itself.
        qualified_entity = actual_entity_declarator(
            prefix[:qualified_operator]
        )
        owner = top_level_module_attachment(qualified_entity)
        if owner is not None:
            return owner

    return top_level_module_attachment(actual_entity_declarator(symbol))


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
#include <rusty/sync/atomic.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

import rrr.connection_metrics;
import rrr.errors;
import rrr.internal_protocol;
import rrr.stat;

static_assert(sizeof(rrr::RpcErrorCategory) == sizeof(std::int32_t));
static_assert(sizeof(rrr::RpcError) == sizeof(std::int32_t));
static_assert(std::is_same_v<
              std::underlying_type_t<rrr::RpcErrorCategory>, std::int32_t>);
static_assert(std::is_same_v<
              std::underlying_type_t<rrr::RpcError>, std::int32_t>);
static_assert(std::is_trivially_copyable_v<rrr::RpcErrorCategory>);
static_assert(std::is_trivially_copyable_v<rrr::RpcError>);
static_assert(std::is_standard_layout_v<rrr::AvgStat>);
static_assert(std::is_trivially_copyable_v<rrr::AvgStat>);
static_assert(sizeof(rrr::AvgStat) == 5 * sizeof(std::int64_t));
static_assert(alignof(rrr::AvgStat) == alignof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, n_stat_) == 0 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, sum_) == 1 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, avg_) == 2 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, max_) == 3 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, min_) == 4 * sizeof(std::int64_t));

using MetricsAtomicU64 = rusty::sync::atomic::AtomicU64;
static_assert(sizeof(MetricsAtomicU64) == sizeof(std::uint64_t));
static_assert(alignof(MetricsAtomicU64) == alignof(std::uint64_t));
static_assert(std::is_standard_layout_v<rrr::ConnectionMetrics>);
static_assert(std::is_copy_constructible_v<rrr::ConnectionMetrics>);
static_assert(std::is_copy_assignable_v<rrr::ConnectionMetrics>);
static_assert(std::is_move_constructible_v<rrr::ConnectionMetrics>);
static_assert(std::is_move_assignable_v<rrr::ConnectionMetrics>);
static_assert(!std::is_trivially_copyable_v<rrr::ConnectionMetrics>);
static_assert(rrr::ConnectionMetrics::is_send);
static_assert(rrr::ConnectionMetrics::is_sync);
static_assert(
    sizeof(rrr::ConnectionMetrics) == 18 * sizeof(std::uint64_t));
static_assert(
    alignof(rrr::ConnectionMetrics) == alignof(std::uint64_t));
static_assert(offsetof(rrr::ConnectionMetrics, requests_sent_field) ==
              0 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, requests_completed_field) ==
              1 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, requests_failed_field) ==
              2 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, requests_timed_out_field) ==
              3 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, in_flight_requests_field) ==
              4 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, bytes_sent_field) ==
              5 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, bytes_received_field) ==
              6 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, reconnect_count_field) ==
              7 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, retry_attempts_field) ==
              8 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, queue_dropped_requests_field) ==
              9 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_open_rejections_field) ==
              10 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_open_transitions_field) ==
              11 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_half_open_transitions_field) ==
              12 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_closed_transitions_field) ==
              13 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, connect_time_ms_field) ==
              14 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, total_latency_us_field) ==
              15 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, min_latency_us_field) ==
              16 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, max_latency_us_field) ==
              17 * sizeof(MetricsAtomicU64));

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

static bool metrics_are_reset(const rrr::ConnectionMetrics& metrics) {
    using rusty::sync::atomic::Ordering;
    return metrics.requests_sent() == 0 &&
           metrics.requests_completed() == 0 &&
           metrics.requests_failed() == 0 &&
           metrics.requests_timed_out() == 0 &&
           metrics.in_flight_requests() == 0 &&
           metrics.bytes_sent() == 0 &&
           metrics.bytes_received() == 0 &&
           metrics.reconnect_count() == 0 &&
           metrics.retry_attempts() == 0 &&
           metrics.queue_dropped_requests() == 0 &&
           metrics.circuit_open_rejections() == 0 &&
           metrics.circuit_open_transitions() == 0 &&
           metrics.circuit_half_open_transitions() == 0 &&
           metrics.circuit_closed_transitions() == 0 &&
           metrics.connect_time_ms() == 0 &&
           metrics.total_latency_us_field.load(Ordering::Relaxed) == 0 &&
           metrics.min_latency_us_field.load(Ordering::Relaxed) ==
               std::numeric_limits<std::uint64_t>::max() &&
           metrics.min_latency_us() == 0 &&
           metrics.max_latency_us() == 0 &&
           metrics.avg_latency_us() == 0 &&
           metrics.success_rate_percent() == 100;
}

static bool metrics_concurrent_updates_are_atomic() {
    constexpr std::uint64_t kThreads = 8;
    constexpr std::uint64_t kOpsPerThread = 2000;
    constexpr std::uint64_t kRounds = 3;
    constexpr std::uint64_t kUpdates = kThreads * kOpsPerThread;
    constexpr std::uint64_t kLatencyTotal =
        kOpsPerThread * kThreads * (kThreads + 1) / 2;

    for (std::uint64_t round = 0; round < kRounds; ++round) {
        auto metrics = rrr::ConnectionMetrics::new_();
        std::atomic<std::uint64_t> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (std::uint64_t thread_index = 0;
             thread_index < kThreads;
             ++thread_index) {
            workers.emplace_back([&, latency = thread_index + 1] {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::uint64_t operation = 0;
                     operation < kOpsPerThread;
                     ++operation) {
                    metrics.record_request_sent();
                    metrics.record_request_completed_with_latency(latency);
                    metrics.record_request_sent();
                    metrics.record_request_failed();
                    metrics.record_request_sent();
                    metrics.record_request_timeout();
                    metrics.record_request_sent();
                    metrics.record_request_dropped();
                    metrics.record_bytes_sent(3);
                    metrics.record_bytes_received(5);
                    metrics.record_reconnect();
                    metrics.record_retry_attempt();
                    metrics.record_queue_drop();
                    metrics.record_circuit_open_rejection();
                    metrics.record_circuit_open_transition();
                    metrics.record_circuit_half_open_transition();
                    metrics.record_circuit_closed_transition();
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kThreads) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }

        if (metrics.requests_sent() != 4 * kUpdates ||
            metrics.requests_completed() != kUpdates ||
            metrics.requests_failed() != kUpdates ||
            metrics.requests_timed_out() != kUpdates ||
            metrics.in_flight_requests() != 0 ||
            metrics.bytes_sent() != 3 * kUpdates ||
            metrics.bytes_received() != 5 * kUpdates ||
            metrics.reconnect_count() != kUpdates ||
            metrics.retry_attempts() != kUpdates ||
            metrics.queue_dropped_requests() != kUpdates ||
            metrics.circuit_open_rejections() != kUpdates ||
            metrics.circuit_open_transitions() != kUpdates ||
            metrics.circuit_half_open_transitions() != kUpdates ||
            metrics.circuit_closed_transitions() != kUpdates ||
            metrics.total_latency_us_field.load(
                rusty::sync::atomic::Ordering::Relaxed) != kLatencyTotal ||
            metrics.min_latency_us() != 1 ||
            metrics.max_latency_us() != kThreads ||
            metrics.avg_latency_us() != kLatencyTotal / kUpdates ||
            metrics.success_rate_percent() != 25) {
            return false;
        }
        for (std::uint64_t extra = 0; extra < kThreads; ++extra) {
            metrics.record_request_dropped();
        }
        if (metrics.in_flight_requests() != 0) {
            return false;
        }
    }
    return true;
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

    struct CategoryRow {
        rrr::RpcErrorCategory category;
        int discriminant;
        std::string_view name;
    };
    constexpr CategoryRow categories[] = {
        {rrr::RpcErrorCategory::NONE, 0, "NONE"},
        {rrr::RpcErrorCategory::CONNECTION, 1, "CONNECTION"},
        {rrr::RpcErrorCategory::PROTOCOL, 2, "PROTOCOL"},
        {rrr::RpcErrorCategory::APPLICATION, 3, "APPLICATION"},
        {rrr::RpcErrorCategory::TIMEOUT, 4, "TIMEOUT"},
        {rrr::RpcErrorCategory::INTERNAL, 5, "INTERNAL"},
    };
    for (const auto& row : categories) {
        if (static_cast<int>(row.category) != row.discriminant ||
            rrr::rpc_error_category_to_string(row.category) != row.name) {
            return 20;
        }
    }
    constexpr int invalid_categories[] = {-1, 6, 999};
    for (const auto value : invalid_categories) {
        if (rrr::rpc_error_category_to_string(
                static_cast<rrr::RpcErrorCategory>(value)) != "UNKNOWN") {
            return 21;
        }
    }

    struct ErrorRow {
        rrr::RpcError error;
        int discriminant;
        std::string_view name;
        rrr::RpcErrorCategory category;
        bool retryable;
    };
    constexpr ErrorRow errors[] = {
        {rrr::RpcError::OK, 0, "OK", rrr::RpcErrorCategory::NONE, false},
        {rrr::RpcError::NOT_CONNECTED, 100, "NOT_CONNECTED", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::CONNECTION_REFUSED, 101, "CONNECTION_REFUSED", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::CONNECTION_RESET, 102, "CONNECTION_RESET", rrr::RpcErrorCategory::CONNECTION, true},
        {rrr::RpcError::NETWORK_UNREACHABLE, 103, "NETWORK_UNREACHABLE", rrr::RpcErrorCategory::CONNECTION, true},
        {rrr::RpcError::HOST_UNREACHABLE, 104, "HOST_UNREACHABLE", rrr::RpcErrorCategory::CONNECTION, true},
        {rrr::RpcError::CONNECTION_CLOSED, 105, "CONNECTION_CLOSED", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::CIRCUIT_OPEN, 106, "CIRCUIT_OPEN", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::INVALID_MESSAGE, 200, "INVALID_MESSAGE", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::UNKNOWN_RPC_ID, 201, "UNKNOWN_RPC_ID", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::MARSHALLING_ERROR, 202, "MARSHALLING_ERROR", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::VERSION_MISMATCH, 203, "VERSION_MISMATCH", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::CHECKSUM_ERROR, 204, "CHECKSUM_ERROR", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::RPC_FAILED, 300, "RPC_FAILED", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::SERVICE_UNAVAILABLE, 301, "SERVICE_UNAVAILABLE", rrr::RpcErrorCategory::APPLICATION, true},
        {rrr::RpcError::PERMISSION_DENIED, 302, "PERMISSION_DENIED", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::INVALID_ARGUMENT, 303, "INVALID_ARGUMENT", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::NOT_FOUND, 304, "NOT_FOUND", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::ALREADY_EXISTS, 305, "ALREADY_EXISTS", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::CONNECT_TIMEOUT, 400, "CONNECT_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, true},
        {rrr::RpcError::REQUEST_TIMEOUT, 401, "REQUEST_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, true},
        {rrr::RpcError::RESPONSE_TIMEOUT, 402, "RESPONSE_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, true},
        {rrr::RpcError::IDLE_TIMEOUT, 403, "IDLE_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, false},
        {rrr::RpcError::HEARTBEAT_TIMEOUT, 404, "HEARTBEAT_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, false},
        {rrr::RpcError::UNKNOWN_ERROR, 500, "UNKNOWN_ERROR", rrr::RpcErrorCategory::INTERNAL, false},
        {rrr::RpcError::OUT_OF_MEMORY, 501, "OUT_OF_MEMORY", rrr::RpcErrorCategory::INTERNAL, false},
        {rrr::RpcError::INVALID_STATE, 502, "INVALID_STATE", rrr::RpcErrorCategory::INTERNAL, false},
        {rrr::RpcError::INTERNAL_ERROR, 503, "INTERNAL_ERROR", rrr::RpcErrorCategory::INTERNAL, false},
    };
    for (const auto& row : errors) {
        if (static_cast<int>(row.error) != row.discriminant ||
            rrr::rpc_error_to_string(row.error) != row.name ||
            rrr::get_error_category(row.error) != row.category ||
            rrr::is_connection_error(row.error) !=
                (row.category == rrr::RpcErrorCategory::CONNECTION) ||
            rrr::is_timeout_error(row.error) !=
                (row.category == rrr::RpcErrorCategory::TIMEOUT) ||
            rrr::is_retryable_error(row.error) != row.retryable) {
            return 22;
        }
    }

    struct ErrorBoundaryRow {
        int code;
        std::string_view name;
        rrr::RpcErrorCategory category;
        bool connection;
        bool timeout;
        bool retryable;
    };
    constexpr ErrorBoundaryRow boundaries[] = {
        {99, "UNKNOWN", rrr::RpcErrorCategory::INTERNAL, false, false, false},
        {100, "NOT_CONNECTED", rrr::RpcErrorCategory::CONNECTION, true, false, false},
        {199, "UNKNOWN", rrr::RpcErrorCategory::CONNECTION, true, false, false},
        {200, "INVALID_MESSAGE", rrr::RpcErrorCategory::PROTOCOL, false, false, false},
        {399, "UNKNOWN", rrr::RpcErrorCategory::APPLICATION, false, false, false},
        {400, "CONNECT_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, false, true, true},
        {499, "UNKNOWN", rrr::RpcErrorCategory::TIMEOUT, false, true, false},
        {500, "UNKNOWN_ERROR", rrr::RpcErrorCategory::INTERNAL, false, false, false},
        {999, "UNKNOWN", rrr::RpcErrorCategory::INTERNAL, false, false, false},
    };
    for (const auto& row : boundaries) {
        const auto error = static_cast<rrr::RpcError>(row.code);
        if (rrr::rpc_error_to_string(error) != row.name ||
            rrr::get_error_category(error) != row.category ||
            rrr::is_connection_error(error) != row.connection ||
            rrr::is_timeout_error(error) != row.timeout ||
            rrr::is_retryable_error(error) != row.retryable) {
            return 23;
        }
    }

    auto metrics = rrr::ConnectionMetrics::new_();
    if (!metrics_are_reset(metrics) || metrics.uptime_ms(1234) != 0) {
        return 30;
    }
    metrics.record_request_dropped();
    if (metrics.in_flight_requests() != 0) {
        return 31;
    }
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_completed_with_latency(30);
    metrics.record_request_completed_with_latency(10);
    if (metrics.requests_sent() != 3 ||
        metrics.requests_completed() != 2 ||
        metrics.in_flight_requests() != 1 ||
        metrics.total_latency_us_field.load(
            rusty::sync::atomic::Ordering::Relaxed) != 40 ||
        metrics.min_latency_us() != 10 || metrics.max_latency_us() != 30 ||
        metrics.avg_latency_us() != 20 ||
        metrics.success_rate_percent() != 66) {
        return 32;
    }
    metrics.record_request_failed();
    metrics.record_request_timeout();
    metrics.record_request_dropped();
    if (metrics.requests_failed() != 1 ||
        metrics.requests_timed_out() != 1 ||
        metrics.in_flight_requests() != 0) {
        return 33;
    }
    metrics.record_request_completed();
    if (metrics.requests_completed() != 3 ||
        metrics.avg_latency_us() != 13 ||
        metrics.success_rate_percent() != 100 ||
        metrics.in_flight_requests() != 0) {
        return 34;
    }

    metrics.record_bytes_sent(11);
    metrics.record_bytes_sent(7);
    metrics.record_bytes_received(23);
    metrics.record_reconnect();
    metrics.record_retry_attempt();
    metrics.record_queue_drop();
    metrics.record_circuit_open_rejection();
    metrics.record_circuit_open_transition();
    metrics.record_circuit_half_open_transition();
    metrics.record_circuit_closed_transition();
    if (metrics.bytes_sent() != 18 || metrics.bytes_received() != 23 ||
        metrics.reconnect_count() != 1 || metrics.retry_attempts() != 1 ||
        metrics.queue_dropped_requests() != 1 ||
        metrics.circuit_open_rejections() != 1 ||
        metrics.circuit_open_transitions() != 1 ||
        metrics.circuit_half_open_transitions() != 1 ||
        metrics.circuit_closed_transitions() != 1) {
        return 35;
    }

    metrics.record_connect(1000);
    if (metrics.connect_time_ms() != 1000 || metrics.uptime_ms(999) != 0 ||
        metrics.uptime_ms(1000) != 0 || metrics.uptime_ms(1123) != 123) {
        return 36;
    }
    const auto metrics_snapshot = metrics;
    metrics.record_bytes_sent(1);
    if (metrics_snapshot.bytes_sent() != 18 || metrics.bytes_sent() != 19) {
        return 37;
    }
    metrics.reset();
    if (!metrics_are_reset(metrics)) {
        return 38;
    }
    metrics.bytes_sent_field.store(
        std::numeric_limits<std::uint64_t>::max(),
        rusty::sync::atomic::Ordering::Relaxed);
    metrics.record_bytes_sent(1);
    if (metrics.bytes_sent() != 0) {
        return 39;
    }
    metrics.requests_completed_field.store(
        std::numeric_limits<std::uint64_t>::max(),
        rusty::sync::atomic::Ordering::Relaxed);
    metrics.requests_sent_field.store(
        3, rusty::sync::atomic::Ordering::Relaxed);
    constexpr auto kWrappedPercent =
        (std::numeric_limits<std::uint64_t>::max() * std::uint64_t{100}) /
        std::uint64_t{3};
    if (metrics.success_rate_percent() != kWrappedPercent) {
        return 40;
    }
    if (!metrics_concurrent_updates_are_atomic()) {
        return 41;
    }
    return 0;
}
"""


def compile_module(
    clang: Path,
    root: Path,
    include: Path,
    source_dir: Path,
    work_dir: Path,
    module_name: str,
    cxx_flags: list[str],
) -> Path:
    source = source_dir / f"{module_name}.cppm"
    pcm = work_dir / f"{module_name}.pcm"
    object_file = work_dir / f"{module_name}.o"
    run(
        [
            str(clang),
            "-std=c++23",
            *cxx_flags,
            "-Wno-deprecated-declarations",
            "-I",
            str(include),
            f"-fprebuilt-module-path={work_dir}",
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
            *cxx_flags,
            f"-fprebuilt-module-path={work_dir}",
            "-c",
            str(pcm),
            "-o",
            str(object_file),
        ],
        root,
    )
    return object_file


def grouped_link_inputs(paths: list[Path]) -> list[str]:
    rendered = [str(path) for path in paths]
    if sys.platform.startswith("linux"):
        return ["-Wl,--start-group", *rendered, "-Wl,--end-group"]
    return rendered


def resolve_file(root: Path, raw: str, description: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        path = root / path
    path = path.resolve()
    if not path.is_file():
        raise GateError(f"{description} is unavailable: {path}")
    return path


def resolve_generated_dir(root: Path, raw: str) -> Path:
    output = Path(raw)
    if not output.is_absolute():
        output = root / output
    output = output.resolve()
    if not output.is_dir():
        raise GateError(f"generated crate directory is unavailable: {output}")
    return output


def check_generated_output(
    *,
    root: Path,
    output: Path,
    modules: list[extraction.ModuleEntry],
    clang: Path,
    nm: Path,
    reference: Path,
    production: Path | None,
    runtime_libraries: list[Path],
    cxx_flags: list[str],
    link_flags: list[str],
) -> None:
    require_cpp_surfaces(root, output, modules)
    require_zero_hand_slots(output / "rusty_hand_slots.md")
    include = root / "third-party/rusty-cpp/include"

    # Compilation products live outside the build-tree generation directory.
    # That directory remains a deterministic crate-output census shared by the
    # production target and this gate.
    with tempfile.TemporaryDirectory(prefix="rrr-crate-mode-compile-") as temporary:
        work = Path(temporary)
        generated_objects = [
            compile_module(
                clang,
                root,
                include,
                output,
                work,
                module.cpp_module,
                cxx_flags,
            )
            for module in modules
        ]
        # Compile the partial umbrella only after every child BMI exists. It is
        # a syntax/import-closure proof, not a production provider or link input.
        compile_module(
            clang,
            root,
            include,
            output,
            work,
            "rrr",
            cxx_flags,
        )

        importer = work / "importer.cpp"
        importer_object = work / "importer.o"
        importer.write_text(importer_source(), encoding="utf-8")
        run(
            [
                str(clang),
                "-std=c++23",
                *cxx_flags,
                "-I",
                str(include),
                f"-fprebuilt-module-path={work}",
                "-c",
                str(importer),
                "-o",
                str(importer_object),
            ],
            root,
        )

        link_sets: list[tuple[str, list[Path]]] = [
            ("generated", [*generated_objects, *runtime_libraries]),
            ("inline-reference", [reference, *runtime_libraries]),
        ]
        if production is not None:
            link_sets.append(
                ("production", [production, *runtime_libraries])
            )
        for label, link_inputs in link_sets:
            executable_path = work / f"importer-{label}"
            run(
                [
                    str(clang),
                    "-std=c++23",
                    *cxx_flags,
                    str(importer_object),
                    *grouped_link_inputs(link_inputs),
                    *link_flags,
                    "-o",
                    str(executable_path),
                ],
                root,
            )
            run([str(executable_path)], root)

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
                "independent inline reference library",
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
                    "the independent inline reference ABI"
                )

            if production is not None:
                production_symbols = module_symbols(
                    nm, root, production, module.cpp_module
                )
                require_expected_symbols(
                    module.cpp_module,
                    "production library",
                    production_symbols,
                )
                if production_symbols != reference_symbols:
                    raise GateError(
                        f"production {module.cpp_module} ABI differs from "
                        "the independent inline reference ABI"
                    )


def check(args: argparse.Namespace) -> None:
    root = repository_root()
    transpiler = executable(root, args.transpiler, "rusty-cpp transpiler")
    verify_pinned_toolchain(root, transpiler)
    require_extraction_check(root, transpiler)
    modules = load_owned_modules(root)
    clang = executable(root, args.clang, "Clang C++ compiler")
    nm = executable(root, args.nm, "nm")
    reference = resolve_file(
        root, args.reference_library, "independent inline reference library"
    )
    production_raw = getattr(args, "production_library", None)
    production = (
        resolve_file(root, production_raw, "production library")
        if production_raw
        else None
    )
    if production is not None and production == reference:
        raise GateError(
            "production library and independent inline reference library "
            "must be different artifacts"
        )
    runtime_libraries = [
        resolve_file(root, raw, "runtime library")
        for raw in (getattr(args, "runtime_library", None) or [])
    ]
    cxx_flags = list(getattr(args, "cxx_flag", None) or [])
    link_flags = list(getattr(args, "link_flag", None) or [])

    generated_raw = getattr(args, "generated_dir", None)
    if generated_raw:
        output = resolve_generated_dir(root, generated_raw)
        check_generated_output(
            root=root,
            output=output,
            modules=modules,
            clang=clang,
            nm=nm,
            reference=reference,
            production=production,
            runtime_libraries=runtime_libraries,
            cxx_flags=cxx_flags,
            link_flags=link_flags,
        )
    else:
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
                    "--module-preamble",
                    MODULE_PREAMBLE,
                ],
                root,
            )
            check_generated_output(
                root=root,
                output=output,
                modules=modules,
                clang=clang,
                nm=nm,
                reference=reference,
                production=production,
                runtime_libraries=runtime_libraries,
                cxx_flags=cxx_flags,
                link_flags=link_flags,
            )

    symbol_count = sum(len(spec.symbols) for spec in ABI_SPECS.values())
    production_label = (
        " and production libraries" if production is not None else ""
    )
    print(
        f"checked whole rrr crate ({len(modules) + 1} modules compiled, "
        "partial root compile-only, 0 hand slots), combined importer against generated "
        f"objects and the independent inline reference{production_label}, "
        "AvgStat, RpcError, and ConnectionMetrics layout/concurrent/wrapping "
        "runtime contracts, and "
        f"{symbol_count} exact strong ABI symbols"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--reference-library",
        "--reference-object",
        dest="reference_library",
        required=True,
        help=(
            "independently compiled inline-carrier archive "
            "(legacy --reference-object is accepted)"
        ),
    )
    parser.add_argument(
        "--production-library",
        help="production librrr archive to link/run and compare against the oracle",
    )
    parser.add_argument(
        "--generated-dir",
        help=(
            "pre-generated rusty-cpp crate output directory; when omitted, "
            "the gate generates into a temporary directory"
        ),
    )
    parser.add_argument(
        "--runtime-library",
        action="append",
        default=[],
        help=(
            "support archive appended to every link lane; may be repeated"
        ),
    )
    parser.add_argument(
        "--cxx-flag",
        action="append",
        default=[],
        help="compiler-driver flag used for module compilation and linking",
    )
    parser.add_argument(
        "--link-flag",
        action="append",
        default=[],
        help="additional flag appended to every link command",
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
