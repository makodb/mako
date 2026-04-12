#!/usr/bin/env python3

import argparse
from pathlib import Path


REQUIRED_SNIPPETS = [
    "# rcc_rpc Typed Fallout Map",
    "## Evidence Inputs",
    "## Current Subsystem Status",
    "## Fallout Taxonomy (Service Subsystem)",
    "## File-Level Migration Map",
    "Leaf 3b",
    "Leaf 3c",
    "SimpleCommand",
    "parent_set_t",
    "test_rpc_rpcgen_in_tree_rcc_rpc_typed_prep",
    "test_rpc_rpcgen_in_tree_rcc_rpc_typed_sync",
    "src/deptran/service.h",
    "src/deptran/service.cc",
    "src/deptran/communicator.h",
    "src/deptran/communicator.cc",
]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Validate that rcc_rpc typed fallout map doc exists and contains "
            "the required migration buckets and leaf mapping anchors"
        )
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    map_path = repo_root / "docs/rpc/rcc_rpc_typed_fallout_map.md"
    if not map_path.exists():
        raise RuntimeError(f"missing fallout map doc: {map_path}")

    text = map_path.read_text(encoding="utf-8")
    missing = [snippet for snippet in REQUIRED_SNIPPETS if snippet not in text]
    if missing:
        missing_rendered = "\n".join(f"- {snippet}" for snippet in missing)
        raise AssertionError(
            "rcc_rpc typed fallout map doc is missing required anchors:\n"
            f"{missing_rendered}"
        )

    print("rcc_rpc typed fallout map doc anchors verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
