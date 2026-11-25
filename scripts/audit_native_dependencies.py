#!/usr/bin/env python3
"""Audit Cargo's production links, native archives and compiler header closure."""

import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    evidence = build / "native-dependency-audit"
    evidence.mkdir(exist_ok=True)
    report = {"passed": False, "programs": {}, "native_libraries": []}
    allowed_libs = {"mako_native_storage", "srpc_native", "rocksdb", "c++", "c++abi",
                    "numa", "pthread", "jemalloc", "tcmalloc", "lz4", "dl", "m", "rt"}
    allowed_mako = {line for line in (root / "cmake/native-storage-headers.txt").read_text().splitlines()
                    if line and not line.startswith("#")}
    dependencies = subprocess.check_output(["ninja", "-C", str(build), "-t", "deps"], text=True)
    headers = set()
    for line in dependencies.splitlines():
        if not line.startswith("    "):
            continue
        path = Path(line.strip())
        if not path.is_absolute():
            path = build / path
        path = path.resolve()
        if path.is_relative_to(root):
            relative = path.relative_to(root).as_posix()
            if relative.startswith("src/"):
                if not relative.startswith("src/masstree/") and relative not in allowed_mako:
                    raise RuntimeError(f"non-storage compiler dependency: {relative}")
                headers.add(relative)
    if "src/mako/storage/mtree_abi.h" not in headers:
        raise RuntimeError("native compiler dependency records are missing")
    report["native_compiler_dependencies"] = sorted(headers)
    (evidence / "native-compiler-dependencies.txt").write_text(dependencies)
    observed = set()
    checked_rlibs = set()
    # Link every production entry point. Cargo emits build-script records even
    # for fresh artifacts. A separate map records each executable's actual link.
    programs = [("mako-runtime", name, ["--features", "replication"])
                for name in ("mako_node", "mako_bench", "mako_transactions", "mako_redis")]
    programs += [("mako-runtime", "mako_convert", ["--features", "logging"])]
    programs += [("mako-runtime", name, []) for name in ("mako_client", "mako_admin")]
    programs += [("mako-cache-bench", name, [])
                 for name in ("mako-cache-bench", "engine_compare", "gc_soak")]
    programs += [("rocksdb-raft-service", name, [])
                 for name in ("rocksdb_raft_server", "rocksdb_raft_client", "rocksdb_raft_bench")]
    try:
        for package, name, features in programs:
            link_map = evidence / f"{name}.map"
            command = [args.cargo, "rustc", "--locked", "--manifest-path", str(root / "crates/Cargo.toml"),
                       "--release", "--package", package, "--bin", name, *features,
                       "--message-format=json", "--", f"-Clink-arg=-Wl,-Map={link_map}"]
            result = subprocess.run(command, stdout=subprocess.PIPE, text=True, check=False)
            (evidence / f"{name}.jsonl").write_text(result.stdout)
            if result.returncode:
                raise RuntimeError(f"Cargo failed linking {name}")
            executable = None
            for line in result.stdout.splitlines():
                event = json.loads(line)
                if event.get("reason") == "build-script-executed":
                    for library in event.get("linked_libs", []):
                        native = library.rsplit("=", 1)[-1]
                        if native not in allowed_libs:
                            raise RuntimeError(f"unreviewed Cargo native library: {library}")
                        observed.add(native)
                if event.get("reason") != "compiler-artifact":
                    continue
                if event.get("executable") and event["target"]["name"] == name:
                    executable = event["executable"]
                manifest = Path(event["manifest_path"])
                if (not manifest.is_relative_to(root)
                        or manifest.relative_to(root).parts[0] not in {"src", "crates"}
                        or "third-party" in manifest.parts):
                    continue
                for filename in event.get("filenames", []):
                    if not filename.endswith(".rlib") or filename in checked_rlibs:
                        continue
                    members = subprocess.check_output(["ar", "t", filename], text=True).splitlines()
                    unexpected = [member for member in members
                                  if member not in {"lib.rmeta", "lib.rmeta-link"}
                                  and not member.endswith(".rcgu.o")]
                    if unexpected:
                        raise RuntimeError(f"bundled native objects in {filename}: {unexpected}")
                    checked_rlibs.add(filename)
            if not executable or not link_map.is_file():
                raise RuntimeError(f"missing executable or link map: {name}")
            contents = link_map.read_text()
            archives = set(re.findall(r"[^\s()]+\.a(?=\(|\s|$)", contents))
            project_archives = []
            for archive in archives:
                path = Path(archive)
                if path.is_absolute() and path.is_relative_to(root):
                    if path.name not in {"libmako_native_storage.a", "libsrpc_native.a"}:
                        raise RuntimeError(f"non-storage application archive in {name}: {path}")
                    project_archives.append(str(path))
            if name in {"mako_node", "mako_bench", "mako_transactions", "mako_redis"}:
                if not any(Path(path).name == "libmako_native_storage.a" for path in project_archives):
                    raise RuntimeError(f"{name} did not link the audited storage archive")
            elif name in {"mako_client", "mako_admin", "mako_convert"}:
                if "libmako_native_storage.a" in contents:
                    raise RuntimeError(f"{name} unexpectedly links native storage")
            symbols = subprocess.check_output([args.nm, "--defined-only", "--demangle", executable], text=True)
            # Rust's srpc:: names are expected; C++ application owners are not.
            for symbol in ("janus::", "TThread::", "ShardClient::", "ShardReceiver::", "rust_sto_tpcc_wrapper::"):
                if symbol in symbols:
                    raise RuntimeError(f"retired C++ symbol in {name}: {symbol}")
            report["programs"][name] = {"executable": executable, "project_archives": sorted(project_archives)}
        if not {"mako_native_storage", "srpc_native", "rocksdb"} <= observed:
            raise RuntimeError("incomplete native dependency inventory")
        report["native_libraries"] = sorted(observed)
        report["rust_archives_checked"] = len(checked_rlibs)
        report["passed"] = True
    finally:
        (evidence / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Native dependency audit passed: {len(programs)} production links, {len(checked_rlibs)} Rust archives")


if __name__ == "__main__":
    main()
