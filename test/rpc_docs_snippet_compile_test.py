#!/usr/bin/env python3

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def extract_tagged_cpp_snippets(book_text: str):
    snippets = []
    lines = book_text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("```cpp") and "srpc-compile" in line:
            start = i + 1
            j = start
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            if j >= len(lines):
                raise RuntimeError(f"unterminated cpp fence starting near line {i + 1}")
            snippet = "\n".join(lines[start:j]).strip()
            snippets.append((i + 1, snippet))
            i = j + 1
            continue
        i += 1
    return snippets


def compile_snippet(cxx: str, repo_root: Path, idx: int, line_no: int, snippet: str):
    unit = f"""#include <time.h>

#include "src/rrr/rpc/reconnect_policy.hpp"
#include "src/rrr/rpc/circuit_breaker.hpp"
#include "src/rrr/rpc/heartbeat.hpp"

using namespace rrr;

void snippet_{idx}() {{
{snippet}
}}

int main() {{
    snippet_{idx}();
    return 0;
}}
"""

    with tempfile.NamedTemporaryFile("w", suffix=".cc", delete=False) as f:
        path = Path(f.name)
        f.write(unit)

    cmd = [
        cxx,
        "-std=c++23",
        "-fsyntax-only",
        "-I",
        str(repo_root),
        "-I",
        str(repo_root / "third-party/rusty-cpp/include"),
        str(path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    path.unlink(missing_ok=True)
    if proc.returncode != 0:
        return (
            False,
            f"snippet tagged at line {line_no} failed to compile\n"
            f"command: {' '.join(cmd)}\n"
            f"{proc.stdout}{proc.stderr}",
        )
    return True, ""


def main():
    parser = argparse.ArgumentParser(description="Compile srpc-book tagged cpp snippets.")
    parser.add_argument("--book", required=True, help="Path to docs/srpc-book.md")
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    parser.add_argument("--min-snippets", type=int, default=1, help="Minimum required tagged snippets")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    book_path = Path(args.book).resolve()

    if not book_path.exists():
        print(f"book not found: {book_path}", file=sys.stderr)
        return 2

    rusty_include = repo_root / "third-party/rusty-cpp/include/rusty"
    if not rusty_include.exists():
        print(
            f"missing Rusty C++ headers at {rusty_include}; run submodule update before this test",
            file=sys.stderr,
        )
        return 2

    snippets = extract_tagged_cpp_snippets(book_path.read_text(encoding="utf-8"))
    if len(snippets) < args.min_snippets:
        print(
            f"expected at least {args.min_snippets} tagged snippets, found {len(snippets)}",
            file=sys.stderr,
        )
        return 2

    failures = []
    for idx, (line_no, snippet) in enumerate(snippets, start=1):
        ok, message = compile_snippet(args.cxx, repo_root, idx, line_no, snippet)
        if not ok:
            failures.append(message)

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        return 1

    print(f"compiled {len(snippets)} tagged srpc-book snippets successfully")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
