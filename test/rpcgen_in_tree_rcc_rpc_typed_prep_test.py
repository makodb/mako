#!/usr/bin/env python3

import argparse
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Probe:
    name: str
    subsystem: str
    expected_status: str
    source: str


@dataclass
class ProbeResult:
    probe: Probe
    status: str
    stderr_excerpt: str
    stderr_full: str


def run_rpcgen(repo_root: Path, rpc_path: Path) -> None:
    cmd = [
        str(repo_root / "bin/rpcgen"),
        "--cpp",
        "--python",
        "--cpp-mode",
        "typed",
        str(rpc_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    if proc.returncode != 0:
        raise RuntimeError(
            "rpcgen failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )


def require_contains(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing expected snippet:\n{needle}\n")


def verify_typed_header_shape(header_text: str) -> None:
    require_contains(header_text, "// rpcgen cpp mode: typed")
    require_contains(header_text, "class ClassicService: public rrr::Service")
    require_contains(header_text, "class ClassicProxy")
    require_contains(header_text, "class ClientControlService: public rrr::Service")
    require_contains(header_text, "class ClientControlProxy")
    require_contains(header_text, "struct RpcDispatchTxnRequest")
    require_contains(header_text, "struct RpcDispatchTxnResponse")
    require_contains(
        header_text,
        "rusty::Result<DispatchTxnTypedFuture, rrr::i32> async_DispatchTxn",
    )


def compile_probe(
    repo_root: Path, generated_dir: Path, cxx: str, probe: Probe
) -> ProbeResult:
    source_path = generated_dir / f"{probe.name}_probe.cc"
    source_path.write_text(probe.source, encoding="utf-8")

    include_root = repo_root / "third-party/rusty-cpp/include"
    if not include_root.exists():
        raise RuntimeError(f"missing include path (submodule not initialized): {include_root}")

    cmd = [
        cxx,
        "-std=c++23",
        "-fsyntax-only",
        "-I",
        str(generated_dir),
        "-I",
        str(repo_root),
        "-I",
        str(repo_root / "src"),
        "-I",
        str(repo_root / "src/rrr"),
        "-I",
        str(repo_root / "src/deptran"),
        "-I",
        str(include_root),
        str(source_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    stderr_excerpt = "\n".join(proc.stderr.splitlines()[:80])
    status = "pass" if proc.returncode == 0 else "fail"
    return ProbeResult(
        probe=probe,
        status=status,
        stderr_excerpt=stderr_excerpt,
        stderr_full=proc.stderr,
    )


def verify_results(results: list[ProbeResult]) -> None:
    mismatches: list[str] = []
    for result in results:
        if result.status != result.probe.expected_status:
            mismatches.append(
                f"{result.probe.name}: expected {result.probe.expected_status}, got {result.status}"
            )
    if mismatches:
        details = "\n".join(mismatches)
        raise AssertionError(f"rcc_rpc typed prep inventory status drifted:\n{details}")

    # Current service fallout is expected in prep stage; keep an explicit marker
    # so we notice if the failure shifts away from rcc_rpc typed bridge issues.
    service = next((r for r in results if r.probe.name == "service"), None)
    if service is None:
        raise AssertionError("missing service probe result")
    if service.status != "fail":
        raise AssertionError("service probe unexpectedly passed; update prep inventory/migration leaf")
    service_markers = ["TxReply", "DispatchTxnTypedFuture", "marked 'override', but does not override"]
    if not any(marker in service.stderr_full for marker in service_markers):
        raise AssertionError(
            "service probe failed, but expected typed-fallout markers were not found in stderr excerpt"
        )


def print_summary(results: list[ProbeResult]) -> None:
    print("rcc_rpc typed prep inventory (subsystem compile probes)")
    print("subsystem,name,status")
    for result in results:
        print(f"{result.probe.subsystem},{result.probe.name},{result.status}")
    failing = [r for r in results if r.status == "fail"]
    if failing:
        print("\nfirst failing probe stderr excerpt:")
        print(failing[0].stderr_excerpt[:3000])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Generate typed rcc_rpc in temp space and capture compile fallout "
            "inventory for prep-phase subsystem probes"
        )
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    rpc_path = repo_root / "src/deptran/rcc_rpc.rpc"
    if not rpc_path.exists():
        raise RuntimeError(f"missing rpc source: {rpc_path}")

    probes = [
        Probe(
            name="service",
            subsystem="service",
            expected_status="fail",
            source='#include "src/deptran/service.h"\nint main() { return 0; }\n',
        ),
        Probe(
            name="communicator",
            subsystem="communicator",
            expected_status="pass",
            source='#include "src/deptran/communicator.h"\nint main() { return 0; }\n',
        ),
        Probe(
            name="config_control",
            subsystem="config_control",
            expected_status="pass",
            source=(
                '#include "src/deptran/config_service.h"\n'
                '#include "src/deptran/config_client.h"\n'
                '#include "src/deptran/benchmark_control_rpc.h"\n'
                "int main() { return 0; }\n"
            ),
        ),
    ]

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)
        tmp_rpc = tmpdir_path / "rcc_rpc.rpc"
        tmp_rpc.write_text(rpc_path.read_text(encoding="utf-8"), encoding="utf-8")
        run_rpcgen(repo_root, tmp_rpc)

        generated_header_path = tmp_rpc.with_suffix(".h")
        if not generated_header_path.exists():
            raise RuntimeError(f"missing generated header: {generated_header_path}")
        generated_header = generated_header_path.read_text(encoding="utf-8")
        verify_typed_header_shape(generated_header)

        results = [compile_probe(repo_root, tmpdir_path, args.cxx, probe) for probe in probes]
        verify_results(results)
        print_summary(results)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
