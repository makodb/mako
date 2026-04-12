#!/usr/bin/env python3

import argparse
import re
from pathlib import Path


def require_regex(text: str, pattern: str, explanation: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise AssertionError(f"missing expected CMake wiring: {explanation}\npattern: {pattern}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate CMake rpcgen --cpp-mode wiring.")
    parser.add_argument("--repo", required=True, help="Repository root path")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    cmake_path = repo_root / "CMakeLists.txt"
    if not cmake_path.exists():
        raise RuntimeError(f"missing CMakeLists.txt: {cmake_path}")

    cmake_text = cmake_path.read_text(encoding="utf-8")

    require_regex(
        cmake_text,
        r'set\(RPCGEN_CPP_MODE\s+"typed"\s+CACHE STRING\s+"rpcgen C\+\+ mode',
        "RPCGEN_CPP_MODE cache option with typed default",
    )
    require_regex(
        cmake_text,
        r'set_property\(CACHE RPCGEN_CPP_MODE PROPERTY STRINGS "typed" "compat"\)',
        "RPCGEN_CPP_MODE allowed value list",
    )
    require_regex(
        cmake_text,
        r'COMMAND\s+\$\{CMAKE_CURRENT_SOURCE_DIR\}/bin/rpcgen --cpp --python --cpp-mode \$\{RPCGEN_CPP_MODE\} src/deptran/rcc_rpc\.rpc',
        "rcc_rpc generator command passes --cpp-mode",
    )
    require_regex(
        cmake_text,
        r'DEPENDS\s+\$\{CMAKE_CURRENT_SOURCE_DIR\}/src/deptran/rcc_rpc\.rpc\s+\$\{RCC_RPCGEN_CPP_MODE_FILE\}',
        "rcc_rpc generation depends on mode marker file",
    )
    require_regex(
        cmake_text,
        r'file\(GENERATE OUTPUT "\$\{RCC_RPCGEN_CPP_MODE_FILE\}" CONTENT "\$\{RPCGEN_CPP_MODE\}\\n"\)',
        "mode marker file generation for rebuild-on-mode-change behavior",
    )

    print("CMake rpcgen --cpp-mode wiring verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
