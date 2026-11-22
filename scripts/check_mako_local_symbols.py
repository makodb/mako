#!/usr/bin/env python3
"""Verify the exact strong C-symbol surface of a libmako static archive."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable, Mapping, Sequence


PUBLIC_SYMBOL = re.compile(r"^mako_local_[A-Za-z0-9_]+$")
FINGERPRINT = re.compile(r"^[0-9a-f]{64}$")
NM_SYMBOL = re.compile(
    r"(?:^|\s)(?P<name>_?mako_local_[A-Za-z0-9_]+)"
    r"\s+(?P<kind>[A-Za-z?])(?:\s|$)"
)
BUILD_ANCHOR_PREFIX = "mako_local_build_anchor_"


class GateError(RuntimeError):
    """A deterministic ABI-gate failure."""


def load_allowlist(path: Path) -> list[str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise GateError(f"cannot read allowlist {path}: {error}") from error

    symbols: list[str] = []
    for line_number, line in enumerate(lines, 1):
        symbol = line.split("#", 1)[0].strip()
        if not symbol:
            continue
        if not PUBLIC_SYMBOL.fullmatch(symbol):
            raise GateError(
                f"{path}:{line_number}: invalid mako-local symbol {symbol!r}"
            )
        if symbol.startswith(BUILD_ANCHOR_PREFIX):
            raise GateError(
                f"{path}:{line_number}: the hash-named build anchor is checked "
                "from the manifest and must not be allowlisted"
            )
        symbols.append(symbol)

    if not symbols:
        raise GateError(f"allowlist {path} is empty")
    duplicates = sorted({symbol for symbol in symbols if symbols.count(symbol) > 1})
    if duplicates:
        raise GateError(f"allowlist {path} contains duplicates: {duplicates}")
    if symbols != sorted(symbols):
        raise GateError(f"allowlist {path} must be sorted lexicographically")
    return symbols


def load_fingerprint(path: Path) -> str:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise GateError(f"cannot read build manifest {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise GateError(f"invalid JSON in build manifest {path}: {error}") from error

    if not isinstance(document, dict):
        raise GateError(f"build manifest {path} must contain a top-level object")
    fingerprint = document.get("fingerprint")
    if not isinstance(fingerprint, str) or not FINGERPRINT.fullmatch(fingerprint):
        raise GateError(
            f"build manifest {path} field 'fingerprint' must be exactly "
            "64 lowercase hexadecimal characters"
        )
    return fingerprint


def parse_nm_output(output: str) -> dict[str, list[str]]:
    """Return normalized mako_local symbol names and all definition kinds."""

    definitions: dict[str, list[str]] = defaultdict(list)
    for line in output.splitlines():
        match = NM_SYMBOL.search(line)
        if match is None:
            continue
        name = match.group("name")
        if name.startswith("_mako_local_"):
            # Mach-O presents C external names with one leading underscore.
            name = name[1:]
        definitions[name].append(match.group("kind"))
    return dict(definitions)


def validate_symbols(
    definitions: Mapping[str, Sequence[str]],
    public_symbols: Iterable[str],
    fingerprint: str,
) -> list[str]:
    expected_public = set(public_symbols)
    expected_anchor = BUILD_ANCHOR_PREFIX + fingerprint
    expected = expected_public | {expected_anchor}
    actual = set(definitions)
    errors: list[str] = []

    missing = sorted(expected - actual)
    if missing:
        errors.append("missing symbols: " + ", ".join(missing))

    unexpected = sorted(actual - expected)
    if unexpected:
        errors.append("unexpected symbols: " + ", ".join(unexpected))

    for symbol in sorted(actual & expected):
        kinds = list(definitions[symbol])
        if kinds != ["T"]:
            rendered = ", ".join(kinds) if kinds else "no definitions"
            errors.append(
                f"{symbol} must have exactly one strong text definition (T), "
                f"found: {rendered}"
            )

    hash_named_anchors = sorted(
        symbol
        for symbol in actual
        if symbol.startswith(BUILD_ANCHOR_PREFIX)
        and FINGERPRINT.fullmatch(symbol.removeprefix(BUILD_ANCHOR_PREFIX))
    )
    if hash_named_anchors != [expected_anchor]:
        errors.append(
            "archive must contain exactly the manifest-selected build anchor "
            f"{expected_anchor}; found: {hash_named_anchors}"
        )
    return errors


def run_nm(nm: str, archive: Path) -> str:
    if not archive.is_file():
        raise GateError(f"archive does not exist: {archive}")

    attempts = [
        [nm, "-P", "-g", "-U", str(archive)],
        [nm, "-P", "-g", "--defined-only", str(archive)],
    ]
    failures: list[str] = []
    for command in attempts:
        try:
            result = subprocess.run(
                command,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except OSError as error:
            raise GateError(f"cannot execute nm tool {nm!r}: {error}") from error
        if result.returncode == 0:
            return result.stdout
        failures.append(
            f"{' '.join(command)} exited {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    raise GateError("nm could not inspect the archive:\n  " + "\n  ".join(failures))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True, help="toolchain nm executable")
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--allowlist", required=True, type=Path)
    parser.add_argument(
        "--manifest",
        required=True,
        type=Path,
        help="generated JSON manifest containing the exact SHA-256 fingerprint",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        public_symbols = load_allowlist(args.allowlist)
        fingerprint = load_fingerprint(args.manifest)
        definitions = parse_nm_output(run_nm(args.nm, args.archive))
        errors = validate_symbols(definitions, public_symbols, fingerprint)
    except GateError as error:
        print(f"mako-local symbol gate: {error}", file=sys.stderr)
        return 2

    if errors:
        print("mako-local symbol gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(
        "mako-local symbol gate passed: "
        f"{len(public_symbols)} public symbols and one SHA-256 build anchor"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
