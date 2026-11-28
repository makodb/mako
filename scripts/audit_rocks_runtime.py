#!/usr/bin/env python3
"""Check the RocksDB-only distributed runtime dependency graph and linked binaries."""
import argparse
import json
import re
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--target-dir", type=Path, required=True)
    parser.add_argument("--profile", choices=("debug", "release"), default="release")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    tree = subprocess.check_output([
        args.cargo, "tree", "--locked", "--manifest-path", str(root / "crates/Cargo.toml"),
        "--package", "mako-runtime", "--no-default-features", "--features", "logging,rocksdb-engine",
        "--edges", "normal,build", "--prefix", "none", "--format", "{p}",
    ], text=True)
    forbidden = ("sto-masstree-api", "sto-masstree", "mako-storage-masstree", "mako-mtree-sys")
    api_tree = subprocess.check_output([
        args.cargo, "tree", "--locked", "--manifest-path", str(root / "crates/Cargo.toml"),
        "--package", "mako-rocksdb", "--no-default-features", "--features", "rocksdb-engine",
        "--edges", "normal,build", "--prefix", "none", "--format", "{p}",
    ], text=True)
    workload_tree = subprocess.check_output([
        args.cargo, "tree", "--locked", "--manifest-path", str(root / "crates/Cargo.toml"),
        "--package", "mako-workloads", "--no-default-features", "--features", "logging,rocksdb-engine",
        "--edges", "normal,build", "--prefix", "none", "--format", "{p}",
    ], text=True)
    for package in forbidden:
        if any(package in t for t in (tree, api_tree, workload_tree)):
            raise RuntimeError(f"Masstree dependency in RocksDB-only runtime: {package}")
    for dependency_tree in (tree, api_tree, workload_tree):
        for package, version in (("rocksdb", "0.25.0"), ("librocksdb-sys", "0.19.0+11.8.1")):
            versions = set(re.findall(rf"^{package} v([^\s]+)", dependency_tree, re.MULTILINE))
            if versions != {version}:
                raise RuntimeError(f"expected one pinned {package}, found {versions}")
    programs = ("mako_node", "mako_admin", "mako_client", "mako_bench", "mako_transactions", "mako_redis", "examples/rocksdb_transactions")
    binaries = {}
    for name in programs:
        path = args.target_dir.resolve() / args.profile / name
        symbols = subprocess.check_output(["nm", "--defined-only", "--demangle", str(path)], text=True)
        for token in ("sto_masstree::", "mako_storage_masstree::", "sto_masstree_api::", "mako_mtree_", "mako_sto_", "Masstree::", "janus::", "TThread::"):
            if token in symbols:
                raise RuntimeError(f"Forbidden backend/application symbol in {name}: {token}")
        libraries = subprocess.check_output(["ldd", str(path)], text=True)
        static_rocksdb = bool(re.search(r"\b[Tt] rocksdb_open$", symbols, re.MULTILINE))
        dynamic_rocksdb = "librocksdb" in libraries
        if static_rocksdb and dynamic_rocksdb:
            raise RuntimeError(f"duplicate bundled/system RocksDB in {name}")
        if name == "mako_node" and not (static_rocksdb or dynamic_rocksdb):
            raise RuntimeError("RocksDB runtime lacks the native engine")
        binaries[name] = {"path": str(path), "bytes": path.stat().st_size, "dynamic_libraries": libraries.splitlines()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"passed": True, "dependency_tree": tree.splitlines(), "api_dependency_tree": api_tree.splitlines(), "workload_dependency_tree": workload_tree.splitlines(), "binaries": binaries}, indent=2) + "\n")
    print(f"RocksDB-only graph and {len(binaries)} executable symbol audits passed")


if __name__ == "__main__":
    main()
