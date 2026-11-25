#!/usr/bin/env python3
"""Check the RocksDB-only distributed runtime dependency graph and linked binaries."""
import argparse
import json
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
    forbidden = ("sto-tpcc-ffi", "sto-masstree", "mako-storage-masstree", "mako-mtree-sys")
    for package in forbidden:
        if package in tree:
            raise RuntimeError(f"Masstree dependency in RocksDB-only runtime: {package}")
    programs = ("mako_node", "mako_admin", "mako_client", "mako_bench", "mako_transactions", "mako_redis", "mako_convert")
    binaries = {}
    for name in programs:
        path = args.target_dir.resolve() / args.profile / name
        symbols = subprocess.check_output(["nm", "--defined-only", "--demangle", str(path)], text=True)
        for token in ("sto_masstree::", "mako_storage_masstree::", "sto_tpcc_ffi::", "mako_mtree_", "mako_sto_", "Masstree::", "janus::", "TThread::"):
            if token in symbols:
                raise RuntimeError(f"Forbidden backend/application symbol in {name}: {token}")
        libraries = subprocess.check_output(["ldd", str(path)], text=True)
        if name == "mako_node" and "librocksdb" not in libraries:
            raise RuntimeError("RocksDB runtime does not link librocksdb")
        binaries[name] = {"path": str(path), "bytes": path.stat().st_size, "dynamic_libraries": libraries.splitlines()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"passed": True, "dependency_tree": tree.splitlines(), "binaries": binaries}, indent=2) + "\n")
    print(f"RocksDB-only graph and {len(binaries)} executable symbol audits passed")


if __name__ == "__main__":
    main()
